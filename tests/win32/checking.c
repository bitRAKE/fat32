/* Optional structural checks use independently encoded media and the same
   hostile ABI callbacks as the ordinary shared interface. */
static void test_check_chain(void) {
    Fixture *f=fixture(512,1); FatCursor cursor,before; FatEntry entry; FatCheck check;
    uint64_t reads=f->provider_reads; unsigned i;
    report("geometry-only mount / no-I/O cursor / bounded optional chain diagnostics");
    OK(ABI(fat_mount,&f->id,&f->fault,NULL,&f->workspace)); CHECK(f->provider_reads==reads+1);
    reads=f->provider_reads; memset(&cursor,0xA5,sizeof(cursor)); before=cursor;
    CHECK(fat_dir_open(&f->id,1,&cursor)==F_CORRUPT && !memcmp(&cursor,&before,sizeof(cursor)));
    CHECK(fat_dir_open(&f->id,f->id.cluster_count+2,&cursor)==F_CORRUPT);
    OK(ABI(fat_dir_open,&f->id,2,&cursor,0)); CHECK(f->provider_reads==reads);
    /* End marker means iteration need not inspect a malformed unused tail. */
    fat_value(&f->disk,0,2,2);
    CHECK(fat_dir_next(&f->id,&cursor,&entry)==F_END);
    CHECK(ABI(fat_check_chain,&f->id,2,8,&check)==F_CORRUPT);
    CHECK(check.issue==FC_CYCLE && check.cluster==2 && check.observed==2 && check.examined==1);
    OK(fat_invalidate(&f->id));
    for(i=5;i<10;i++) fat_value(&f->disk,0,i,i+1);
    fat_value(&f->disk,0,10,0xFFFFFFF);
    reads=f->provider_reads;
    CHECK(ABI(fat_check_chain,&f->id,5,0,&check)==F_LIMIT);
    CHECK(check.issue==FC_BUDGET && check.count==0 && f->provider_reads==reads);
    CHECK(fat_check_chain(&f->id,5,3,&check)==F_LIMIT && check.count==3 && check.last==0);
    OK(ABI(fat_check_chain,&f->id,5,6,&check)); CHECK(check.count==6 && check.last==10);
    OK(fat_check_chain(&f->id,0,0,&check)); CHECK(check.count==0 && check.last==0);
    CHECK(fat_check_chain(&f->id,1,8,&check)==F_CORRUPT && check.issue==FC_LINK);
    OK(fat_invalidate(&f->id)); f->disk.fail_read=0;
    CHECK(fat_check_chain(&f->id,5,6,&check)==F_IO);
    CHECK(check.issue==FC_IO && check.sector==32 && check.cluster==5 && check.examined==0);
    f->disk.fail_read=-1; f->id.transaction=1;
    CHECK(fat_check_chain(&f->id,5,6,&check)==F_BUSY); f->id.transaction=0;
    destroy(f);
}

