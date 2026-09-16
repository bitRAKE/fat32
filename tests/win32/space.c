static void test_space(void) {
    unsigned geometry;
    report("optional exact free scan / current selected FAT / ignored hints / partial I/O keeps output / staged allocation");
    for(geometry=0;geometry<4;geometry++) {
        unsigned bps=512u<<geometry; Fixture *f=fixture(bps,1); uint32_t count=0xAABBCCDD;
        uint64_t reads; unsigned expected=(f->disk.clusters+2)*4;
        f->id.free_hint=0; f->id.fat_lba=UINT64_MAX; reads=f->provider_reads;
        OK(ABI(fat_count_free,&f->id,&count,0,0)); CHECK(count==f->disk.clusters-1);
        CHECK(f->provider_reads-reads==(expected+bps-1)/bps && !f->disk.writes);
        fat_value(&f->disk,0,3,0xA0000000); fat_value(&f->disk,0,4,0xFFFFFF7);
        f->id.fat_lba=UINT64_MAX; OK(fat_count_free(&f->id,&count)); CHECK(count==f->disk.clusters-2);
        f->id.fat_lba=UINT64_MAX; f->disk.fail_read=1; count=0xAABBCCDD;
        CHECK(fat_count_free(&f->id,&count)==F_IO && count==0xAABBCCDD); f->disk.fail_read=-1;
        wr16(page(&f->disk,0,0)->data+40,0x81); OK(fat_mount(&f->id,&f->fault,NULL,&f->workspace));
        OK(fat_count_free(&f->id,&count)); CHECK(count==f->disk.clusters-1); /* Active copy 1. */
        f->id.magic=0; count=0xAABBCCDD; CHECK(fat_count_free(&f->id,&count)==F_FORMAT && count==0xAABBCCDD);
        destroy(f);
    }
    {
        CommitFixture *c=commit_fixture(512,1,64,64); uint32_t before,after;
        OK(fat_count_free(&c->base->id,&before)); OK(fat_handle_resize(&c->opened,1400));
        OK(fat_count_free(&c->base->id,&after)); CHECK(after==before-2 && !c->writes);
        OK(fat_order_discard(&c->order,&c->base->id)); OK(fat_count_free(&c->base->id,&after)); CHECK(after==before);
        commit_destroy(c);
    }
}
