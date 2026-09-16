/* Freestanding host fixture: real read paths, deliberately writable provider.
   No device access, CRT, heap or operating-system imports. */
#include "../../fat32.h"
#define CHECK(x) do { if (!(x)) return __LINE__; } while (0)
enum { SECTOR=512, FAT_SECTORS=513, DATA_START=32+2*FAT_SECTORS,
       SECTORS=DATA_START+65536 };
static uint8_t boot[SECTOR],fat[SECTOR],directory[SECTOR],data[SECTOR];
static uint8_t work[3*SECTOR],output[8],original_data[SECTOR];
static unsigned reads,writes,begins,ends,flushes;
static FatIdentity identity;
static FatVolume volume;
static FatObject objects[4];
static FatHandle root,file;
#ifndef READONLY_CONTROL
static FatObject saved_object;
static FatHandle new_handle,saved_handle;
static FatEntry entry,saved_entry,new_entry;
static FatOrder order,saved_order;
static const uint16_t other[]={'N','E','W','.','T','X','T',0};
#endif
static const uint16_t name[]={'R','E','A','D','.','T','X','T',0};
extern const uintptr_t readonly_roots[];
static void fill(void *p,uint8_t value,unsigned n) {
    volatile uint8_t *out=p; while(n--) *out++=value;
}
static void copy(void *out,const void *in,unsigned n) {
    volatile uint8_t *d=out; const uint8_t *s=in; while(n--) *d++=*s++;
}
static int equal(const void *a,const void *b,unsigned n) {
    const uint8_t *x=a,*y=b; while(n--) if(*x++!=*y++) return 0; return 1;
}
static void w16(uint8_t *p,uint16_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static void w32(uint8_t *p,uint32_t v) { w16(p,(uint16_t)v); w16(p+2,(uint16_t)(v>>16)); }
static int read_sector(void *context,uint64_t lba,void *out) {
    (void)context; ++reads;
    if(lba>=SECTORS) return F_RANGE;
    fill(out,0,SECTOR);
    if(lba==0) copy(out,boot,SECTOR);
    else if(lba==32 || lba==32+FAT_SECTORS) copy(out,fat,SECTOR);
    else if(lba==DATA_START) copy(out,directory,SECTOR);
    else if(lba==DATA_START+1) copy(out,data,SECTOR);
    return F_OK;
}
static int write_sector(void *context,uint64_t lba,const void *in) {
    (void)context; ++writes;
    if(lba>=SECTORS) return F_RANGE;
    if(lba==DATA_START+1) copy(data,in,SECTOR);
    else if(lba==DATA_START) copy(directory,in,SECTOR);
    else if(lba==32 || lba==32+FAT_SECTORS) copy(fat,in,SECTOR);
    else if(lba==0) copy(boot,in,SECTOR);
    return F_OK;
}
static int begin(void *context) { (void)context; ++begins; return F_OK; }
static void end(void *context,int commit) { (void)context; (void)commit; ++ends; }
static int flush(void *context) { (void)context; ++flushes; return F_OK; }
static SectorOps ops={0,read_sector,write_sector,begin,end,flush,SECTORS,SECTOR,0};
static FatWorkspace workspace={work,sizeof(work),0};

int readonly_probe(void) {
    FatTransfer transfer={output,0,5,99};
#ifndef READONLY_CONTROL
    FatBuffer buffer={output,1,99};
    FatStamp stamp={0};
    FatCreate request={other,0,0};
    FatOrderWorkspace staging={0};
    struct { uint64_t before; FatCommitReport report; uint64_t after; } guarded;
    FatCall call;
    struct { uint64_t before; FatFormatReport report; uint64_t after; } formatted;
#endif
    /* Root every public entry, including accidental mutation references. */
    CHECK(*(volatile const uintptr_t *)readonly_roots!=0);
    w16(boot+11,SECTOR); boot[13]=1; w16(boot+14,32); boot[16]=2;
    boot[21]=0xf8; w32(boot+32,SECTORS); w32(boot+36,FAT_SECTORS);
    w32(boot+44,2); w16(boot+510,0xaa55);
    w32(fat,0x0ffffff8); w32(fat+4,0x0fffffff);
    w32(fat+8,0x0fffffff); w32(fat+12,0x0fffffff);
    copy(directory,"READ    TXT",11); directory[11]=0x20;
    w16(directory+26,3); w32(directory+28,5); copy(data,"hello",5);
    copy(original_data,data,SECTOR);
    CHECK(fat_mount(&identity,&ops,0,&workspace)==F_OK);
    CHECK(fat_volume_init(&volume,&identity,objects,4)==F_OK);
    CHECK(fat_root(&volume,FH_READ|FH_WRITE,&root)==F_OK);
    CHECK(fat_open(&root,name,FH_READ|FH_WRITE,&file)==F_OK);
    CHECK(fat_read_at(&file,&transfer)==F_OK && transfer.done==5);
    CHECK(equal(output,"hello",5));
    output[0]='X'; transfer.length=1; transfer.done=99;
#ifdef READONLY_CONTROL
    /* Same fixture without stubs really writes, so zero writes is not caused
       by a read-only provider, handle or malformed test volume. */
    CHECK(fat_write_at(&file,&transfer)==F_OK && transfer.done==1);
    CHECK(writes && begins && ends && data[0]=='X');
    return 0;
#else
    for(unsigned verified=0;verified<2;++verified) {
        fill(&formatted,0xa5,sizeof(formatted));
        CHECK((verified?fat_format_verified:fat_format)(&ops,0,0,&formatted.report)==F_READONLY);
        CHECK(formatted.report.status==F_READONLY && formatted.report.effect==FE_NONE);
        CHECK(!formatted.report.phase && !formatted.report.operation && formatted.report.lba==UINT64_MAX);
        CHECK(!formatted.report.writes && !formatted.report.flushes && !formatted.report.completed_writes);
        CHECK(!formatted.report.completed_flushes && !formatted.report.reads);
        CHECK(formatted.before==UINT64_C(0xa5a5a5a5a5a5a5a5) && formatted.after==formatted.before);
    }
    CHECK(fat_put(&identity,3,0)==F_READONLY);
    CHECK(fat_put_checked(&identity,3,0)==F_READONLY);
    copy(&saved_object,file.object,sizeof(saved_object));
    CHECK(fat_seek(&file,2)==F_OK);
    CHECK(fat_write_at(&file,&transfer)==F_READONLY && transfer.done==0);
    CHECK(fat_write_next(&file,&buffer)==F_READONLY && buffer.done==0 && file.position==2);
    CHECK(fat_handle_resize(&file,2)==F_READONLY);
    CHECK(fat_handle_set_info(&file,&stamp)==F_READONLY);
    CHECK(fat_handle_rename(&file,other)==F_READONLY);
    CHECK(fat_unlink(&root,name)==F_READONLY);
    fill(&new_handle,0xa5,sizeof(new_handle)); copy(&saved_handle,&new_handle,sizeof(saved_handle));
    CHECK(fat_new(&root,&request,&new_handle)==F_READONLY);
    CHECK(equal(&new_handle,&saved_handle,sizeof(new_handle)));
    CHECK(equal(file.object,&saved_object,sizeof(saved_object)));
    CHECK(fat_lookup(&identity,2,name,&entry)==F_OK);
    copy(&saved_entry,&entry,sizeof(entry)); transfer.done=99;
    CHECK(fat_write(&identity,&entry,&transfer)==F_READONLY && transfer.done==0);
    CHECK(fat_resize(&identity,&entry,2)==F_READONLY);
    CHECK(fat_set_info(&identity,&entry,&stamp)==F_READONLY);
    CHECK(fat_remove(&identity,&entry)==F_READONLY);
    CHECK(fat_rename(&identity,&entry,other)==F_READONLY);
    CHECK(equal(&entry,&saved_entry,sizeof(entry)));
    fill(&new_entry,0xa5,sizeof(new_entry)); copy(&saved_entry,&new_entry,sizeof(saved_entry));
    CHECK(fat_create(&identity,2,&request,&new_entry)==F_READONLY);
    CHECK(equal(&new_entry,&saved_entry,sizeof(new_entry)));
    fill(&order,0xa5,sizeof(order)); copy(&saved_order,&order,sizeof(order));
    CHECK(fat_order_init(&order,&ops,&staging)==F_READONLY);
    CHECK(equal(&order,&saved_order,sizeof(order)));
    for(unsigned verified=0;verified<2;++verified) {
        fill(&guarded,0xa5,sizeof(guarded));
        CHECK((verified?fat_order_commit_verified:fat_order_commit)(&order,&identity,&guarded.report)==F_READONLY);
        CHECK(guarded.report.status==F_READONLY && guarded.report.effect==FE_NONE);
        CHECK(!guarded.report.phase && !guarded.report.operation && !guarded.report.lba);
        CHECK(!guarded.report.writes && !guarded.report.flushes);
        CHECK(guarded.before==UINT64_C(0xa5a5a5a5a5a5a5a5) && guarded.after==guarded.before);
        CHECK(equal(&order,&saved_order,sizeof(order)));
    }
    transfer.done=99;
    call.target=(uintptr_t)fat_write_at; call.args[0]=(uintptr_t)&file;
    call.args[1]=(uintptr_t)&transfer; call.args[2]=call.args[3]=0;
    CHECK(fat_call_locked(&volume,&call)==F_READONLY && transfer.done==0 && !volume.gate);
    transfer.length=5;
    CHECK(fat_read_at(&file,&transfer)==F_OK && transfer.done==5 && equal(output,"hello",5));
    CHECK(reads && !writes && !begins && !ends && !flushes);
    CHECK(equal(original_data,data,SECTOR));
    CHECK(fat_close(&file)==F_OK && fat_close(&root)==F_OK && fat_volume_close(&volume)==F_OK);
    return 0;
#endif
}