static void test_check_file(void) {
    Fixture *f=fixture(512,1); FatVolume v={0}; FatObject pool[5];
    FatHandle root={0},a={0},alias={0},b={0}; FatCheck check={0}; FatCheckedRead policy={&check,8,0};
    FatTransfer t; uint64_t reads; unsigned char out[1200],data[1200];
    report("scoped chain evidence / checked read reuse / fault refusal / handle and mount lifetime");
    memset(data,0x39,sizeof(data)); OK(fat_volume_init(&v,&f->id,pool,5)); OK(fat_root(&v,3,&root));
    shared_new(&root,U("A"),0,&a); shared_new(&root,U("B"),0,&b);
    t=(FatTransfer){data,0,sizeof(data),0}; OK(fat_write_at(&a,&t));
    OK(fat_open(&root,U("A"),3,&alias));
    CHECK(fat_check_fresh(&a,&check)==F_STALE);
    OK(ABI(fat_check_file,&a,8,&check,0)); CHECK(check.count==3 && check.expected==3);
    OK(ABI(fat_check_fresh,&alias,&check,0,0));
    t=(FatTransfer){out,0,1,99}; reads=f->provider_reads;
    OK(ABI(fat_read_checked,&alias,&t,&policy,0)); CHECK(t.done==1 && out[0]==0x39 && f->provider_reads==reads+1);
    OK(fat_handle_resize(&b,1600)); OK(fat_check_fresh(&a,&check));
    OK(fat_handle_rename(&a,U("renamed"))); OK(fat_check_fresh(&alias,&check));
    t=(FatTransfer){data,2,1,0}; OK(fat_write_at(&a,&t)); OK(fat_check_fresh(&a,&check));
    OK(fat_handle_resize(&alias,1800)); CHECK(fat_check_fresh(&a,&check)==F_STALE);
    t=(FatTransfer){out,0,1,99}; OK(fat_read_checked(&a,&t,&policy)); CHECK(check.count==4);
    CHECK(fat_check_fresh(&b,&check)==F_STALE);
    CHECK(ABI(fat_check_file,&root,8,&check,0)==F_ARGUMENT);
    CHECK(fat_check_fresh(&a,&check)==F_STALE);
    CHECK(fat_check_file(&a,1,&check)==F_LIMIT); reads=f->provider_reads;
    CHECK(fat_read_checked(&a,&t,&policy)==F_LIMIT && t.done==0 && f->provider_reads==reads);
    OK(fat_check_file(&a,8,&check));
    policy.reserved=1; CHECK(fat_read_checked(&a,&t,&policy)==F_ARGUMENT); policy.reserved=0;
    OK(fat_close(&a)); CHECK(fat_check_fresh(&a,&check)==F_STALE);
    OK(fat_check_fresh(&alias,&check)); OK(fat_close(&alias));
    OK(fat_open(&root,U("renamed"),3,&a)); CHECK(fat_check_fresh(&a,&check)==F_STALE);
    OK(fat_close(&a)); OK(fat_close(&b)); OK(fat_close(&root)); destroy(f);

    /* A short chain permits a prefix through basic API; selected checking
       refuses the operation before data access and keeps that scoped result. */
    f=fixture(512,1); memset(&v,0,sizeof(v)); memset(&root,0,sizeof(root)); memset(&a,0,sizeof(a));
    raw_entry(page(&f->disk,f->disk.data,1)->data,"SHORT   BIN",5,1024);
    fat_value(&f->disk,0,5,0xFFFFFFF); memset(page(&f->disk,f->disk.data+3,1)->data,0x37,512);
    OK(fat_volume_init(&v,&f->id,pool,5)); OK(fat_root(&v,1,&root)); OK(fat_open(&root,U("SHORT.BIN"),1,&a));
    t=(FatTransfer){out,0,1,0}; OK(fat_read_at(&a,&t)); CHECK(out[0]==0x37);
    CHECK(ABI(fat_check_file,&a,8,&check,0)==F_CORRUPT);
    CHECK(check.issue==FC_SHORT && check.count==1 && check.expected==2);
    reads=f->provider_reads; t.done=99;
    CHECK(fat_read_checked(&a,&t,&policy)==F_CORRUPT && t.done==0 && f->provider_reads==reads);
    OK(fat_mount(&f->id,&f->fault,NULL,&f->workspace)); CHECK(fat_check_fresh(&a,&check)==F_STALE);
    CHECK(fat_close(&a)==F_STALE); CHECK(fat_close(&root)==F_STALE); destroy(f);
}

static void test_check_metadata(void) {
    Fixture *f=fixture(512,1); FatCheck check; uint64_t reads;
    unsigned char *boot=page(&f->disk,0,0)->data,*backup=page(&f->disk,6,0)->data;
    report("optional reserved/status and backup checks / absent versus contradictory / cache integrity");
    OK(ABI(fat_check_reserved,&f->id,&check,0,0)); CHECK(check.examined==2 && check.flags==0);
    fat_value(&f->disk,0,1,0x03FFFFFF);
    CHECK(fat_check_reserved(&f->id,&check)==F_ATTENTION);
    CHECK(check.issue==FC_STATUS && check.flags==(FC_DIRTY|FC_HARD_ERROR));
    fat_value(&f->disk,0,1,0x03FFFFFE);
    CHECK(fat_check_reserved(&f->id,&check)==F_CORRUPT && check.issue==FC_RESERVED_VALUE && check.cluster==1);
    fat_value(&f->disk,0,1,0xFFFFFFF); fat_value(&f->disk,0,0,0xFFFFFF9);
    CHECK(fat_check_reserved(&f->id,&check)==F_CORRUPT && check.cluster==0 && check.expected==0xFFFFFF8);
    fat_value(&f->disk,0,0,0xFFFFFF8);
    OK(ABI(fat_check_backup,&f->id,&check,0,0)); CHECK(f->id.fat_lba==UINT64_MAX);
    backup[3]^=1; backup[71]^=1; OK(fat_check_backup(&f->id,&check)); /* boot code/label */
    backup[13]=2; CHECK(fat_check_backup(&f->id,&check)==F_CORRUPT);
    CHECK(check.issue==FC_BACKUP_MISMATCH && check.sector==6 && check.expected==1 && check.observed==2);
    backup[13]=1; backup[510]=0;
    CHECK(fat_check_backup(&f->id,&check)==F_CORRUPT && check.issue==FC_BACKUP_SIGNATURE); backup[510]=0x55;
    wr16(boot+50,0); reads=f->provider_reads;
    OK(fat_check_backup(&f->id,&check)); CHECK(check.flags==FC_ABSENT && f->provider_reads==reads+1);
    wr16(boot+50,32); reads=f->provider_reads;
    CHECK(fat_check_backup(&f->id,&check)==F_CORRUPT && check.issue==FC_BACKUP_LOCATION);
    CHECK(f->provider_reads==reads+1); wr16(boot+50,6);
    f->disk.fail_read=1; CHECK(fat_check_backup(&f->id,&check)==F_IO && check.sector==6 && check.issue==FC_IO);
    f->disk.fail_read=-1; OK(fat_check_reserved(&f->id,&check)); destroy(f);
}

