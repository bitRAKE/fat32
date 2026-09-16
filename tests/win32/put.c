/* Advanced mutation hooks: test transaction ownership explicitly, without
   giving these low-level edits the semantics of a file operation. */
typedef int (*PutHook)(FatIdentity *,uint32_t,uint32_t);
static const PutHook put_hooks[]={fat_put,fat_put_checked};
static void put_begin(Fixture *f) {
    OK(sb_begin(&f->buffer)); f->id.transaction=1;
}
static void put_end(Fixture *f,int accept) {
    sb_end(&f->buffer,accept); f->id.transaction=0;
    f->id.fat_lba=f->id.dir_lba=UINT64_MAX;
}
static void test_put(void) {
    static const unsigned geometry[][2]={{512,1},{512,128},{4096,16}};
    report("basic/checked FAT hooks / exact reads / high bits / selected copies / ABI");
    for(unsigned g=0;g<3;++g) for(unsigned strict=0;strict<2;++strict)
    for(unsigned copies=0;copies<3;++copies) {
        Fixture *f=fixture(geometry[g][0],geometry[g][1]);
        uint32_t c=f->disk.bytes/4; unsigned char out[4096];
        unsigned selected=copies==0?1:0, count=copies==2?2:1;
        if(copies==0) { f->id.mirrored=0; f->id.active_fat=1; }
        if(copies==1) f->id.fat_count=1;
        for(unsigned i=0;i<2;++i) {
            fat_value(&f->disk,i,c,((i+10u)<<28)|7);
            fat_value(&f->disk,i,c+1,0x76543210);
        }
        uint64_t reads=f->provider_reads; f->fail_stage=(int)count;
        put_begin(f);
        OK(ABI(put_hooks[strict],&f->id,c,0x0ffffff7,0));
        CHECK(f->provider_reads-reads==count+strict && !f->fail_stage);
        CHECK(!f->disk.writes && f->id.transaction==1 && f->id.fat_lba==UINT64_MAX);
        put_end(f,1);
        for(unsigned i=0;i<2;++i) {
            OK(sb_read(&f->buffer,33+(uint64_t)i*f->disk.fat_sectors,out));
            CHECK(rd32(out)==(((i+10u)<<28)|((i>=selected && i<selected+count)?0x0ffffff7:7)));
            CHECK(rd32(out+4)==0x76543210);
        }
        /* Admission errors do not read or stage, and required bounds remain
           in the basic hook even though mirror comparison is optional. */
        reads=f->provider_reads;
        CHECK(ABI(put_hooks[strict],&f->id,c,0,0)==F_BUSY);
        put_begin(f);
        CHECK(ABI(put_hooks[strict],&f->id,c,0xf0000000,0)==F_ARGUMENT);
        CHECK(ABI(put_hooks[strict],&f->id,f->id.cluster_count+2,0,0)==F_CORRUPT);
        CHECK(ABI(put_hooks[strict],&f->id,UINT32_MAX,0,0)==F_CORRUPT);
        f->id.magic=0;
        CHECK(ABI(put_hooks[strict],&f->id,c,0,0)==F_CORRUPT);
        CHECK(f->provider_reads==reads && !f->fail_stage);
        put_end(f,0); destroy(f);
    }
    /* A disagreement is only examined by the selected checked operation.
       Its staged first copy must be rolled back by the outer transaction. */
    for(unsigned strict=0;strict<2;++strict) {
        Fixture *f=fixture(512,1); unsigned char out[512];
        fat_value(&f->disk,0,3,0xa0000004); fat_value(&f->disk,1,3,0xb0000005);
        put_begin(f); int status=ABI(put_hooks[strict],&f->id,3,0x0fffffff,0);
        CHECK(status==(strict?F_CORRUPT:F_OK)); put_end(f,status==F_OK);
        for(unsigned i=0;i<2;++i) {
            OK(sb_read(&f->buffer,32+(uint64_t)i*f->disk.fat_sectors,out));
            CHECK(rd32(out+12)==(((i+10u)<<28)|(strict?4+i:0x0fffffff)));
        }
        CHECK(!f->disk.writes); destroy(f);
    }
}
static void test_put_failures(void) {
    report("FAT hook read/staging faults / outer rollback / prior accepted state");
    for(unsigned strict=0;strict<2;++strict) for(unsigned writing=0;writing<2;++writing)
    for(unsigned fail=0;fail<(writing?2:2+strict);++fail) {
        Fixture *f=fixture(512,1); unsigned char out[512],prior[512];
        fat_value(&f->disk,0,128,0xa0000007); fat_value(&f->disk,1,128,0xb0000007);
        memset(prior,0x5a,sizeof(prior));
        OK(sb_begin(&f->buffer)); OK(sb_write(&f->buffer,f->disk.data+1,prior)); sb_end(&f->buffer,1);
        uint64_t pages=f->buffer.pages;
        if(writing) f->fail_stage=(int)fail; else f->disk.fail_read=(int)fail;
        put_begin(f);
        CHECK(ABI(put_hooks[strict],&f->id,128,0x0fffffff,0)==F_IO);
        CHECK(f->id.transaction==1 && f->id.fat_lba==UINT64_MAX);
        put_end(f,0); f->disk.fail_read=-1; f->fail_stage=-1;
        CHECK(f->buffer.pages==pages && !f->disk.writes);
        for(unsigned i=0;i<2;++i) {
            OK(sb_read(&f->buffer,33+(uint64_t)i*f->disk.fat_sectors,out));
            CHECK(rd32(out)==(((i+10u)<<28)|7));
        }
        OK(sb_read(&f->buffer,f->disk.data+1,out)); CHECK(!memcmp(out,prior,sizeof(out)));
        destroy(f);
    }
}
