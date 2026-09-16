/* Formatter oracle reads raw sectors and independently computes capacity.
   Physical callbacks are deliberately not the transaction-buffer provider. */
extern int abi_flush(void *);
typedef struct FormatFixture {
    Fixture *base;
    SectorOps direct,poison;
    FatFormatOptions options;
    FatFormatPlan plan;
    FatFormatReport result;
    FatWorkspace work;
    unsigned char guarded[2*4096+32];
    uint64_t calls,writes,flushes,reads,fail;
    uint32_t mismatch;
} FormatFixture;
static int format_fault(FormatFixture *f) { return ++f->calls==f->fail; }
static int format_read(void *context,uint64_t lba,void *out) {
    FormatFixture *f=context; ++f->reads;
    CHECK(lba<f->direct.sectors);
    if(format_fault(f)) return F_IO;
    int status=read_image(&f->base->disk,lba,out);
    if(f->mismatch && f->reads==f->mismatch) ((unsigned char *)out)[17]^=1;
    return status;
}
static int format_write(void *context,uint64_t lba,const void *data) {
    FormatFixture *f=context; ++f->writes;
    CHECK(lba<f->direct.sectors);
    if(format_fault(f)) {
        /* A failed callback may already have changed the physical medium. */
        memcpy(page(&f->base->disk,lba,1)->data,data,f->direct.sector_bytes/2);
        return F_IO;
    }
    return write_image(&f->base->disk,lba,data);
}
static int format_flush(void *context) {
    FormatFixture *f=context; ++f->flushes; return format_fault(f)?F_IO:F_OK;
}
static FormatFixture *format_fixture(unsigned bps,unsigned cluster_bytes) {
    FormatFixture *f=calloc(1,sizeof(*f)); CHECK(f);
    f->base=fixture(bps,cluster_bytes/bps);
    f->base->id.magic=0; /* Retire the old mount before exclusive formatting. */
    f->base->disk.ops.sectors+=32*(cluster_bytes/bps);
    f->direct=(SectorOps){f,format_read,format_write,0,0,format_flush,
                         f->base->disk.ops.sectors,bps,0};
    f->poison=f->direct; f->poison.context=&f->direct;
    f->poison.read=abi_read; f->poison.write=abi_write; f->poison.flush=abi_flush;
    f->options=(FatFormatOptions){cluster_bytes,0x1234abcd,2048,0,{0},{0}};
    memcpy(f->options.label,"New Volume ",11);
    memset(f->guarded,0xa5,sizeof(f->guarded));
    f->work=(FatWorkspace){f->guarded+16,bps*2,0};
    OK(ABI(fat_format_plan,&f->poison,&f->options,&f->plan,0));
    CHECK(!f->calls);
    /* Pre-existing data outside the quick-format target must survive. */
    memset(page(&f->base->disk,f->plan.data_start+f->plan.cluster_sectors,1)->data,0x7c,bps);
    memset(page(&f->base->disk,f->direct.sectors,1)->data,0x39,bps);
    return f;
}
static void format_destroy(FormatFixture *f) { destroy(f->base); free(f); }
static int format_run(FormatFixture *f,unsigned verified) {
    return ABI(verified?fat_format_verified:fat_format,&f->poison,&f->options,&f->work,&f->result);
}
static void format_oracle(FormatFixture *f,unsigned verified) {
    Image *d=&f->base->disk; unsigned bps=d->bytes;
    unsigned char *boot=page(d,0,0)->data,*backup=page(d,6,0)->data,*info=page(d,1,0)->data;
    CHECK(!memcmp(boot,backup,bps)); CHECK(rd16(boot+510)==0xaa55);
    CHECK(rd16(boot+11)==bps && boot[13]==f->options.cluster_bytes/bps);
    CHECK(rd16(boot+14)==32 && boot[16]==2 && !rd16(boot+17) && !rd16(boot+19) && !rd16(boot+22));
    CHECK(rd32(boot+28)==2048 && rd32(boot+32)==f->direct.sectors);
    CHECK(rd32(boot+67)==f->options.serial && !memcmp(boot+82,"FAT32   ",8));
    uint32_t fats=rd32(boot+36),spc=boot[13],start=32+2*fats;
    uint32_t clusters=((uint32_t)f->direct.sectors-start)/spc;
    CHECK(clusters>=65541 && (uint64_t)fats*bps/4>=clusters+2);
    CHECK((uint64_t)(fats-1)*bps/4<((uint32_t)f->direct.sectors-32-2*(fats-1))/spc+2);
    CHECK(rd32(boot+44)==2 && rd16(boot+48)==1 && rd16(boot+50)==6);
    CHECK(!memcmp(info,page(d,7,0)->data,bps));
    CHECK(rd32(info)==0x41615252 && rd32(info+484)==0x61417272 && rd32(info+508)==0xaa550000);
    CHECK(rd32(info+488)==clusters-1 && rd32(info+492)==3);
    for(unsigned sector=0;sector<fats;++sector) {
        const unsigned char *a=page(d,32+sector,0)->data,*b=page(d,32+fats+sector,0)->data;
        CHECK(!memcmp(a,b,bps));
        for(unsigned index=0;index<bps/4;++index) {
            unsigned entry=sector*bps/4+index;
            CHECK(rd32(a+index*4)==(entry==0?0x0ffffff8u:entry<3?0x0fffffffu:0u));
        }
    }
    const unsigned char *root=page(d,start,0)->data;
    if(f->options.label[0]) {
        CHECK(!memcmp(boot+71,"NEW VOLUME ",11) && !memcmp(root,boot+71,11) && root[11]==8);
    } else CHECK(!memcmp(boot+71,"NO NAME    ",11) && !root[0]);
    for(unsigned sector=0;sector<spc;++sector) {
        const unsigned char *p=page(d,start+sector,0)->data;
        for(unsigned i=(sector==0 && f->options.label[0])?12:0;i<bps;++i) CHECK(!p[i]);
    }
    for(unsigned i=0;i<bps;++i) {
        CHECK(page(d,start+spc,0)->data[i]==0x7c);
        CHECK(page(d,f->direct.sectors,0)->data[i]==0x39);
    }
    CHECK(f->result.status==F_OK && f->result.effect==FE_COMMITTED);
    CHECK(f->writes==f->plan.writes && f->result.writes==f->writes && f->result.completed_writes==f->writes);
    CHECK(f->flushes==4+(verified?f->writes:0) && f->result.flushes==f->flushes);
    CHECK(f->result.completed_flushes==f->flushes && f->reads==(verified?f->writes:0));
    CHECK(f->result.reads==f->reads && f->result.phase==FF_PRIMARY);
    for(unsigned i=0;i<16;++i) CHECK(f->guarded[i]==0xa5 && f->guarded[16+f->work.bytes+i]==0xa5);
}
static void test_format(void) {
    report("FAT32 format / raw geometry and allocation oracle / labels / quick extent / ABI");
    for(unsigned bps=512;bps<=4096;bps*=2)
    for(unsigned cb=bps;cb<=262144 && cb/bps<=128;cb*=2)
    for(unsigned verified=0;verified<2;++verified) {
        FormatFixture *f=format_fixture(bps,cb);
        unsigned g=(cb/bps)&0x55;
        if(g&1) memset(f->options.label,0,11);
        f->work.bytes=f->direct.sector_bytes*(verified?2:1);
        OK(format_run(f,verified)); format_oracle(f,verified);
        /* Re-mount the produced filesystem and use the default shared API. */
        FatIdentity identity={0}; FatVolume volume={0}; FatObject pool[3]={0}; FatHandle root={0},file={0};
        unsigned char scratch[3*4096],data[1234],out[1234];
        FatWorkspace work={scratch,3*f->direct.sector_bytes,0};
        OK(sb_init(&f->base->buffer,&f->base->disk.ops));
        OK(fat_mount(&identity,&f->base->buffer.ops,0,&work));
        OK(fat_volume_init(&volume,&identity,pool,3)); OK(fat_root(&volume,FH_READ|FH_WRITE,&root));
        FatCreate create_request={U("format check.bin"),0,0}; OK(fat_new(&root,&create_request,&file));
        memset(data,0x65,sizeof(data)); FatTransfer t={data,0,sizeof(data),0}; OK(fat_write_at(&file,&t));
        t=(FatTransfer){out,0,sizeof(out),0}; OK(fat_read_at(&file,&t)); CHECK(!memcmp(data,out,sizeof(data)));
        OK(fat_close(&file)); OK(fat_close(&root)); OK(fat_volume_close(&volume));
        format_destroy(f);
    }
}
static void test_format_admission(void) {
    report("formatter rejection / planning boundaries / no callback before admission");
    FormatFixture *f=format_fixture(512,4096); FatFormatPlan plan;
    uint64_t original=f->poison.sectors;
    for(unsigned i=0;i<32;++i) {
        f->poison.sectors=i; CHECK(fat_format_plan(&f->poison,&f->options,&plan)==F_FORMAT);
    }
    f->poison.sectors=UINT64_MAX; CHECK(fat_format_plan(&f->poison,&f->options,&plan)==F_RANGE);
    f->poison.sectors=original;
    for(unsigned size=0;size<=131072;size=size?size*2:1) {
        if(size>=512 && size<=65536) continue;
        f->options.cluster_bytes=size; CHECK(format_run(f,0)==F_FORMAT && f->result.effect==FE_NONE);
    }
    f->options.cluster_bytes=4096;
    f->options.label[2]='/'; CHECK(format_run(f,0)==F_NAME); f->options.label[2]='w';
    f->options.label[2]=0; CHECK(format_run(f,0)==F_NAME); f->options.label[2]='w';
    f->options.reserved=1; CHECK(format_run(f,0)==F_ARGUMENT); f->options.reserved=0;
    f->options.reserved_label[4]=1; CHECK(format_run(f,0)==F_ARGUMENT); f->options.reserved_label[4]=0;
    f->work.bytes=511; CHECK(format_run(f,0)==F_MEMORY);
    f->work.bytes=512; CHECK(format_run(f,1)==F_MEMORY);
    f->work.bytes=1024; f->work.data=0; CHECK(format_run(f,0)==F_ARGUMENT);
    f->work.data=(void *)(uintptr_t)(UINT64_MAX-100); CHECK(format_run(f,0)==F_ARGUMENT);
    f->work.data=f->guarded+16; f->work.reserved=1; CHECK(format_run(f,0)==F_ARGUMENT); f->work.reserved=0;
    f->poison.write=0; CHECK(format_run(f,0)==F_READONLY); f->poison.write=abi_write;
    f->poison.flush=0; CHECK(format_run(f,0)==F_READONLY); f->poison.flush=abi_flush;
    f->poison.read=0; CHECK(format_run(f,1)==F_ARGUMENT); f->poison.read=abi_read;
    f->poison.begin=abi_begin; CHECK(format_run(f,0)==F_ARGUMENT); f->poison.begin=0;
    f->poison.end=abi_end; CHECK(format_run(f,0)==F_ARGUMENT); f->poison.end=0;
    f->poison.reserved=80; CHECK(format_run(f,0)==F_ARGUMENT); f->poison.reserved=0;
    CHECK(!f->calls && !f->result.writes && f->result.effect==FE_NONE);
    /* Find the first admissible extent without relying on the production
       formula, and verify the exact cluster-classification boundary. */
    f->poison=f->direct;
    for(unsigned cb=512;cb<=65536;cb*=2) {
        f->options.cluster_bytes=cb;
        uint64_t lo=32,hi=32+2*1024+UINT64_C(65542)*(cb/512);
        while(lo<hi) {
            uint64_t mid=lo+(hi-lo)/2; f->poison.sectors=mid;
            if(fat_format_plan(&f->poison,&f->options,&plan)==F_OK) hi=mid; else lo=mid+1;
        }
        f->poison.sectors=lo; OK(fat_format_plan(&f->poison,&f->options,&plan));
        CHECK(plan.cluster_count==65541);
        f->poison.sectors=lo-1; CHECK(fat_format_plan(&f->poison,&f->options,&plan)==F_FORMAT);
        for(unsigned i=0;i<sizeof(plan);++i) CHECK(!((unsigned char *)&plan)[i]);
    }
    CHECK(!f->calls);
    format_destroy(f);
}
static void test_format_failures(void) {
    report("formatter every callback fault / half writes / honest counters / readback mismatch");
    for(unsigned verified=0;verified<2;++verified) {
        FormatFixture *f=format_fixture(4096,4096); OK(format_run(f,verified));
        uint64_t calls=f->calls; format_destroy(f);
        for(uint64_t fault=1;fault<=calls;++fault) {
            f=format_fixture(4096,4096); f->fail=fault;
            CHECK(format_run(f,verified)==F_IO);
            CHECK(f->result.status==F_IO && f->result.effect==FE_UNCERTAIN);
            CHECK(f->calls==fault && f->writes==f->result.writes && f->flushes==f->result.flushes && f->reads==f->result.reads);
            CHECK(f->result.completed_writes==f->writes-(f->result.operation==FI_WRITE));
            CHECK(f->result.completed_flushes==f->flushes-(f->result.operation==FI_FLUSH));
            CHECK(f->result.phase>=FF_INVALIDATE && f->result.phase<=FF_PRIMARY && f->result.lba<f->direct.sectors);
            /* Before either BPB publication, a successful first invalidation
               leaves the old primary unrecognizable, including all metadata faults. */
            if(fault>1 && f->result.phase<FF_PRIMARY) CHECK(!rd16(page(&f->base->disk,0,0)->data+510));
            CHECK(page(&f->base->disk,f->direct.sectors,0)->data[0]==0x39);
            format_destroy(f);
        }
    }
    FormatFixture *f=format_fixture(512,65536); f->mismatch=1;
    CHECK(format_run(f,1)==F_VERIFY);
    CHECK(f->result.operation==FI_VERIFY && f->result.effect==FE_UNCERTAIN);
    CHECK(f->calls==3 && f->writes==1 && f->reads==1 && f->flushes==1);
    format_destroy(f);
}
