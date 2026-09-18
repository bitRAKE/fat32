/* The allocation search cursor is in-memory state, independent of FSInfo.
   Count provider FAT reads, including staged sectors; cache hits cost none. */
static void test_allocation_progress(unsigned bytes,unsigned spc) {
    enum { APPENDS=6, FIRST=36095 };
    report("accepted appends retain allocation progress / snapshot and shared APIs");
    for(unsigned shared=0;shared<2;++shared) for(unsigned selected=0;selected<2;++selected) {
        Fixture *f=fixture(bytes,spc); FatEntry entry={0}; FatVolume volume={0}; FatObject pool[2];
        FatHandle root={0},file={0}; uint64_t reads[APPENDS]; uint32_t cursors[APPENDS];
        unsigned cb=bytes*spc; unsigned char *data=malloc(cb),*out=malloc(cb);
        CHECK(data && out);
        if(selected) { f->id.mirrored=0; f->id.active_fat=1; }
        /* The first free cluster crosses a FAT-sector boundary on append two
           with 512-byte sectors. The occupied prefix makes a reset expensive. */
        for(unsigned c=3;c<FIRST;++c) for(unsigned copy=0;copy<2;++copy)
            fat_value(&f->disk,copy,c,0x0fffffff);
        if(shared) {
            OK(fat_volume_init(&volume,&f->id,pool,2)); OK(fat_root(&volume,3,&root));
            shared_new(&root,U("APPEND.BIN"),0,&file);
        } else entry=create(f,2,U("APPEND.BIN"),0);
        for(unsigned i=0;i<APPENDS;++i) {
            memset(data,0x31+i,cb); FatTransfer t={data,i*cb,cb,0};
            uint64_t before=f->fat_reads;
            OK(shared?fat_write_at(&file,&t):fat_write(&f->id,&entry,&t)); CHECK(t.done==cb);
            reads[i]=f->fat_reads-before; cursors[i]=f->id.next_free;
        }
        printf("    sector=%u cluster=%u %s %s FAT reads:",bytes,cb,
               shared?"shared":"snapshot",selected?"active FAT 1":"mirrored");
        for(unsigned i=0;i<APPENDS;++i) printf(" %llu",(unsigned long long)reads[i]);
        printf("\n"); fflush(stdout);
        CHECK(reads[0]>=FIRST/(bytes/4));
        for(unsigned i=0;i<APPENDS;++i) {
            if(i) CHECK(reads[i]<=16); /* Independent of the occupied prefix. */
            CHECK(cursors[i]==FIRST+i+1);
            FatTransfer t={out,i*cb,cb,0};
            OK(shared?fat_read_at(&file,&t):fat_read(&f->id,&entry,&t)); CHECK(t.done==cb);
            memset(data,0x31+i,cb); CHECK(!memcmp(data,out,cb));
        }
        /* Public invalidation still handles external edits/remounts. */
        OK(fat_invalidate(&f->id)); CHECK(f->id.next_free==2 && f->id.free_hint==UINT32_MAX);
        CHECK(f->id.fat_lba==UINT64_MAX && f->id.dir_lba==UINT64_MAX);
        free(data); free(out); destroy(f);
    }
}

