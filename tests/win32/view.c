/* One quiescent provider, independent interpretations of its FAT copies.
   Raw media deliberately gives the two copies contradictory directory/file
   chains; neither mounting nor diagnosing a view may modify the source. */
static void test_view(void) {
    const unsigned geometry[][2]={{512,1},{512,128},{1024,128},{2048,128},{4096,64}};
    report("selected FAT views / independent caches / directory, name, file and ownership checks / read-only / lifetime");
    for(unsigned g=0;g<sizeof(geometry)/sizeof(*geometry);++g) {
        Fixture *f=fixture(geometry[g][0],geometry[g][1]);
        unsigned cb=f->id.cluster_bytes,bps=f->id.sector_bytes,slots=cb/32;
        for(unsigned sector=0;sector<f->disk.spc;++sector)
            memset(page(&f->disk,f->disk.data+sector,1)->data,0xe5,bps);
        raw_entry(owner_data(f,3),"CHOICE  BIN",5,cb+7);
        memset(owner_data(f,5),0x35,bps); memset(owner_data(f,6),0x36,bps);
        fat_value(&f->disk,0,2,2); fat_value(&f->disk,0,5,5);
        fat_value(&f->disk,1,2,3); fat_value(&f->disk,1,3,0x0fffffff);
        fat_value(&f->disk,1,5,6); fat_value(&f->disk,1,6,0x0fffffff);
        /* Exercise a source whose BPB also selects FAT 1, without changing the
           explicit FAT 0 view's interpretation. Other geometries are mirrored. */
        if(g==1) {
            wr16(page(&f->disk,0,0)->data+40,0x81);
            wr16(page(&f->disk,6,0)->data+40,0x81);
            OK(fat_mount(&f->id,&f->fault,0,&f->workspace));
        }
        uint32_t value; OK(fat_get(&f->id,2,&value));
        FatIdentity source=f->id; unsigned char source_cache[3*4096];
        memcpy(source_cache,f->workspace_data,sizeof(source_cache));
        FatView view[2]={0}; unsigned char scratch[2][3*4096+32];
        FatVolume volumes[2]={0}; FatObject pools[2][3]; FatHandle roots[2]={0},file={0};
        FatCheck check[2]={0}; FatWorkspace work[2];
        uint64_t reads=f->provider_reads,writes=f->disk.writes,pages=f->buffer.pages;
        for(unsigned i=0;i<2;++i) {
            memset(scratch[i],0xa5,sizeof(scratch[i]));
            work[i]=(FatWorkspace){scratch[i]+16,3*bps,0};
            /* Copy selection consumes R8D; preserve no assumption about R8[63:32]. */
            OK(ABI(fat_view_open,&view[i],&f->id,0xFFFFFFFF00000000ull|i,&work[i]));
            CHECK(view[i].identity.active_fat==i && !view[i].identity.mirrored);
            CHECK(view[i].identity.ops==&view[i].provider && !view[i].identity.shared);
            CHECK(!view[i].provider.write && !view[i].provider.begin && !view[i].provider.end && !view[i].provider.flush);
            CHECK(view[i].identity.fat_lba==UINT64_MAX && view[i].identity.dir_lba==UINT64_MAX);
            OK(fat_volume_init(&volumes[i],&view[i].identity,pools[i],3)); OK(fat_root(&volumes[i],FH_READ,&roots[i]));
        }
        CHECK(f->provider_reads==reads); /* clone/cache setup does no I/O */
        CHECK(fat_check_chain(&view[0].identity,5,8,&check[0])==F_CORRUPT && check[0].issue==FC_CYCLE);
        OK(fat_check_chain(&view[1].identity,5,8,&check[1])); CHECK(check[1].count==2);
        CHECK(fat_check_directory(&roots[0],slots*2+2,&check[0])==F_CORRUPT);
        OK(fat_check_directory(&roots[1],slots+2,&check[1]));
        FatEntry entries[2]; FatNameCheck names={entries,2,2*(slots+2),2,0};
        OK(fat_check_names(&roots[1],&names,&check[1]));
        OK(fat_open(&roots[1],U("CHOICE.BIN"),FH_READ,&file));
        OK(fat_check_file(&file,8,&check[1])); CHECK(check[1].count==2);
        CHECK(fat_check_fresh(&file,&check[0])==F_STALE);
        unsigned char bytes[7]; FatTransfer transfer={bytes,cb,sizeof(bytes),0};
        OK(fat_read_at(&file,&transfer)); CHECK(transfer.done==7);
        for(unsigned i=0;i<sizeof(bytes);++i) CHECK(bytes[i]==0x36);
        FatCheck metadata; OK(fat_check_reserved(&view[1].identity,&metadata));
        OK(fat_check_backup(&view[1].identity,&metadata));
        FatFatRange range={0,8}; OK(fat_check_mirrors(&view[1].identity,&range,&metadata)); CHECK(metadata.flags&FC_INACTIVE);
        FatOwner *owners=malloc(f->id.cluster_count*sizeof(*owners)); CHECK(owners);
        FatDirectoryTask directories[1]; FatOwnershipReport ownership;
        FatOwnershipCheck request={owners,directories,f->id.cluster_count,1,f->id.cluster_count+16,slots+2,0};
        OK(fat_check_ownership(&view[1].identity,&request,&ownership));
        CHECK(ownership.files==1 && ownership.directories==1 && ownership.check.count==4);
        CHECK(ownership.free_clusters==f->id.cluster_count-4 && !ownership.orphan_clusters); free(owners);
        FatEntry entry; OK(fat_lookup(&view[1].identity,2,U("CHOICE.BIN"),&entry));
        CHECK(fat_resize(&view[1].identity,&entry,cb+8)==F_READONLY);
        FatFormatOptions options={0}; options.cluster_bytes=cb; FatFormatReport formatted;
        SectorOps readonly=view[1].provider; readonly.sectors+=32*f->disk.spc;
        CHECK(fat_format(&readonly,&options,&work[1],&formatted)==F_READONLY);
        CHECK(formatted.effect==FE_NONE && !formatted.writes);
        CHECK(!memcmp(&source,&f->id,sizeof(source)) && !memcmp(source_cache,f->workspace_data,sizeof(source_cache)));
        CHECK(f->disk.writes==writes && f->buffer.pages==pages);
        for(unsigned i=0;i<2;++i) {
            for(unsigned j=0;j<16;++j) CHECK(scratch[i][j]==0xa5 && scratch[i][16+3*bps+j]==0xa5);
        }
        /* Close retires this view's handles and reports even if their storage
           remains; reopening the same address cannot revive old evidence. */
        reads=f->provider_reads; uint64_t generation=view[1].identity.generation;
        OK(ABI(fat_view_close,&view[1],0,0,0));
        CHECK(fat_check_fresh(&file,&check[1])==F_STALE && fat_close(&file)==F_STALE);
        OK(fat_view_open(&view[1],&f->id,1,&work[1]));
        CHECK(view[1].identity.generation>generation && fat_check_fresh(&file,&check[1])==F_STALE);
        OK(fat_view_close(&view[1])); OK(fat_view_close(&view[0])); CHECK(f->provider_reads==reads);
        CHECK(!memcmp(&source,&f->id,sizeof(source))); destroy(f);
    }
}