static void test_check_mirrors(void) {
    unsigned bytes;
    report("bounded FAT comparison / sector boundary / high nibble / inactive copy / I/O evidence");
    for(bytes=512;bytes<=4096;bytes*=2) {
        Fixture *f=fixture(bytes,1); FatCheck check; uint32_t boundary=bytes/4;
        FatFatRange range={boundary-1,3}; uint64_t reads=f->provider_reads;
        fat_value(&f->disk,0,boundary-1,0xA0000000); fat_value(&f->disk,1,boundary-1,0xB0000000);
        OK(ABI(fat_check_mirrors,&f->id,&range,&check,0));
        CHECK(check.examined==3 && f->provider_reads==reads+4);
        fat_value(&f->disk,1,boundary,0xFFFFFFF);
        CHECK(fat_check_mirrors(&f->id,&range,&check)==F_CORRUPT);
        CHECK(check.issue==FC_MIRROR_MISMATCH && check.cluster==boundary && check.examined==2);
        CHECK(check.expected==0 && check.observed==0xFFFFFFF && check.sector==32+f->disk.fat_sectors+1);
        range=(FatFatRange){0,1}; OK(fat_check_mirrors(&f->id,&range,&check));
        range=(FatFatRange){f->id.cluster_count+2,0}; reads=f->provider_reads;
        OK(fat_check_mirrors(&f->id,&range,&check)); CHECK(check.examined==0 && f->provider_reads==reads);
        range.count=1; CHECK(fat_check_mirrors(&f->id,&range,&check)==F_RANGE);
        range=(FatFatRange){UINT32_MAX,2}; CHECK(fat_check_mirrors(&f->id,&range,&check)==F_RANGE);
        range=(FatFatRange){0,2}; f->disk.fail_read=1;
        CHECK(fat_check_mirrors(&f->id,&range,&check)==F_IO && check.issue==FC_IO && check.sector==32+f->disk.fat_sectors);
        f->disk.fail_read=-1; wr16(page(&f->disk,0,0)->data+40,0x81);
        OK(fat_mount(&f->id,&f->fault,NULL,&f->workspace)); reads=f->provider_reads;
        OK(fat_check_mirrors(&f->id,&range,&check)); CHECK(check.flags==FC_INACTIVE && f->provider_reads==reads);
        destroy(f);
    }
}