static void test_allocation_wrap(void) {
    report("allocation cursor wrap / bounded exhaustion / freed-cluster reuse");
    for(unsigned shared=0;shared<2;++shared) {
        Fixture *f=fixture(512,1); FatEntry entry={0}; FatVolume volume={0}; FatObject pool[2];
        FatHandle root={0},file={0}; unsigned last=f->disk.clusters+1; uint32_t chain[2];
        /* Exactly two free clusters, on opposite sides of the wrap. */
        for(unsigned c=4;c<last;++c) for(unsigned copy=0;copy<2;++copy)
            fat_value(&f->disk,copy,c,0x0fffffff);
        if(shared) {
            OK(fat_volume_init(&volume,&f->id,pool,2)); OK(fat_root(&volume,3,&root));
            shared_new(&root,U("WRAP.BIN"),0,&file);
        } else entry=create(f,2,U("WRAP.BIN"),0);
        f->id.next_free=last; /* Seed the allocator boundary, not disk FSInfo. */
        OK(shared?fat_handle_resize(&file,512):fat_resize(&f->id,&entry,512));
        CHECK(f->id.next_free==2);
        CHECK((shared?file.object->record.cluster:entry.cluster)==last);
        OK(shared?fat_handle_resize(&file,1024):fat_resize(&f->id,&entry,1024));
        CHECK(f->id.next_free==4);
        CHECK((shared?fat_handle_resize(&file,1536):fat_resize(&f->id,&entry,1536))==F_NOSPACE);
        CHECK(f->id.next_free==2 && f->id.fat_lba==UINT64_MAX && f->id.dir_lba==UINT64_MAX);
        /* Failed snapshot transactions retire the old entry; reacquire it. */
        if(!shared) entry=lookup(f,2,U("WRAP.BIN"));
        CHECK((shared?file.object->record.size:entry.size)==1024);
        OK(fat_chain(&f->id,last,chain)); CHECK(chain[0]==2 && chain[1]==3);
        OK(shared?fat_handle_resize(&file,0):fat_resize(&f->id,&entry,0));
        OK(shared?fat_handle_resize(&file,512):fat_resize(&f->id,&entry,512));
        CHECK(f->id.next_free==4);
        CHECK((shared?file.object->record.cluster:entry.cluster)==3);
        OK(shared?fat_handle_resize(&file,1024):fat_resize(&f->id,&entry,1024));
        CHECK(f->id.next_free==2);
        OK(fat_chain(&f->id,3,chain)); CHECK(chain[0]==2 && chain[1]==last);
        destroy(f);
    }
}

static void test_mutation_caches(void) {
    report("metadata/FSInfo writes retain unrelated FAT cache / FAT writes retain directory cache");
    for(unsigned shared=0;shared<2;++shared) {
        Fixture *f=fixture(512,1); FatEntry entry={0}; FatVolume volume={0}; FatObject pool[2];
        FatHandle root={0},file={0}; FatStamp stamp={0x1234,0x5821,0x5822,0x2345,0x5823,199,0x22};
        uint32_t value; unsigned char sector[512];
        if(shared) {
            OK(fat_volume_init(&volume,&f->id,pool,2)); OK(fat_root(&volume,3,&root));
            shared_new(&root,U("CACHE.BIN"),0,&file);
        } else entry=create(f,2,U("CACHE.BIN"),0);
        OK(shared?fat_handle_resize(&file,512):fat_resize(&f->id,&entry,512));
        uint32_t cursor=f->id.next_free;
        OK(fat_get(&f->id,600,&value)); CHECK(!value);
        uint64_t key=f->id.fat_lba,reads=f->fat_reads;
        OK(shared?fat_handle_set_info(&file,&stamp):fat_set_info(&f->id,&entry,&stamp));
        CHECK(f->id.next_free==cursor && f->id.free_hint==UINT32_MAX);
        CHECK(f->id.fat_lba==key && f->id.dir_lba==UINT64_MAX);
        OK(fat_get(&f->id,600,&value)); CHECK(!value && f->fat_reads==reads);
        entry=lookup(f,2,U("CACHE.BIN")); CHECK(entry.raw[11]==stamp.attributes && rd16(entry.raw+14)==stamp.create_time);
        for(unsigned copy=0;copy<2;++copy) {
            OK(sb_read(&f->buffer,copy?7:1,sector));
            CHECK(rd32(sector+488)==UINT32_MAX && rd32(sector+492)==UINT32_MAX);
        }
        destroy(f);
    }
    for(unsigned strict=0;strict<2;++strict) {
        Fixture *f=fixture(512,1); FatEntry entry=create(f,2,U("CACHE.BIN"),0);
        entry=lookup(f,2,U("CACHE.BIN")); uint64_t key=f->id.dir_lba;
        put_begin(f); OK(put_hooks[strict](&f->id,600,0x0fffffff));
        CHECK(f->id.fat_lba==UINT64_MAX && f->id.dir_lba==key);
        uint64_t reads=f->provider_reads;
        entry=lookup(f,2,U("CACHE.BIN")); CHECK(entry.size==0 && f->provider_reads==reads);
        put_end(f,0); destroy(f);
    }
}
