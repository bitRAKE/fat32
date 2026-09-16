static void test_boot_view(void) {
    report("backup BPB view / 31 geometries / one explicit read / authentic primary evidence / read-only shared handles");
    unsigned geometries=0;
    for(unsigned bps=512;bps<=4096;bps*=2) for(unsigned spc=1;spc<=128 && bps*spc<=262144;spc*=2) {
        ++geometries; Fixture *f=fixture(bps,spc); unsigned char original[4096],backup[4096];
        raw_entry(owner_data(f,2),"CANDIDATBIN",5,7); fat_value(&f->disk,0,5,0x0fffffff); salvage_data(f,5);
        unsigned char *primary=page(&f->disk,0,0)->data; primary[510]=0;
        memcpy(original,primary,bps); memcpy(backup,page(&f->disk,6,0)->data,bps);
        CHECK(fat_mount(&f->id,&f->fault,NULL,&f->workspace)==F_FORMAT);
        FatView view={0}; unsigned char scratch[3*4096+32]; memset(scratch,0xa5,sizeof(scratch));
        FatWorkspace workspace={scratch+16,3*bps,0}; uint16_t oem[256];
        for(unsigned i=0;i<256;++i) oem[i]=(uint16_t)i;
        FatBootSource boot={6,0,oem}; SectorOps provider=f->fault; uint64_t reads=f->provider_reads;
        OK(ABI(fat_view_boot,&view,&f->fault,&boot,&workspace)); CHECK(f->provider_reads==reads+1);
        CHECK(view.identity.cluster_bytes==bps*spc && view.identity.ops==&view.provider && view.identity.oem==oem);
        CHECK(view.provider.context==provider.context && view.provider.read==provider.read && !view.provider.reserved);
        CHECK(!view.provider.write && !view.provider.begin && !view.provider.end && !view.provider.flush);
        FatCheck checked; CHECK(fat_check_backup(&view.identity,&checked)==F_CORRUPT);
        CHECK(checked.issue==FC_BACKUP_SIGNATURE && checked.sector==0); /* not a hidden substitute BPB */
        FatVolume volume={0}; FatObject objects[2]; FatHandle root={0},file={0};
        OK(fat_volume_init(&volume,&view.identity,objects,2)); OK(fat_root(&volume,FH_READ,&root));
        OK(fat_open(&root,U("CANDIDAT.BIN"),FH_READ,&file)); unsigned char bytes[7]; FatTransfer transfer={bytes,0,7,0};
        OK(fat_read_at(&file,&transfer)); CHECK(transfer.done==7);
        for(unsigned i=0;i<7;++i) CHECK(bytes[i]==(unsigned char)(5*13+i));
        FatEntry entry; OK(fat_lookup(&view.identity,2,U("CANDIDAT.BIN"),&entry));
        CHECK(fat_resize(&view.identity,&entry,8)==F_READONLY);
        uint64_t generation=view.identity.generation;
        OK(fat_view_close(&view)); CHECK(fat_read_at(&file,&transfer)==F_STALE && !transfer.done);
        OK(fat_view_boot(&view,&f->fault,&boot,&workspace)); CHECK(view.identity.generation>generation);
        CHECK(fat_read_at(&file,&transfer)==F_STALE && !transfer.done); OK(fat_view_close(&view));
        CHECK(!memcmp(&f->fault,&provider,sizeof(provider)) && !f->disk.writes && !f->buffer.pages);
        CHECK(!memcmp(primary,original,bps) && !memcmp(page(&f->disk,6,0)->data,backup,bps));
        for(unsigned i=0;i<16;++i) CHECK(scratch[i]==0xa5 && scratch[16+3*bps+i]==0xa5);
        destroy(f);
    }
    CHECK(geometries==31);
}
static void test_boot_view_admission(void) {
    report("backup BPB failure leaves view unchanged / candidate bounds and location / malformed geometry / IO and lifecycle limits");
    Fixture *f=fixture(512,1); FatView view={0},saved; view.identity.generation=17;
    unsigned char scratch[1536]; FatWorkspace workspace={scratch,sizeof(scratch),0}; FatBootSource boot={6,0,NULL};
    uint64_t reads=f->provider_reads; saved=view;
    boot.reserved=1; CHECK(fat_view_boot(&view,&f->fault,&boot,&workspace)==F_ARGUMENT); boot.reserved=0;
    workspace.reserved=1; CHECK(fat_view_boot(&view,&f->fault,&boot,&workspace)==F_ARGUMENT); workspace.reserved=0;
    workspace.data=NULL; CHECK(fat_view_boot(&view,&f->fault,&boot,&workspace)==F_ARGUMENT);
    workspace.data=(void *)(uintptr_t)(UINT64_MAX-511); CHECK(fat_view_boot(&view,&f->fault,&boot,&workspace)==F_ARGUMENT);
    workspace.data=scratch; workspace.bytes=1535; CHECK(fat_view_boot(&view,&f->fault,&boot,&workspace)==F_MEMORY); workspace.bytes=1536;
    boot.sector=0; CHECK(fat_view_boot(&view,&f->fault,&boot,&workspace)==F_RANGE);
    boot.sector=65536; CHECK(fat_view_boot(&view,&f->fault,&boot,&workspace)==F_RANGE); boot.sector=6;
    SectorOps short_provider=f->fault; short_provider.sectors=6;
    CHECK(fat_view_boot(&view,&short_provider,&boot,&workspace)==F_RANGE);
    short_provider=f->fault; short_provider.read=NULL; CHECK(fat_view_boot(&view,&short_provider,&boot,&workspace)==F_FORMAT);
    CHECK(f->provider_reads==reads && !memcmp(&view,&saved,sizeof(view)));
    f->disk.fail_read=0; CHECK(ABI(fat_view_boot,&view,&f->fault,&boot,&workspace)==F_IO);
    CHECK(f->provider_reads==reads+1 && !memcmp(&view,&saved,sizeof(view))); f->disk.fail_read=-1;
    unsigned char *backup=page(&f->disk,6,0)->data;
    backup[510]=0; CHECK(fat_view_boot(&view,&f->fault,&boot,&workspace)==F_FORMAT); backup[510]=0x55;
    backup[13]=3; CHECK(fat_view_boot(&view,&f->fault,&boot,&workspace)==F_FORMAT); backup[13]=1;
    CHECK(!memcmp(&view,&saved,sizeof(view)));
    memcpy(page(&f->disk,32,1)->data,backup,512); boot.sector=32;
    CHECK(fat_view_boot(&view,&f->fault,&boot,&workspace)==F_FORMAT && !memcmp(&view,&saved,sizeof(view)));
    /* Caller can explicitly inspect another reserved-sector candidate even
       when its optional backup pointer names 6. No authority is inferred. */
    memcpy(page(&f->disk,31,1)->data,backup,512); boot.sector=31;
    OK(fat_view_boot(&view,&f->fault,&boot,&workspace)); CHECK(view.identity.backup==6);
    saved=view; reads=f->provider_reads; CHECK(fat_view_boot(&view,&f->fault,&boot,&workspace)==F_BUSY);
    CHECK(f->provider_reads==reads && !memcmp(&view,&saved,sizeof(view))); OK(fat_view_close(&view));
    view.identity.generation=UINT64_MAX-1; saved=view;
    CHECK(fat_view_boot(&view,&f->fault,&boot,&workspace)==F_LIMIT && !memcmp(&view,&saved,sizeof(view)));
    CHECK(f->provider_reads==reads && !f->disk.writes && !f->buffer.pages); destroy(f);
}