static void test_read_adaptive(void) {
    const unsigned geometries[][2]={{512,1},{512,128},{4096,16}};
    unsigned g,j;
    report("adaptive read / no healthy preflight / bounded diagnosis / retained prefix and primary error");
    for(g=0;g<sizeof(geometries)/sizeof(geometries[0]);g++) {
        Fixture *f=fixture(geometries[g][0],geometries[g][1]);
        FatVolume v={0}; FatObject pool[5]; FatHandle root={0},a={0},alias={0},b={0},denied={0};
        FatCheck check={0},metadata,before; FatCheckedRead policy={&check,8,0};
        FatTransfer t; uint64_t reads; unsigned cluster=f->id.cluster_bytes;
        unsigned char *out=malloc(2*(size_t)cluster); CHECK(out);
        unsigned char *dir=page(&f->disk,f->disk.data,1)->data;
        raw_entry(dir,"SHORT   BIN",5,2*cluster);
        raw_entry(dir+32,"GOOD    BIN",6,16);
        fat_value(&f->disk,0,5,0xFFFFFFF); fat_value(&f->disk,0,6,0xFFFFFFF);
        for(j=0;j<f->disk.spc;j++)
            memset(page(&f->disk,f->disk.data+3*f->disk.spc+j,1)->data,0x37,f->disk.bytes);
        memset(page(&f->disk,f->disk.data+4*f->disk.spc,1)->data,0x62,f->disk.bytes);
        OK(fat_volume_init(&v,&f->id,pool,5)); OK(fat_root(&v,1,&root));
        OK(fat_open(&root,U("SHORT.BIN"),1,&a)); OK(fat_open(&root,U("SHORT.BIN"),1,&alias));
        OK(fat_open(&root,U("GOOD.BIN"),1,&b)); OK(fat_seek(&a,11));
        reads=f->provider_reads; t=(FatTransfer){out,0,17,99};
        OK(ABI(fat_read_adaptive,&a,&t,&policy,0));
        CHECK(t.done==17 && out[0]==0x37 && check.scope==0 && f->provider_reads==reads+1);
        CHECK(a.position==11); /* An explicit-offset read never moves the handle. */
        memset(out,0xA5,2*(size_t)cluster); t.length=2*cluster;
        CHECK(ABI(fat_read_adaptive,&a,&t,&policy,0)==F_CORRUPT);
        CHECK(t.done==cluster && check.scope==FC_FILE && check.status==F_CORRUPT);
        CHECK(check.issue==FC_SHORT && check.count==1 && check.expected==2 && check.examined==1);
        for(j=0;j<cluster;j++) CHECK(out[j]==0x37 && out[cluster+j]==0xA5);
        reads=f->provider_reads; t.done=99;
        CHECK(fat_read_adaptive(&alias,&t,&policy)==F_CORRUPT && t.done==0 && f->provider_reads==reads);
        /* Evidence for A does not restrict B or turn success into checked-good. */
        t=(FatTransfer){out,0,16,99}; OK(fat_read_adaptive(&b,&t,&policy));
        CHECK(t.done==16 && out[0]==0x62 && check.scope==0);
        policy.budget=0; t=(FatTransfer){out,0,2*cluster,99};
        CHECK(fat_read_adaptive(&a,&t,&policy)==F_CORRUPT && t.done==cluster);
        CHECK(check.status==F_LIMIT && check.issue==FC_BUDGET && !check.examined);
        policy.budget=8;
        /* Data I/O fails but the coherent cached FAT still proves this chain.
           Returning F_OK here would conceal the failed data operation. */
        f->disk.fail_read=0; reads=f->provider_reads;
        memset(out,0xA5,16); t=(FatTransfer){out,0,16,99};
        CHECK(ABI(fat_read_adaptive,&b,&t,&policy,0)==F_IO && t.done==0);
        CHECK(check.scope==FC_FILE && check.status==F_OK && check.count==1);
        CHECK(f->provider_reads==reads+1 && out[0]==0xA5);
        f->disk.fail_read=-1; OK(fat_check_backup(&f->id,&metadata));
        /* With no cached FAT, one failed data read plus one diagnostic read;
           no data retry, no repeated diagnostic and no writes. */
        f->disk.fail_read=0; reads=f->provider_reads;
        CHECK(fat_read_adaptive(&b,&t,&policy)==F_IO && t.done==0);
        CHECK(check.status==F_IO && check.issue==FC_IO && check.examined==0);
        CHECK(f->provider_reads==reads+2);
        f->disk.fail_read=-1; policy.reserved=1; before=check; reads=f->provider_reads;
        CHECK(ABI(fat_read_adaptive,&b,&t,&policy,0)==F_ARGUMENT && t.done==0);
        CHECK(!memcmp(&before,&check,sizeof(check)) && f->provider_reads==reads);
        policy.reserved=0; t.offset=16;
        OK(fat_read_adaptive(&b,&t,&policy)); CHECK(t.done==0 && !check.scope && f->provider_reads==reads);
        OK(fat_open(&root,U("GOOD.BIN"),2,&denied)); reads=f->provider_reads;
        CHECK(fat_read_adaptive(&denied,&t,&policy)==F_ARGUMENT && t.done==0 && !check.scope && f->provider_reads==reads);
        OK(fat_close(&denied));
        /* The base file reader reports F_CORRUPT for a directory. Keep that
           primary result even though file diagnosis reports F_ARGUMENT. */
        CHECK(fat_read_at(&root,&t)==F_CORRUPT);
        CHECK(fat_read_adaptive(&root,&t,&policy)==F_CORRUPT && t.done==0);
        CHECK(check.scope==FC_FILE && check.status==F_ARGUMENT && f->provider_reads==reads);
        OK(fat_close(&b)); reads=f->provider_reads;
        CHECK(fat_read_adaptive(&b,&t,&policy)==F_STALE && t.done==0 && !check.scope && f->provider_reads==reads);
        CHECK(!f->disk.writes); OK(fat_close(&alias)); OK(fat_close(&a)); OK(fat_close(&root));
        free(out); destroy(f);
    }
}