static void test_view_admission(void) {
    report("FAT view admission / untouched output / generation exhaustion / selected-copy read failure");
    Fixture *f=fixture(512,1); FatView view={0},before; unsigned char scratch[3*512];
    FatWorkspace work={scratch,sizeof(scratch),0}; uint64_t reads=f->provider_reads;
    uint32_t magic=f->id.magic;
#define VIEW_FAIL(status,index) do { before=view; CHECK(ABI(fat_view_open,&view,&f->id,index,&work)==status); CHECK(!memcmp(&view,&before,sizeof(view))); } while(0)
    VIEW_FAIL(F_RANGE,2); VIEW_FAIL(F_RANGE,UINT32_MAX);
    f->id.transaction=1; VIEW_FAIL(F_BUSY,1); f->id.transaction=0;
    f->id.magic=0; VIEW_FAIL(F_STALE,1); f->id.magic=magic;
    work.bytes=sizeof(scratch)-1; VIEW_FAIL(F_MEMORY,1); work.bytes=sizeof(scratch);
    work.data=0; VIEW_FAIL(F_ARGUMENT,1);
    work.data=(void *)(uintptr_t)(UINT64_MAX-100); VIEW_FAIL(F_ARGUMENT,1); work.data=scratch;
    work.reserved=1; VIEW_FAIL(F_ARGUMENT,1); work.reserved=0;
    view.identity.generation=UINT64_MAX-1; VIEW_FAIL(F_LIMIT,1);
    view.identity.generation=UINT64_MAX; VIEW_FAIL(F_LIMIT,1);
    view.identity.generation=0;
    OK(fat_view_open(&view,&f->id,1,&work)); VIEW_FAIL(F_BUSY,0);
    CHECK(f->provider_reads==reads); FatCheck check;
    f->disk.fail_read=0;
    CHECK(fat_check_chain(&view.identity,2,1,&check)==F_IO && check.issue==FC_IO);
    CHECK(check.sector==f->id.fat_start+f->id.fat_sectors);
    f->disk.fail_read=-1; OK(fat_check_chain(&view.identity,2,1,&check));
    OK(fat_view_close(&view)); CHECK(fat_view_close(&view)==F_STALE);
    view.identity.generation=UINT64_MAX-2;
    OK(fat_view_open(&view,&f->id,1,&work)); CHECK(view.identity.generation==UINT64_MAX-1);
    OK(fat_view_close(&view)); CHECK(view.identity.generation==UINT64_MAX); VIEW_FAIL(F_LIMIT,0);
    view.identity.generation=UINT64_MAX-2;
    OK(fat_view_open(&view,&f->id,1,&work));
    OK(fat_invalidate(&view.identity)); CHECK(view.identity.generation==UINT64_MAX);
    OK(fat_view_close(&view)); CHECK(view.identity.generation==UINT64_MAX); VIEW_FAIL(F_LIMIT,0);
#undef VIEW_FAIL
    destroy(f);
}

