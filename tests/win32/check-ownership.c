static unsigned char *owner_data(Fixture *f,unsigned cluster) {
    return page(&f->disk,f->disk.data+(uint64_t)(cluster-2)*f->disk.spc,1)->data;
}
static void owner_link(Fixture *f,unsigned cluster,unsigned next) {
    fat_value(&f->disk,0,cluster,next); fat_value(&f->disk,1,cluster,next);
}
static void owner_tree(Fixture *f) {
    unsigned char *p=owner_data(f,2);
    raw_entry(p,"TESTING    ",0,0); p[11]=8;
    raw_entry(p+32,"SUB        ",3,0); p[43]=0x10;
    raw_entry(p+64,"KEEP    TXT",4,97);
    p=owner_data(f,3);
    raw_entry(p,".          ",3,0); p[11]=0x10;
    raw_entry(p+32,"..         ",0,0); p[43]=0x10;
    raw_entry(p+64,"ALPHA   BIN",5,2*f->id.cluster_bytes+17);
    raw_entry(p+96,"BETA    BIN",7,f->id.cluster_bytes+1);
    raw_entry(p+128,"EMPTY   BIN",0,0);
    for(unsigned c=2;c<=7;c++) owner_link(f,c,0x0fffffff);
    owner_link(f,5,9); owner_link(f,9,6); owner_link(f,7,11); owner_link(f,11,0x0fffffff);
}
static int ownership_call(Fixture *f,const FatOwnershipCheck *request,FatOwnershipReport *out) {
    struct { uint64_t before; FatOwnershipReport value; uint64_t after; } guard;
    FatOwnershipCheck unchanged=*request; int status;
    memset(&guard,0xa5,sizeof(guard)); f->id.dir_lba=f->id.fat_lba=UINT64_MAX;
    status=ABI(fat_check_ownership,&f->id,request,&guard.value,0);
    CHECK(guard.before==UINT64_C(0xa5a5a5a5a5a5a5a5) && guard.after==guard.before);
    CHECK(guard.value.check.status==(unsigned)status && guard.value.check.scope==FC_OWNERSHIP);
    CHECK(!memcmp(&unchanged,request,sizeof(unchanged)));
    *out=guard.value; return status;
}
static void test_check_ownership(void) {
    const unsigned sizes[][2]={{512,1},{512,128},{4096,16}};
    report("global ownership / physical owner locations / cycles vs cross-links / orphan and bad-cluster inventory");
    for(unsigned geometry=0;geometry<3;geometry++) {
        Fixture *f=fixture(sizes[geometry][0],sizes[geometry][1]);
        FatOwner *memory=malloc((f->id.cluster_count+2)*sizeof(*memory));
        struct { uint64_t before; FatDirectoryTask dirs[4]; uint64_t after; } tasks;
        FatOwnershipCheck request={memory+1,tasks.dirs,f->id.cluster_count,4,f->id.cluster_count+100,100,0};
        FatOwnershipReport c; FatVolume v={0}; FatObject pool[3],saved[3]; FatHandle root={0};
        uint64_t sub_lba=f->disk.data+f->disk.spc; unsigned char *p; unsigned i;
        CHECK(memory); memset(memory,0xa5,(f->id.cluster_count+2)*sizeof(*memory)); memset(&tasks,0xa5,sizeof(tasks));
        owner_tree(f); OK(fat_volume_init(&v,&f->id,pool,3)); OK(fat_root(&v,3,&root)); memcpy(saved,pool,sizeof(saved));
        OK(ownership_call(f,&request,&c));
        CHECK(c.check.count==8 && c.files==4 && c.directories==2 && c.slots==10);
        CHECK(c.check.examined==f->id.cluster_count && c.free_clusters==f->id.cluster_count-8);
        CHECK(!c.bad_clusters && !c.orphan_clusters && !c.check.flags && c.check.sector==UINT64_MAX);
        CHECK(request.owners[0].sector==UINT32_MAX && request.owners[0].offset==UINT32_MAX);
        CHECK(request.owners[1].sector==f->disk.data && request.owners[1].offset==32);
        CHECK(request.owners[3].sector==sub_lba && request.owners[3].offset==64);
        CHECK(request.owners[7].sector==sub_lba && request.owners[7].offset==64);
        CHECK(fat_check_fresh(&root,&c.check)==F_STALE && !memcmp(saved,pool,sizeof(saved)));
        owner_link(f,20,0xaffffff7); OK(ownership_call(f,&request,&c));
        CHECK(c.bad_clusters==1 && c.free_clusters==f->id.cluster_count-9);
        owner_link(f,21,0xb0000016); owner_link(f,22,0xcfffffff);
        CHECK(ownership_call(f,&request,&c)==F_CORRUPT && c.check.issue==FC_ORPHAN && c.check.flags==FC_ORPHANS);
        CHECK(c.orphan_clusters==2 && c.check.cluster==21 && c.check.observed==2 && c.check.expected==0);
        CHECK(c.check.count+c.free_clusters+c.bad_clusters+c.orphan_clusters==f->id.cluster_count);
        CHECK(c.check.sector==f->id.fat_start && c.owner_sector==UINT64_MAX && c.other_sector==UINT64_MAX);
        owner_link(f,20,0); owner_link(f,21,0); owner_link(f,22,0);
        p=owner_data(f,3); wr16(p+96+26,9);
        CHECK(ownership_call(f,&request,&c)==F_CORRUPT && c.check.issue==FC_CROSSLINK && c.check.cluster==9);
        CHECK(c.owner_sector==sub_lba && c.owner_offset==96 && c.other_sector==sub_lba && c.other_offset==64);
        wr16(p+96+26,7); owner_link(f,6,9);
        CHECK(ownership_call(f,&request,&c)==F_CORRUPT && c.check.issue==FC_CYCLE && c.check.cluster==9);
        CHECK(c.owner_sector==sub_lba && c.owner_offset==64);
        owner_link(f,6,0x0fffffff); owner_link(f,5,0x0fffffff);
        CHECK(ownership_call(f,&request,&c)==F_CORRUPT && c.check.issue==FC_SHORT && c.check.observed==1 && c.check.expected==3);
        wr16(p+64+26,0);
        CHECK(ownership_call(f,&request,&c)==F_CORRUPT && c.check.issue==FC_SHORT && c.check.cluster==0);
        wr16(p+64+26,5);
        for(i=0;i<5;i++) {
            const unsigned bad[]={0,1,0x0ffffff0,0x0ffffff7,65532};
            owner_link(f,5,bad[i]);
            CHECK(ownership_call(f,&request,&c)==F_CORRUPT && c.check.issue==FC_LINK && c.check.cluster==5);
            CHECK(c.check.observed==bad[i]);
        }
        owner_link(f,5,9); wr16(p+96+26,2);
        CHECK(ownership_call(f,&request,&c)==F_CORRUPT && c.check.issue==FC_CROSSLINK && c.other_sector==UINT64_MAX);
        wr16(p+96+26,7); wr32(p+64+28,1);
        OK(ownership_call(f,&request,&c)); CHECK(c.check.flags==FC_EXTRA && c.check.count==8);
        wr32(p+64+28,2*f->id.cluster_bytes+17);
        /* Nested directory back-reference is a cross-link, not another BFS task. */
        raw_entry(p+160,"LOOP       ",2,0); p[171]=0x10;
        CHECK(ownership_call(f,&request,&c)==F_CORRUPT && c.check.issue==FC_CROSSLINK && c.check.cluster==2);
        memset(p+160,0,32); wr16(p+32+26,2);
        CHECK(ownership_call(f,&request,&c)==F_CORRUPT && c.check.issue==FC_DOT && c.check.expected==0);
        wr16(p+32+26,0); p[0]=0;
        CHECK(ownership_call(f,&request,&c)==F_CORRUPT && c.check.issue==FC_DOT);
        p[0]='.';
        CHECK(memory[0].sector==0xa5a5a5a5 && memory[f->id.cluster_count+1].offset==0xa5a5a5a5);
        CHECK(tasks.before==UINT64_C(0xa5a5a5a5a5a5a5a5) && tasks.after==tasks.before);
        CHECK(!memcmp(saved,pool,sizeof(saved)) && !f->disk.writes && !f->buffer.pages);
        {
            FatHandle pending={0}; FatCreate make={U("PENDING.BIN"),0,0};
            unsigned char byte=0x5a; FatTransfer transfer={&byte,0,1,0};
            OK(fat_new(&root,&make,&pending)); OK(fat_write_at(&pending,&transfer));
            CHECK(transfer.done==1 && f->buffer.pages && !f->disk.writes);
            OK(ownership_call(f,&request,&c));
            CHECK(c.check.count==9 && c.files==5 && c.free_clusters==f->id.cluster_count-9);
            CHECK(!c.orphan_clusters && owner_data(f,2)[96]==0); /* raw backend still has no new SFN */
            OK(fat_close(&pending));
        }
        OK(fat_close(&root)); OK(fat_volume_close(&v)); free(memory); destroy(f);
    }
}
static void test_check_ownership_limits(void) {
    Fixture *f=fixture(512,1); FatOwner *owners=malloc(f->id.cluster_count*sizeof(*owners));
    FatDirectoryTask tasks[3]; FatOwnershipReport c; uint64_t before,reads; unsigned i;
    FatOwnershipCheck request={owners,tasks,f->id.cluster_count,3,f->id.cluster_count+10,100,0};
    unsigned char saved[512]; CHECK(owners);
    report("ownership exact budgets/capacity / fragmented directories / every read failure / selected FAT and early markers");
    memset(owners,0xa5,f->id.cluster_count*sizeof(*owners)); before=f->provider_reads;
    request.owner_capacity--; CHECK(ownership_call(f,&request,&c)==F_MEMORY && c.check.issue==FC_WORKSPACE);
    request.owner_capacity++; request.directory_capacity=0;
    CHECK(ownership_call(f,&request,&c)==F_MEMORY && c.check.expected==1);
    request.directory_capacity=3; request.reserved=1;
    CHECK(ownership_call(f,&request,&c)==F_ARGUMENT); request.reserved=0;
    request.owners=NULL; CHECK(ownership_call(f,&request,&c)==F_ARGUMENT);
    request.owners=(FatOwner *)(uintptr_t)(UINT64_MAX-7);
    CHECK(ownership_call(f,&request,&c)==F_RANGE); request.owners=owners;
    request.fat_budget=0; CHECK(ownership_call(f,&request,&c)==F_LIMIT && c.check.issue==FC_BUDGET);
    CHECK(f->provider_reads==before && owners[0].sector==0xa5a5a5a5);
    request.fat_budget=f->id.cluster_count-1;
    CHECK(ownership_call(f,&request,&c)==F_LIMIT && c.check.examined==request.fat_budget);
    request.fat_budget=f->id.cluster_count; request.slot_budget=1;
    OK(ownership_call(f,&request,&c)); CHECK(c.check.count==1 && c.slots==1 && c.check.examined==request.fat_budget);
    request.slot_budget=0; CHECK(ownership_call(f,&request,&c)==F_LIMIT && c.check.issue==FC_SLOT_BUDGET && !c.slots);
    request.slot_budget=100; request.fat_budget=f->id.cluster_count+10;
    owner_tree(f); request.directory_capacity=1;
    CHECK(ownership_call(f,&request,&c)==F_MEMORY && c.check.expected==2 && c.directories==1);
    request.directory_capacity=2; OK(ownership_call(f,&request,&c));
    request.slot_budget=9; CHECK(ownership_call(f,&request,&c)==F_LIMIT && c.check.issue==FC_SLOT_BUDGET && c.slots==9);
    request.slot_budget=10; OK(ownership_call(f,&request,&c));
    memcpy(saved,owner_data(f,2),512); memset(owner_data(f,2),0xe5,512);
    memcpy(owner_data(f,20),saved,512); owner_link(f,2,20); owner_link(f,20,0x0fffffff);
    request.slot_budget=26; before=f->provider_reads;
    OK(ownership_call(f,&request,&c)); reads=f->provider_reads-before;
    CHECK(c.check.count==9 && c.slots==26 && c.check.examined==f->id.cluster_count+1);
    CHECK(reads>500 && reads<600);
    for(i=0;i<reads;i++) {
        f->disk.fail_read=(int)i;
        CHECK(ownership_call(f,&request,&c)==F_IO && c.check.issue==FC_IO && c.check.sector!=UINT64_MAX);
        CHECK(!f->disk.writes);
    }
    f->disk.fail_read=-1;
    /* Corruption in the inactive copy is outside the selected-FAT scope. */
    f->id.mirrored=0; f->id.active_fat=1; fat_value(&f->disk,0,5,0);
    OK(ownership_call(f,&request,&c)); fat_value(&f->disk,1,5,0);
    CHECK(ownership_call(f,&request,&c)==F_CORRUPT && c.check.issue==FC_LINK);
    CHECK(c.check.sector==f->id.fat_start+f->id.fat_sectors);
    fat_value(&f->disk,0,5,9); fat_value(&f->disk,1,5,9);
    f->id.active_fat=0; f->id.mirrored=1;
    /* Unknown LFN extensions may carry semantics this ownership walk lacks. */
    memset(owner_data(f,20)+96,0,32); owner_data(f,20)[96]=0x41;
    owner_data(f,20)[107]=0x0f; owner_data(f,20)[108]=1;
    CHECK(ownership_call(f,&request,&c)==F_ATTENTION && c.check.issue==FC_EXTENSION);
    owner_data(f,20)[108]=0; wr16(owner_data(f,20)+96+26,7);
    CHECK(ownership_call(f,&request,&c)==F_ATTENTION && c.check.observed==7);
    memset(owner_data(f,20)+96,0,32);
    /* A zero marker hides later directory records, but not allocated tail ownership. */
    owner_data(f,20)[0]=0;
    CHECK(ownership_call(f,&request,&c)==F_CORRUPT && c.check.issue==FC_ORPHAN && c.check.count==2);
    CHECK(c.orphan_clusters==7 && c.directories==1 && !c.files);
    owner_link(f,2,0x0fffffff); memset(owner_data(f,2),0xe5,512);
    request.slot_budget=16;
    CHECK(ownership_call(f,&request,&c)==F_LIMIT && c.check.issue==FC_SLOT_BUDGET && c.slots==16);
    request.slot_budget=17;
    CHECK(ownership_call(f,&request,&c)==F_CORRUPT && c.check.issue==FC_ORPHAN && c.slots==16);
    CHECK(c.orphan_clusters==8 && c.check.count==1 && c.check.examined==f->id.cluster_count+1);
    f->id.transaction=1; before=f->provider_reads;
    CHECK(ownership_call(f,&request,&c)==F_BUSY && f->provider_reads==before);
    f->id.transaction=0; f->id.magic=0;
    CHECK(ownership_call(f,&request,&c)==F_FORMAT && f->provider_reads==before);
    CHECK(!f->disk.writes && !f->buffer.pages); free(owners); destroy(f);
}
