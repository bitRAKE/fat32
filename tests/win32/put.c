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

static void test_put_lba_span(void) {
    /* High stride/end values in the first two layouts; later layouts also
       read and stage FAT copies at LBAs above INT32_MAX. */
    static const struct { unsigned copies; uint32_t sectors; } layouts[]={
        {1,0x80000101u},{2,0x40000101u},{3,0x50000101u},
        {16,0x0f000101u},{255,0x00f0f101u}
    };
    report("FAT hook LBA spans / oversized FATs / unsigned strides / 1..255 mirrors / active copy selection");
    for(unsigned layout=0;layout<sizeof(layouts)/sizeof(layouts[0]);++layout)
    for(unsigned selected=0;selected<2;++selected) for(unsigned strict=0;strict<2;++strict) {
        Fixture *f=fixture(512,1); Image *d=&f->disk;
        unsigned copies=layouts[layout].copies, active=selected?(copies<16?copies-1:15):0;
        unsigned touched=selected?1:copies;
        uint32_t cluster=d->clusters+1, offset=(cluster*4)%d->bytes;
        unsigned char out[512], *boot=page(d,0,0)->data;
        d->fats=copies; d->fat_sectors=layouts[layout].sectors;
        uint64_t data=32+(uint64_t)copies*d->fat_sectors;
        uint64_t total=data+d->clusters;
        CHECK(total<=UINT32_MAX && data>INT32_MAX);
        d->data=(uint32_t)data; d->ops.sectors=total;
        f->buffer.ops.sectors=f->inner.sectors=f->fault.sectors=total;
        boot[16]=(unsigned char)copies; wr32(boot+32,(uint32_t)total);
        wr32(boot+36,d->fat_sectors); wr16(boot+40,selected?0x80u+active:0);
        memcpy(page(d,6,1)->data,boot,d->bytes);
        for(unsigned copy=0;copy<copies;++copy) {
            fat_value(d,copy,cluster,((copy%16u)<<28)|7u);
            fat_value(d,copy,cluster-1,0x76543210u);
        }
        OK(fat_mount(&f->id,&f->fault,NULL,&f->workspace));
        CHECK(f->id.fat_count==copies && f->id.active_fat==active);
        CHECK(f->id.cluster_count==d->clusters);
        uint64_t reads=f->provider_reads;
        put_begin(f); f->fail_stage=(int)touched;
        OK(ABI(put_hooks[strict],&f->id,cluster,0x0ffffff7u,0));
        CHECK(f->provider_reads-reads==touched+strict && f->fail_stage==0);
        CHECK(f->buffer.pages==touched && f->id.fat_lba==UINT64_MAX);
        put_end(f,1);
        for(unsigned copy=0;copy<copies;++copy) {
            uint64_t lba=32+(uint64_t)copy*d->fat_sectors+(uint64_t)cluster*4/d->bytes;
            unsigned changed=!selected || copy==active;
            OK(sb_read(&f->buffer,lba,out));
            CHECK(rd32(out+offset)==(((copy%16u)<<28)|(changed?0x0ffffff7u:7u)));
            CHECK(rd32(out+offset-4)==0x76543210u);
        }
        CHECK(!d->writes); destroy(f);
    }
}