static void test_view_staging(void) {
    report("FAT view includes accepted staging without commit / live source handles survive");
    Fixture *f=fixture(512,1); FatVolume v={0}; FatObject pool[3]; FatHandle root={0},file={0};
    OK(fat_volume_init(&v,&f->id,pool,3)); OK(fat_root(&v,3,&root)); shared_new(&root,U("pending"),0,&file);
    unsigned char data[513]; memset(data,0x75,sizeof(data)); FatTransfer t={data,0,sizeof(data),0}; OK(fat_write_at(&file,&t));
    FatCheck original; OK(fat_check_file(&file,4,&original)); CHECK(f->buffer.pages && !f->disk.writes);
    uint64_t pages=f->buffer.pages; FatIdentity source=f->id;
    FatView view={0}; unsigned char scratch[1536]; FatWorkspace work={scratch,sizeof(scratch),0};
    OK(fat_view_open(&view,&f->id,1,&work));
    FatEntry entry; OK(fat_lookup(&view.identity,2,U("pending"),&entry)); CHECK(entry.size==513);
    FatCheck check; OK(fat_check_chain(&view.identity,entry.cluster,4,&check)); CHECK(check.count==2);
    unsigned char out[513]; t=(FatTransfer){out,0,sizeof(out),0}; OK(fat_read(&view.identity,&entry,&t)); CHECK(!memcmp(data,out,sizeof(data)));
    CHECK(fat_resize(&view.identity,&entry,600)==F_READONLY);
    OK(fat_view_close(&view)); CHECK(!memcmp(&source,&f->id,sizeof(source)));
    OK(fat_check_fresh(&file,&original)); CHECK(f->buffer.pages==pages && !f->disk.writes);
    OK(fat_handle_resize(&file,600)); OK(fat_close(&file)); OK(fat_close(&root)); destroy(f);
}
