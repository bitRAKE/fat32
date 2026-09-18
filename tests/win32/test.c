#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "api.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); ExitProcess(1); } } while (0)
#define OK(x) do { int s_ = (x); if (s_) { fprintf(stderr, "FAIL %s:%d: %s -> %d\n", __FILE__, __LINE__, #x, s_); ExitProcess(1); } } while (0)
#define U(s) ((const uint16_t *)L##s)
typedef struct Page { struct Page *next; uint64_t lba; unsigned char data[4096]; } Page;
typedef struct Image {
    SectorOps ops;
    Page *bucket[4096];
    uint32_t bytes, spc, fats, fat_sectors, data, clusters;
    int fail_read, fail_write, fail_flush;
    uint64_t reads, writes;
} Image;
typedef struct Fixture {
    Image disk; SectorBuffer buffer; FatIdentity id; SectorOps fault, inner;
    int fail_stage; uint64_t provider_reads, fat_reads; FatWorkspace workspace;
    unsigned char workspace_data[3*4096];
} Fixture;
/* ABI shims poison home space and volatile GPRs after provider callbacks. */
extern int abi_read(void *,uint64_t,void *);
extern int abi_write(void *,uint64_t,const void *);
extern int abi_begin(void *);
extern void abi_end(void *,int);
typedef struct AbiCall { uintptr_t target,args[4]; int result; } AbiCall;
extern unsigned abi_probe(AbiCall *);
extern int abi_home_probe(void);
_Static_assert(offsetof(AbiCall,result)==40,"ABI probe result");
static int checked_call(uintptr_t target,uintptr_t a,uintptr_t b,uintptr_t c,uintptr_t d) {
    AbiCall call={target,{a,b,c,d},-1}; unsigned mask=abi_probe(&call);
    if(mask) { fprintf(stderr,"ABI register/home corruption: mask=%03X\n",mask); ExitProcess(1); }
    return call.result;
}
#define ABI(fn,a,b,c,d) checked_call((uintptr_t)(fn),(uintptr_t)(a),(uintptr_t)(b),(uintptr_t)(c),(uintptr_t)(d))
static unsigned tests;
static uint16_t rd16(const void *v) { const unsigned char *p=v; return (uint16_t)(p[0] | p[1]<<8); }
static uint32_t rd32(const void *v) { const unsigned char *p=v; return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
static void wr16(void *v, unsigned x) { unsigned char *p=v; p[0]=(unsigned char)x; p[1]=(unsigned char)(x>>8); }
static void wr32(void *v, uint32_t x) { unsigned char *p=v; unsigned i; for(i=0;i<4;i++) p[i]=(unsigned char)(x>>(i*8)); }
static Page *page(Image *d, uint64_t lba, int create) {
    unsigned b=(unsigned)(lba%4096); Page *p=d->bucket[b];
    while(p && p->lba!=lba) p=p->next;
    if(!p && create) { p=calloc(1,sizeof(*p)); CHECK(p); p->lba=lba; p->next=d->bucket[b]; d->bucket[b]=p; }
    return p;
}
static int read_image(void *v,uint64_t lba,void *out) {
    Image *d=v; Page *p; ++d->reads;
    if(d->fail_read==0) return F_IO;
    if(d->fail_read>0) --d->fail_read;
    if(lba>=d->ops.sectors) return F_RANGE;
    p=page(d,lba,0); if(p) memcpy(out,p->data,d->bytes); else memset(out,0,d->bytes);
    return 0;
}
static int write_image(void *v,uint64_t lba,const void *in) {
    Image *d=v; ++d->writes;
    if(d->fail_write==0) return F_IO;
    if(d->fail_write>0) --d->fail_write;
    if(lba>=d->ops.sectors) return F_RANGE;
    memcpy(page(d,lba,1)->data,in,d->bytes); return 0;
}
static int flush_image(void *v) { return ((Image *)v)->fail_flush?F_IO:0; }
static void fat_value(Image *d, unsigned copy, uint32_t c, uint32_t value) {
    uint64_t lba=32+(uint64_t)copy*d->fat_sectors+(uint64_t)c*4/d->bytes;
    wr32(page(d,lba,1)->data+(c*4%d->bytes),value);
}
static void init_image(Image *d,unsigned bytes,unsigned spc) {
    unsigned c; unsigned char *p;
    memset(d,0,sizeof(*d)); d->bytes=bytes; d->spc=spc; d->fats=2; d->clusters=65530;
    d->fat_sectors=(d->clusters+2)*4/bytes+1; d->data=32+d->fats*d->fat_sectors;
    d->ops.context=d; d->ops.read=read_image; d->ops.write=write_image; d->ops.flush=flush_image;
    d->ops.sector_bytes=bytes; d->ops.sectors=d->data+(uint64_t)d->clusters*spc;
    d->fail_read=d->fail_write=-1;
    p=page(d,0,1)->data; p[0]=0xEB; p[1]=0x58; p[2]=0x90; memcpy(p+3,"TESTFAT ",8);
    wr16(p+11,bytes); p[13]=(unsigned char)spc; wr16(p+14,32); p[16]=2; p[21]=0xF8;
    wr32(p+32,(uint32_t)d->ops.sectors); wr32(p+36,d->fat_sectors); wr32(p+44,2);
    wr16(p+48,1); wr16(p+50,6); p[66]=0x29; wr32(p+67,0x12345678);
    memcpy(p+71,"TESTING    ",11); memcpy(p+82,"FAT32   ",8); wr16(p+510,0xAA55);
    memcpy(page(d,6,1)->data,p,bytes);
    p=page(d,1,1)->data; wr32(p,0x41615252); wr32(p+484,0x61417272);
    wr32(p+488,d->clusters-1); wr32(p+492,3); wr32(p+508,0xAA550000);
    memcpy(page(d,7,1)->data,p,bytes);
    for(c=0;c<2;c++) { fat_value(d,c,0,0x0FFFFFF8); fat_value(d,c,1,0x0FFFFFFF); fat_value(d,c,2,0x0FFFFFFF); }
}
static int fault_read(void *v,uint64_t lba,void *out) {
    Fixture *f=v; ++f->provider_reads;
    if(lba>=32 && lba<f->disk.data) ++f->fat_reads;
    return sb_read(&f->buffer,lba,out);
}
static int fault_write(void *v,uint64_t lba,const void *in) {
    Fixture *f=v;
    if(f->fail_stage==0) return F_IO;
    if(f->fail_stage>0) --f->fail_stage;
    return sb_write(&f->buffer,lba,in);
}
static int fault_begin(void *v) { return sb_begin(&((Fixture *)v)->buffer); }
static void fault_end(void *v,int accept) { sb_end(&((Fixture *)v)->buffer,accept); }
static Fixture *fixture(unsigned bytes,unsigned spc) {
    Fixture *f=calloc(1,sizeof(*f)); CHECK(f); init_image(&f->disk,bytes,spc);
    f->workspace=(FatWorkspace){f->workspace_data,bytes*3,0};
    OK(sb_init(&f->buffer,&f->disk.ops)); f->fault=f->buffer.ops; f->fault.context=f;
    f->fault.read=fault_read; f->fault.write=fault_write; f->fault.begin=fault_begin; f->fault.end=fault_end;
    f->inner=f->fault; f->fault.context=&f->inner;
    f->fault.read=abi_read; f->fault.write=abi_write; f->fault.begin=abi_begin; f->fault.end=abi_end;
    f->fail_stage=-1; OK(fat_mount(&f->id,&f->fault,NULL,&f->workspace)); return f;
}
static void destroy(Fixture *f) {
    unsigned i; OK(sb_discard(&f->buffer));
    for(i=0;i<4096;i++) { Page *p=f->disk.bucket[i]; while(p) { Page *n=p->next; free(p); p=n; } }
    free(f);
}
static FatEntry create(Fixture *f,uint32_t parent,const uint16_t *name,int dir) {
    FatCreate r={name,(uint32_t)dir,0}; FatEntry e; OK(fat_create(&f->id,parent,&r,&e)); return e;
}
static FatEntry lookup(Fixture *f,uint32_t parent,const uint16_t *name) { FatEntry e; OK(fat_lookup(&f->id,parent,name,&e)); return e; }
static void report(const char *s) { printf("  %s\n",s); fflush(stdout); ++tests; }
static void test_basic(unsigned bytes,unsigned spc) {
    Fixture *f=fixture(bytes,spc); FatEntry e,other; FatCursor cursor; FatTransfer t;
    unsigned char in[150123],out[150123],sector[4096]; unsigned i; uint32_t chain[2];
    report("create / data / extend / truncate / metadata / delete");
    printf("    sector=%u sectors/cluster=%u cluster=%u\n",bytes,spc,bytes*spc);
    CHECK(f->id.cluster_bytes==bytes*spc); CHECK(f->id.cluster_count==65530);
    OK(fat_dir_open(&f->id,2,&cursor)); CHECK(fat_dir_next(&f->id,&cursor,&e)==F_END);
    e=create(f,2,U("Long file name Ω.bin"),0); CHECK(e.size==0 && e.lfn_count>0);
    for(i=0;i<sizeof(in);i++) in[i]=(unsigned char)(i*37+5);
    t=(FatTransfer){in,17,sizeof(in),0}; OK(fat_write(&f->id,&e,&t)); CHECK(t.done==sizeof(in));
    t=(FatTransfer){out,17,sizeof(out),0}; OK(fat_read(&f->id,&e,&t)); CHECK(t.done==sizeof(out)); CHECK(!memcmp(in,out,sizeof(in)));
    t=(FatTransfer){out,0,17,0}; OK(fat_read(&f->id,&e,&t)); for(i=0;i<17;i++) CHECK(out[i]==0);
    other=lookup(f,2,U("long file name Ω.BIN")); CHECK(other.cluster==e.cluster && other.size==e.size);
    OK(fat_chain(&f->id,e.cluster,chain)); CHECK(chain[0]==(e.size+f->id.cluster_bytes-1)/f->id.cluster_bytes);
    OK(fat_resize(&f->id,&e,511)); CHECK(fat_read(&f->id,&other,&t)==F_STALE);
    OK(fat_resize(&f->id,&e,70000)); memset(out,0xFF,sizeof(out));
    t=(FatTransfer){out,511,69489,0}; OK(fat_read(&f->id,&e,&t)); for(i=0;i<t.done;i++) CHECK(out[i]==0);
    { FatStamp s={0x1234,0x5821,0x5822,0x2345,0x5823,199,0x23}; OK(fat_set_info(&f->id,&e,&s));
      CHECK(e.raw[11]==0x23 && e.raw[13]==199 && rd16(e.raw+14)==0x1234);
      t=(FatTransfer){in,0,1,0}; CHECK(fat_write(&f->id,&e,&t)==F_READONLY);
      s.attributes=0x20; OK(fat_set_info(&f->id,&e,&s)); }
    OK(sb_read(&f->buffer,1,sector)); CHECK(rd32(sector+488)==0xFFFFFFFF);
    OK(sb_read(&f->buffer,7,sector)); CHECK(rd32(sector+492)==0xFFFFFFFF);
    CHECK(f->disk.writes==0); OK(sb_commit(&f->buffer)); CHECK(f->buffer.pages==0 && f->disk.writes>0);
    OK(fat_mount(&f->id,&f->fault,NULL,&f->workspace)); e=lookup(f,2,U("Long file name Ω.bin")); CHECK(e.size==70000);
    OK(fat_resize(&f->id,&e,0)); CHECK(e.cluster==0 && e.size==0); OK(fat_remove(&f->id,&e));
    CHECK(fat_lookup(&f->id,2,U("Long file name Ω.bin"),&e)==F_NOTFOUND); destroy(f);
}
static void test_directories(void) {
    Fixture *f=fixture(512,1); FatEntry dir,file,e; uint16_t name[256]; unsigned i; uint32_t chain[2];
    report("LFN lengths / directory growth / nested directories / empty-only removal");
    dir=create(f,2,U("Nested directory"),1); file=create(f,dir.cluster,U("child"),0);
    dir=lookup(f,2,U("Nested directory")); CHECK(fat_remove(&f->id,&dir)==F_NOTEMPTY);
    file=lookup(f,dir.cluster,U("child")); OK(fat_remove(&f->id,&file));
    dir=lookup(f,2,U("Nested directory")); OK(fat_remove(&f->id,&dir));
    for(i=1;i<=255;i+=1) {
        unsigned j; for(j=0;j<i;j++) name[j]=(uint16_t)('a'+j%26); name[i]=0;
        e=create(f,2,name,0); file=lookup(f,2,name); CHECK(e.index==file.index && file.name_length==i);
    }
    OK(fat_chain(&f->id,2,chain)); CHECK(chain[0]>100);
    CHECK(fat_lookup(&f->id,2,U("not present"),&e)==F_NOTFOUND); destroy(f);
}
static void test_geometry(void) {
    Fixture *f=fixture(512,128); unsigned char *p=page(&f->disk,0,0)->data; FatCheck check;
    report("BPB rejection / truncated extent / reserved and cyclic FAT entries");
    p[13]=3; CHECK(fat_mount(&f->id,&f->fault,NULL,&f->workspace)==F_FORMAT); p[13]=128;
    wr16(p+11,1024); CHECK(fat_mount(&f->id,&f->fault,NULL,&f->workspace)==F_FORMAT); wr16(p+11,512);
    wr16(p+42,1); CHECK(fat_mount(&f->id,&f->fault,NULL,&f->workspace)==F_FORMAT); wr16(p+42,0);
    f->fault.sectors--; CHECK(fat_mount(&f->id,&f->fault,NULL,&f->workspace)==F_FORMAT); f->fault.sectors++;
    fat_value(&f->disk,0,2,2); OK(fat_mount(&f->id,&f->fault,NULL,&f->workspace));
    CHECK(fat_check_chain(&f->id,2,8,&check)==F_CORRUPT && check.issue==FC_CYCLE);
    fat_value(&f->disk,0,2,0x0FFFFFF7); OK(fat_mount(&f->id,&f->fault,NULL,&f->workspace));
    CHECK(fat_check_chain(&f->id,2,8,&check)==F_CORRUPT && check.issue==FC_LINK);
    fat_value(&f->disk,0,2,0); OK(fat_mount(&f->id,&f->fault,NULL,&f->workspace));
    CHECK(fat_check_chain(&f->id,2,8,&check)==F_CORRUPT && check.issue==FC_LINK);
    fat_value(&f->disk,0,2,0x0FFFFFFF); OK(fat_mount(&f->id,&f->fault,NULL,&f->workspace)); destroy(f);
}
static void test_rollback(void) {
    unsigned fail, failures=0; report("write failure at every staging point / rollback preserves previous edits");
    for(fail=0;fail<100;fail++) {
        Fixture *f=fixture(512,1); FatEntry e=create(f,2,U("existing"),0),before=e;
        unsigned char data[1800]; FatTransfer t={data,0,sizeof(data),0};
        void *head=f->buffer.head; uint64_t pages=f->buffer.pages; int s;
        memset(data,0xA5,sizeof(data)); f->fail_stage=(int)fail;
        s=fat_write(&f->id,&e,&t);
        if(!s) { CHECK(t.done==sizeof(data)); destroy(f); break; }
        CHECK(s==F_IO); ++failures; CHECK(t.done==0); CHECK(!memcmp(&e,&before,sizeof(e)));
        CHECK(f->id.next_free==2 && f->id.free_hint==UINT32_MAX);
        CHECK(f->id.fat_lba==UINT64_MAX && f->id.dir_lba==UINT64_MAX);
        CHECK(f->buffer.head==head && f->buffer.pages==pages && f->buffer.active==0);
        f->fail_stage=-1; e=lookup(f,2,U("existing")); CHECK(e.size==0 && e.cluster==0); destroy(f);
    }
    CHECK(failures>10 && fail<100);
}
static void test_buffer(void) {
    Fixture *f=fixture(512,1); unsigned char a[4096],b[4096];
    report("generic buffer savepoint / bounds / commit failure poisoning");
    memset(a,0x77,sizeof(a)); CHECK(sb_write(&f->buffer,10,a)==F_BUSY);
    OK(sb_begin(&f->buffer)); CHECK(sb_begin(&f->buffer)==F_BUSY); OK(sb_write(&f->buffer,10,a)); sb_end(&f->buffer,1);
    OK(sb_begin(&f->buffer)); memset(a,0x33,sizeof(a)); OK(sb_write(&f->buffer,10,a)); sb_end(&f->buffer,0);
    OK(sb_read(&f->buffer,10,b)); CHECK(b[0]==0x77); CHECK(sb_read(&f->buffer,f->disk.ops.sectors,b)==F_RANGE);
    f->disk.fail_write=0; CHECK(sb_commit(&f->buffer)==F_IO); CHECK(f->buffer.poisoned==1); CHECK(sb_begin(&f->buffer)==F_IO); destroy(f);
}
static void raw_entry(unsigned char *p,const char *name,uint32_t cl,uint32_t size) {
    memset(p,0,32); memcpy(p,name,11); p[11]=0x20; wr16(p+20,cl>>16); wr16(p+26,cl); wr32(p+28,size);
}
static void test_fragmented(unsigned bytes,unsigned spc) {
    Fixture *f=fixture(bytes,spc); Image *d=&f->disk; FatEntry e; FatTransfer t; uint32_t v;
    unsigned cb=bytes*spc,length=cb*2+176,middle=d->clusters+1;
    unsigned char *out=malloc(length),*in=malloc(length),b[4096]; unsigned i,j;
    report("independently encoded fragmented chain / high nibble / mirror preservation");
    CHECK(out && in);
    printf("    sector=%u cluster=%u chain=5,%u,8 middle-byte-offset=%llu\n",
           bytes,cb,middle,(unsigned long long)(d->data+(uint64_t)(middle-2)*spc)*bytes);
    raw_entry(page(d,d->data,1)->data,"FRAG    BIN",5,length);
    for(i=0;i<2;i++) {
        uint32_t high=(i+10)<<28;
        fat_value(d,i,3,high); fat_value(d,i,5,high|middle); fat_value(d,i,middle,high|8); fat_value(d,i,8,high|0x0FFFFFFF);
    }
    for(j=0;j<spc;j++) {
        memset(page(d,d->data+(uint64_t)3*spc+j,1)->data,0x11,bytes);
        memset(page(d,d->data+(uint64_t)(middle-2)*spc+j,1)->data,0x22,bytes);
        memset(page(d,d->data+(uint64_t)6*spc+j,1)->data,0x33,bytes);
    }
    OK(fat_mount(&f->id,&f->fault,NULL,&f->workspace)); e=lookup(f,2,U("frag.bin"));
    t=(FatTransfer){out,0,length,0}; OK(fat_read(&f->id,&e,&t)); CHECK(t.done==length);
    for(i=0;i<length;i++) CHECK(out[i]==(i<cb?0x11:i<cb*2?0x22:0x33));
    memset(in,0xA9,length); t=(FatTransfer){in,cb-13,cb+188,0}; OK(fat_write(&f->id,&e,&t));
    t=(FatTransfer){out,0,length,0}; OK(fat_read(&f->id,&e,&t));
    for(i=0;i<length;i++) CHECK(out[i]==(i<cb-13?0x11:i<length-1?0xA9:0x33));
    /* Inspect committed sectors independently, including the last sector of
       the first cluster and the backward link out of the high cluster. */
    OK(sb_commit(&f->buffer));
    for(i=0;i<length;i++) {
        unsigned c=i/cb,k=i%cb; uint32_t cl=c==0?5:c==1?middle:8;
        Page *p=page(d,d->data+(uint64_t)(cl-2)*spc+k/bytes,0);
        CHECK(p && p->data[k%bytes]==out[i]);
    }
    OK(fat_resize(&f->id,&e,cb*4-48));
    for(i=0;i<2;i++) {
        OK(sb_read(&f->buffer,32+(uint64_t)i*d->fat_sectors,b));
        CHECK((rd32(b+3*4)&0xF0000000)==(i+10)<<28);
        CHECK((rd32(b+8*4)&0xF0000000)==(i+10)<<28);
        CHECK((rd32(b+8*4)&0x0FFFFFFF)==3);
    }
    OK(fat_resize(&f->id,&e,0)); OK(fat_get(&f->id,middle,&v)); CHECK(v==0);
    free(in); free(out); destroy(f);
}
static void test_large_directory(unsigned bytes,unsigned spc) {
    Fixture *f=fixture(bytes,spc); Image *d=&f->disk; FatEntry e,found;
    FatCursor cursor; uint16_t name[256]; uint32_t chain[2]; unsigned i,count=0;
    unsigned slots=bytes*spc/32; char short_name[12];
    report("large-cluster directory / LFN crossing final sector / grow / rename / delete");
    printf("    sector=%u sectors/cluster=%u directory slots=%u\n",bytes,spc,slots);
    /* Independently fill all but the final slot; no deleted slots can absorb
       the new 20-LFN + SFN set. This forces allocation while writing that set. */
    for(i=0;i<slots-1;i++) {
        sprintf_s(short_name,sizeof(short_name),"N%07uBIN",i);
        raw_entry(page(d,d->data+(uint64_t)i*32/bytes,1)->data+i*32%bytes,short_name,0,0);
    }
    for(i=0;i<255;i++) name[i]=(uint16_t)('a'+i%26); name[255]=0;
    OK(fat_invalidate(&f->id)); e=create(f,2,name,0);
    CHECK(e.lfn_count==20 && e.index==slots+19);
    OK(fat_chain(&f->id,2,chain)); CHECK(chain[0]==2);
    OK(sb_commit(&f->buffer)); OK(fat_mount(&f->id,&f->fault,NULL,&f->workspace));
    found=lookup(f,2,name); CHECK(found.index==e.index && found.name_length==255);
    OK(fat_dir_open(&f->id,2,&cursor));
    while(fat_dir_next(&f->id,&cursor,&e)==F_OK) ++count;
    CHECK(count==slots); CHECK(cursor.ended);
    OK(fat_rename(&f->id,&found,U("Renamed across 64KiB Ω.bin")));
    OK(fat_remove(&f->id,&found)); OK(sb_commit(&f->buffer));
    OK(fat_mount(&f->id,&f->fault,NULL,&f->workspace));
    CHECK(fat_lookup(&f->id,2,U("Renamed across 64KiB Ω.bin"),&found)==F_NOTFOUND);
    e=lookup(f,2,U("N0002046.BIN")); CHECK(e.index==2046);
    destroy(f);
}
static void test_boot_in_directory(unsigned spc) {
    Fixture *f=fixture(512,spc); FatEntry e; FatCursor cursor; unsigned char *p;
    report("boot-sector bytes in directory rejected independently of cluster size");
    printf("    sector=512 cluster=%u decoded invalid cluster=134217983\n",512*spc);
    /* Reproduce the recorded byte pattern, not an unavailable original image.
       Both geometries mount; only directory interpretation must fail. */
    p=page(&f->disk,f->disk.data,1)->data;
    memcpy(p,page(&f->disk,0,0)->data,512); memcpy(p+3,"MSDOS5.0",8); wr16(p+26,255);
    CHECK((((uint32_t)rd16(p+20)<<16)|rd16(p+26))%0x10000000==134217983);
    OK(fat_invalidate(&f->id)); OK(fat_dir_open(&f->id,2,&cursor));
    CHECK(fat_dir_next(&f->id,&cursor,&e)==F_CORRUPT);
    CHECK(f->disk.writes==0); destroy(f);
}
static void test_external_cache_change(unsigned spc) {
    Fixture *f=fixture(512,spc); Image *d=&f->disk; FatEntry e; uint32_t cached;
    unsigned char out=0; FatTransfer transfer={&out,0,1,0}; unsigned i;
    report("external FAT change / stale read cache / explicit quiescent invalidation");
    /* Explicitly cache FAT[3]==free. Mount no longer reads the FAT. An external
       writer now publishes a valid file, outside the library's generation. */
    OK(fat_get(&f->id,3,&cached)); CHECK(cached==0);
    CHECK(f->id.fat_lba==32 && rd32(f->id.fat+12)==0);
    for(i=0;i<2;i++) fat_value(d,i,3,0xFFFFFFF);
    raw_entry(page(d,d->data,1)->data,"EXTERNALBIN",3,1);
    page(d,d->data+spc,1)->data[0]=0xA7;
    e=lookup(f,2,U("EXTERNAL.BIN"));
    CHECK(fat_read(&f->id,&e,&transfer)==F_CORRUPT && transfer.done==0);
    /* Once the external writer has stopped, invalidate and reacquire entries.
       This controlled reproduction is not proof of the USB failure's cause. */
    OK(fat_invalidate(&f->id)); e=lookup(f,2,U("EXTERNAL.BIN"));
    OK(fat_read(&f->id,&e,&transfer)); CHECK(transfer.done==1 && out==0xA7);
    printf("    cluster=%u cached-free -> F_CORRUPT; invalidate/relookup -> F_OK\n",512*spc);
    destroy(f);
}
static void test_active_fat(void) {
    Fixture *f=fixture(512,1); Image *d=&f->disk; FatEntry e; unsigned char original[512],now[4096];
    report("disabled mirroring / active FAT 1 / FSInfo remains advisory");
    wr16(page(d,0,0)->data+40,0x81); fat_value(d,0,2,0); /* deliberately unusable inactive FAT */
    wr32(page(d,1,0)->data+488,0); wr32(page(d,1,0)->data+492,0xFFFFFFFE);
    memcpy(original,page(d,32,0)->data,512); OK(fat_mount(&f->id,&f->fault,NULL,&f->workspace));
    e=create(f,2,U("active"),0); OK(fat_resize(&f->id,&e,5000));
    OK(sb_read(&f->buffer,32,now)); CHECK(!memcmp(original,now,512));
    OK(sb_read(&f->buffer,32+d->fat_sectors,now)); CHECK((rd32(now+12)&0x0FFFFFFF)!=0);
    destroy(f);
}
static void test_no_space(void) {
    Fixture *f=fixture(512,1); FatEntry e; uint32_t c; void *head; uint64_t pages;
    report("full FAT / rollback after partial allocation / 4 GiB file-size limit");
    e=create(f,2,U("full"),0); head=f->buffer.head; pages=f->buffer.pages;
    for(c=4;c<f->disk.clusters+2;c++) { fat_value(&f->disk,0,c,0x0FFFFFF7); fat_value(&f->disk,1,c,0x0FFFFFF7); }
    OK(fat_invalidate(&f->id)); e=lookup(f,2,U("full"));
    CHECK(fat_resize(&f->id,&e,1024)==F_NOSPACE);
    CHECK(f->buffer.head==head && f->buffer.pages==pages); e=lookup(f,2,U("full"));
    CHECK(fat_resize(&f->id,&e,0x100000000ULL)==F_RANGE);
    { unsigned char x=1; FatTransfer t={&x,0xFFFFFFFFULL,1,0}; CHECK(fat_write(&f->id,&e,&t)==F_RANGE); }
    CHECK(e.cluster==0 && e.size==0); destroy(f);
}
static void test_invalid_names(void) {
    Fixture *f=fixture(512,1); FatEntry e; FatCreate r; uint16_t malformed[]={0xD800,'x',0};
    const uint16_t *names[]={U(""),U("."),U(".."),U("bad/name"),U("trailing."),U("trailing "),malformed};
    unsigned i; report("invalid UTF-16 components / duplicate names / SFN aliases");
    for(i=0;i<sizeof(names)/sizeof(names[0]);i++) { r=(FatCreate){names[i],0,0}; CHECK(fat_create(&f->id,2,&r,&e)==F_NAME); }
    e=create(f,2,U("Long Name"),0); r=(FatCreate){U("long name"),0,0}; CHECK(fat_create(&f->id,2,&r,&e)==F_EXISTS);
    e=lookup(f,2,U("F0000001")); CHECK(e.name_length==9);
    { uint16_t unicode[]={0xD83D,0xDE00,'.','t','x','t',0}; e=create(f,2,unicode,0); e=lookup(f,2,unicode); CHECK(e.name_length==6); }
    destroy(f);
}
static void test_corruption(void) {
    Fixture *f=fixture(512,1); FatEntry e; FatCursor cursor; unsigned char *p; FatTransfer t; unsigned char x=1;
    report("malformed directory clusters / basic mirrored FAT update");
    p=page(&f->disk,f->disk.data,1)->data;
    raw_entry(p,"BROKEN  BIN",0x7FFFFFF,3); OK(fat_invalidate(&f->id));
    OK(fat_dir_open(&f->id,2,&cursor)); CHECK(fat_dir_next(&f->id,&cursor,&e)==F_CORRUPT);
    memset(p,0,512); OK(fat_invalidate(&f->id)); e=create(f,2,U("mirror"),0);
    fat_value(&f->disk,1,3,0xFFFFFFF); t=(FatTransfer){&x,0,1,0};
    OK(fat_write(&f->id,&e,&t)); CHECK(e.cluster==3 && t.done==1); destroy(f);
}
static void test_win32_io(void) {
    wchar_t path[260]; WinVolume v={0}; SectorBuffer b; HANDLE h;
    LARGE_INTEGER size; unsigned char *aligned; unsigned char out[4096],input[4096]; DWORD got,returned; unsigned i;
    report("Win32 unbuffered sector read/write / sparse offset beyond 4 GiB / commit");
    CreateDirectoryW(L"build",NULL);
    swprintf_s(path,260,L"build\\sector-io-%lu.bin",GetCurrentProcessId());
    h=CreateFileW(path,GENERIC_READ|GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_FLAG_NO_BUFFERING|FILE_FLAG_WRITE_THROUGH,NULL);
    CHECK(h!=INVALID_HANDLE_VALUE);
    CHECK(DeviceIoControl(h,0x900C4,NULL,0,NULL,0,&returned,NULL)); /* FSCTL_SET_SPARSE */
    size.QuadPart=0x100000000LL+8192; CHECK(SetFilePointerEx(h,size,NULL,FILE_BEGIN)); CHECK(SetEndOfFile(h));
    aligned=VirtualAlloc(NULL,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE); CHECK(aligned);
    v.handle=h; v.locked=1; v.bounce=aligned; v.ops.context=&v; v.ops.read=win_read; v.ops.write=win_write; v.ops.flush=win_flush;
    v.ops.sector_bytes=512; v.ops.sectors=(uint64_t)size.QuadPart/512;
    for(i=0;i<sizeof(input);i++) input[i]=(unsigned char)(i*7+13);
    OK(sb_init(&b,&v.ops)); OK(sb_begin(&b)); OK(sb_write(&b,0x800003,input)); sb_end(&b,1); OK(sb_commit(&b));
    OK(ABI(win_write,&v,0x800003,input,0)); OK(ABI(win_flush,&v,0,0,0));
    memset(out,0,sizeof(out)); OK(ABI(win_read,&v,0x800003,out,0)); CHECK(!memcmp(out,input,512));
    size.QuadPart=0x100000000LL+3*512; CHECK(SetFilePointerEx(h,size,NULL,FILE_BEGIN));
    CHECK(ReadFile(h,aligned,512,&got,NULL)); CHECK(got==512 && !memcmp(aligned,input,512));
    CHECK(ABI(win_read,&v,v.ops.sectors,out,0)==F_RANGE); v.locked=0; CHECK(ABI(win_write,&v,0,input,0)==F_READONLY);
    OK(ABI(win_close,&v,0,0,0)); CHECK(DeleteFileW(path));
    CHECK(ABI(win_open,&v,path,0,0)==F_IO); CHECK(v.error==ERROR_FILE_NOT_FOUND);
    OK(ABI(win_close,&v,0,0,0));
}
static void test_rename(void) {
    Fixture *f=fixture(512,1); FatEntry e,other; unsigned char input[1200],out[1200]; FatTransfer t;
    uint32_t cluster; unsigned fail,failures=0;
    report("atomic rename / case-only rename / directory rename / rollback");
    memset(input,0x39,sizeof(input)); e=create(f,2,U("original"),0);
    t=(FatTransfer){input,0,sizeof(input),0}; OK(fat_write(&f->id,&e,&t)); cluster=e.cluster;
    OK(fat_rename(&f->id,&e,U("renamed Ω"))); CHECK(e.cluster==cluster && e.size==sizeof(input));
    CHECK(fat_lookup(&f->id,2,U("original"),&other)==F_NOTFOUND);
    OK(fat_rename(&f->id,&e,U("RENAMED Ω"))); CHECK(e.name[0]=='R');
    t=(FatTransfer){out,0,sizeof(out),0}; OK(fat_read(&f->id,&e,&t)); CHECK(!memcmp(input,out,sizeof(input)));
    other=create(f,2,U("directory"),1); cluster=other.cluster; OK(fat_rename(&f->id,&other,U("directory renamed"))); CHECK(other.cluster==cluster);
    for(fail=0;fail<100;fail++) {
        void *head=f->buffer.head; uint64_t pages=f->buffer.pages; int s;
        e=lookup(f,2,U("RENAMED Ω")); f->fail_stage=(int)fail;
        s=fat_rename(&f->id,&e,U("final name")); f->fail_stage=-1;
        if(!s) break;
        CHECK(s==F_IO); CHECK(f->buffer.head==head && f->buffer.pages==pages); ++failures;
        CHECK(fat_lookup(&f->id,2,U("final name"),&other)==F_NOTFOUND);
    }
    CHECK(failures>=5 && fail<100); destroy(f);
}
static void test_lfn_damage(void) {
    Fixture *f=fixture(512,1); FatEntry e; unsigned char *p; unsigned char raw[4096]; uint32_t first_sector=f->disk.data;
    report("orphan LFN checksum / order / padding / Unicode fallback to SFN");
    e=create(f,2,U("long.txt"),0); OK(sb_commit(&f->buffer));
    p=page(&f->disk,first_sector,0)->data; memcpy(raw,p,512);
    p[13]^=1; OK(fat_mount(&f->id,&f->fault,NULL,&f->workspace)); e=lookup(f,2,U("F0000001")); CHECK(e.lfn_count==0);
    memcpy(p,raw,512); p[0]=0x42; OK(fat_mount(&f->id,&f->fault,NULL,&f->workspace)); e=lookup(f,2,U("F0000001")); CHECK(e.lfn_count==0);
    memcpy(p,raw,512); wr16(p+1,0xD800); OK(fat_mount(&f->id,&f->fault,NULL,&f->workspace)); e=lookup(f,2,U("F0000001")); CHECK(e.lfn_count==0);
    memcpy(p,raw,512); wr16(p+24,'x'); OK(fat_mount(&f->id,&f->fault,NULL,&f->workspace)); e=lookup(f,2,U("F0000001")); CHECK(e.lfn_count==0);
    memcpy(p,raw,512); OK(fat_mount(&f->id,&f->fault,NULL,&f->workspace)); e=lookup(f,2,U("long.txt")); CHECK(e.lfn_count==1);
    destroy(f);
}
static void test_mutation_rollback(void) {
    unsigned operation,fail,failures; report("create / shrink / remove / metadata rollback at every write");
    for(operation=0;operation<4;operation++) {
        failures=0;
        for(fail=0;fail<100;fail++) {
            Fixture *f=fixture(512,1); FatEntry e=create(f,2,U("kept"),0),before; int s;
            void *head; uint64_t pages;
            OK(fat_resize(&f->id,&e,3500)); before=e; head=f->buffer.head; pages=f->buffer.pages;
            f->fail_stage=(int)fail;
            if(operation==0) { FatCreate r={U("new directory"),1,0}; s=fat_create(&f->id,2,&r,&e); }
            else if(operation==1) s=fat_resize(&f->id,&e,100);
            else if(operation==2) s=fat_remove(&f->id,&e);
            else { FatStamp stamp={0,0,0,0,0,0,0x21}; s=fat_set_info(&f->id,&e,&stamp); }
            if(!s) { destroy(f); break; }
            CHECK(s==F_IO); ++failures; CHECK(!memcmp(&e,&before,sizeof(e)));
            CHECK(head==f->buffer.head && pages==f->buffer.pages);
            f->fail_stage=-1; e=lookup(f,2,U("kept")); CHECK(e.size==3500); destroy(f);
        }
        CHECK(failures>0 && fail<100);
    }
}
static void test_media_errors(void) {
    Fixture *f=fixture(512,1); FatEntry e; FatTransfer t; unsigned char data[1000]; uint64_t pages; void *head;
    report("backend read failure / allocation limit / failed flush retains poisoned overlay");
    e=create(f,2,U("failure"),0); OK(fat_resize(&f->id,&e,sizeof(data)));
    OK(sb_commit(&f->buffer)); OK(fat_mount(&f->id,&f->fault,NULL,&f->workspace)); e=lookup(f,2,U("failure"));
    f->disk.fail_read=0; t=(FatTransfer){data,0,sizeof(data),123}; CHECK(fat_read(&f->id,&e,&t)==F_IO); CHECK(t.done==0);
    f->disk.fail_read=-1; e=lookup(f,2,U("failure")); pages=f->buffer.pages; head=f->buffer.head;
    f->buffer.limit=pages+1; CHECK(fat_resize(&f->id,&e,5000)==F_MEMORY); CHECK(f->buffer.pages==pages && f->buffer.head==head);
    f->buffer.limit=131072; e=lookup(f,2,U("failure")); memset(data,0x73,sizeof(data));
    t=(FatTransfer){data,0,sizeof(data),0}; OK(fat_write(&f->id,&e,&t));
    f->disk.fail_flush=1; CHECK(sb_commit(&f->buffer)==F_IO); CHECK(f->buffer.poisoned && f->buffer.pages);
    CHECK(sb_read(&f->buffer,0,data)==F_IO); destroy(f);
}
static void test_abi(void) {
    Fixture *f=fixture(512,1); FatEntry e,found; FatCursor cursor; FatCreate request={U("ABI.bin"),0,0};
    FatTransfer transfer; FatStamp stamp={0,0,0,0,0,0,0x20}; SectorBuffer extra={0}; uint32_t chain[2],value;
    unsigned char data[517],out[517];
    report("Win64 registers / home-space clobbers and bounds / packed DWORDs / fifth argument");
    OK(ABI(abi_home_probe,0,0,0,0));
    OK(ABI(fat_mount,&f->id,&f->fault,NULL,&f->workspace));
    OK(ABI(fat_create,&f->id,2,&request,&e));
    memset(data,0xA9,sizeof(data)); transfer=(FatTransfer){data,0,sizeof(data),0};
    OK(ABI(fat_write,&f->id,&e,&transfer,0)); CHECK(transfer.done==sizeof(data));
    transfer=(FatTransfer){out,0,sizeof(out),0}; OK(ABI(fat_read,&f->id,&e,&transfer,0)); CHECK(!memcmp(data,out,sizeof(data)));
    OK(ABI(fat_resize,&f->id,&e,1031,0));
    OK(ABI(fat_set_info,&f->id,&e,&stamp,0));
    OK(ABI(fat_rename,&f->id,&e,U("ABI renamed Ω.bin"),0));
    OK(ABI(fat_lookup,&f->id,2,U("ABI renamed Ω.bin"),&found));
    OK(ABI(fat_dir_open,&f->id,0xFFFFFFFF00000002ull,&cursor,0));
    CHECK(cursor.generation==f->id.generation && cursor.cluster==2 && cursor.parent==2);
    OK(ABI(fat_dir_next,&f->id,&cursor,&found,0));
    CHECK(ABI(fat_dir_next,&f->id,&cursor,&found,0)==F_END);
    OK(ABI(fat_get,&f->id,e.cluster,&value,0));
    OK(ABI(fat_chain,&f->id,e.cluster,chain,0)); CHECK(chain[0]==3);
    CHECK(ABI(fat_resize,&f->id,&e,0x100000000ull,0)==F_RANGE);
    stamp.create_tenth=200; CHECK(ABI(fat_set_info,&f->id,&e,&stamp,0)==F_ARGUMENT); stamp.create_tenth=0;
    stamp.attributes=0x21; OK(ABI(fat_set_info,&f->id,&e,&stamp,0));
    CHECK(ABI(fat_write,&f->id,&e,&transfer,0)==F_READONLY);
    CHECK(ABI(fat_resize,&f->id,&e,10,0)==F_READONLY);
    CHECK(ABI(fat_rename,&f->id,&e,U("blocked"),0)==F_READONLY);
    CHECK(ABI(fat_remove,&f->id,&e,0,0)==F_READONLY);
    stamp.attributes=0x20; OK(ABI(fat_set_info,&f->id,&e,&stamp,0));
    OK(ABI(fat_remove,&f->id,&e,0,0));
    CHECK(ABI(fat_lookup,&f->id,2,U("ABI renamed Ω.bin"),&found)==F_NOTFOUND);
    CHECK(ABI(fat_read,&f->id,&e,&transfer,0)==F_STALE);
    OK(ABI(fat_invalidate,&f->id,0,0,0));
    OK(ABI(sb_init,&extra,&f->disk.ops,0,0));
    CHECK(ABI(sb_write,&extra,10,data,0)==F_BUSY);
    OK(ABI(sb_begin,&extra,0,0,0)); CHECK(ABI(sb_begin,&extra,0,0,0)==F_BUSY);
    CHECK(ABI(sb_commit,&extra,0,0,0)==F_BUSY);
    OK(ABI(sb_write,&extra,10,data,0)); (void)ABI(sb_end,&extra,1,0,0);
    OK(ABI(sb_read,&extra,10,out,0)); CHECK(!memcmp(data,out,512));
    CHECK(ABI(sb_read,&extra,extra.ops.sectors,out,0)==F_RANGE);
    OK(ABI(sb_commit,&extra,0,0,0)); OK(ABI(sb_discard,&extra,0,0,0));
    destroy(f);
}
#include "shared.c"
#include "checking.c"
#include "check-directory.c"
#include "check-names.c"
#include "check-ownership.c"
#include "ordering.c"
#include "commit.c"
#include "commit-traces.c"
#include "stream.c"
#include "space.c"
#include "put.c"
#include "allocation.c"
#include "format.c"
#include "format-traces.c"
#include "view.c"
#include "salvage.c"
#include "boot-view.c"
#include "policy.c"

int main(int argc,char **argv) {
    if(argc==8 && !strcmp(argv[1],"--format-trace"))
        return export_format_trace(argv[2],(unsigned)strtoul(argv[3],NULL,10),(unsigned)strtoul(argv[4],NULL,10),
                                   (unsigned)strtoul(argv[5],NULL,10),argv[6],argv[7]);
    if(argc==5 && !strcmp(argv[1],"--commit-trace"))
        return export_commit_trace(argv[2],(unsigned)strtoul(argv[3],NULL,10),(unsigned)strtoul(argv[4],NULL,10));
    if(argc!=1) { fprintf(stderr,"usage: tests.exe [--commit-trace output operation geometry]\n"); return 2; }
    printf("FAT32 verification (assembly library, synthetic sparse sector backends)\n");
    test_allocation_progress(512,1); test_allocation_progress(512,128); test_allocation_progress(4096,16);
    test_allocation_wrap(); test_mutation_caches();
    test_geometry(); test_buffer(); test_basic(512,1); test_basic(512,64); test_basic(512,128); test_basic(4096,16);
    test_directories(); test_rollback(); test_fragmented(512,1); test_fragmented(512,128); test_fragmented(4096,16);
    test_large_directory(512,128); test_large_directory(4096,16);
    test_boot_in_directory(1); test_boot_in_directory(64); test_boot_in_directory(128);
    test_external_cache_change(1); test_external_cache_change(128);
    test_active_fat(); test_no_space(); test_invalid_names();
    test_corruption(); test_win32_io(); test_rename(); test_lfn_damage(); test_mutation_rollback(); test_media_errors(); test_abi();
    test_shared(512,1); test_shared(512,128); test_shared(4096,16);
    test_shared_capacity(); test_shared_directories(); test_shared_rollback(); test_shared_basic_read(); test_shared_gate();
    test_shared_workspace();
    test_put(); test_put_failures(); test_put_lba_span();
    test_format(); test_format_admission(); test_format_failures();
    test_view(); test_view_admission(); test_view_staging();
    test_salvage(); test_salvage_damage(); test_salvage_admission(); test_salvage_large();
    test_boot_view(); test_boot_view_admission();
    test_policy_basic(); test_policy_damage(); test_policy_io(); test_policy_limits();
    test_policy_fallback(); test_policy_commit();
    test_basic(1024,128); test_basic(2048,128); test_basic(4096,64);
    test_fragmented(1024,128); test_fragmented(2048,128); test_fragmented(4096,64);
    test_shared(1024,128); test_shared(2048,128); test_shared(4096,64);
    test_stream(1024,128); test_stream(2048,128); test_stream(4096,64);
    test_large_directory(2048,128);
    test_check_chain(); test_check_file(); test_check_metadata(); test_check_mirrors();
    test_read_adaptive();
    test_check_directory(); test_check_directory_parent();
    test_check_names(); test_check_names_limits();
    test_check_ownership(); test_check_ownership_limits();
    test_ordering(); test_ordering_failures();
    test_commit(); test_commit_limits(); test_commit_failures(); test_commit_admission();
    test_commit_gate_variants();
    test_stream(512,1); test_stream(512,128); test_stream(4096,16); test_stream_limits();
    test_stream_fragmented();
    test_stream_ordered();
    test_space();
    printf("PASS: %u suites\n",tests); return 0;
}
