/* Independently encoded directory slots; only the diagnostic is under test. */
static unsigned char *check_dir_slot(Fixture *f,unsigned slot) {
    unsigned per=f->disk.bytes*f->disk.spc/32,cluster=slot<per?2:7;
    uint64_t offset=(uint64_t)(slot%per)*32;
    CHECK(slot<2*per);
    return page(&f->disk,f->disk.data+(uint64_t)(cluster-2)*f->disk.spc+offset/f->disk.bytes,1)->data+offset%f->disk.bytes;
}
static void check_dir_clear(Fixture *f) {
    unsigned i,per=f->disk.bytes*f->disk.spc/32;
    for(i=0;i<2*per;i++) memset(check_dir_slot(f,i),0,32);
    for(i=0;i<2;i++) { fat_value(&f->disk,i,2,0x0fffffff); fat_value(&f->disk,i,7,0x0fffffff); }
    f->id.dir_lba=f->id.fat_lba=UINT64_MAX;
}
static void check_dir_extend(Fixture *f) {
    for(unsigned i=0;i<2;i++) fat_value(&f->disk,i,2,7);
}
static void check_dir_lfn(Fixture *f,unsigned start,unsigned length) {
    static const unsigned offsets[]={1,3,5,7,9,14,16,18,20,22,24,28,30};
    unsigned count=(length+12)/13,checksum=0,i,j; unsigned char *p;
    const unsigned char alias[]="FILE    BIN";
    if(!count) count=1;
    for(i=0;i<11;i++) checksum=((checksum&1)?128:0)+(checksum>>1)+alias[i],checksum&=255;
    if(start+count+1>=f->disk.bytes*f->disk.spc/32) check_dir_extend(f);
    for(i=0;i<count;i++) {
        unsigned ordinal=count-i;
        p=check_dir_slot(f,start+i); memset(p,0,32);
        p[0]=(unsigned char)(ordinal|(i?0:0x40)); p[11]=0x0f; p[13]=(unsigned char)checksum;
        for(j=0;j<13;j++) {
            unsigned at=(ordinal-1)*13+j;
            wr16(p+offsets[j],at<length?'a'+at%26:at==length?0:0xffff);
        }
    }
    raw_entry(check_dir_slot(f,start+count),"FILE    BIN",5,1);
}
static int check_directory_call(Fixture *f,FatHandle *dir,unsigned budget,FatCheck *out) {
    struct { uint64_t before; FatCheck value; uint64_t after; } guarded;
    int status;
    memset(&guarded,0xa5,sizeof(guarded));
    f->id.dir_lba=f->id.fat_lba=UINT64_MAX;
    status=ABI(fat_check_directory,dir,budget,&guarded.value,0);
    CHECK(guarded.before==UINT64_C(0xa5a5a5a5a5a5a5a5) && guarded.after==guarded.before);
    CHECK(guarded.value.status==(unsigned)status && guarded.value.scope==FC_DIRECTORY);
    *out=guarded.value;
    return status;
}
static void test_check_directory(void) {
    const unsigned sizes[][2]={{512,1},{512,128},{4096,16}};
    report("bounded directory diagnostics / independent LFN slots / boundaries / cycles / no mutations");
    for(unsigned geometry=0;geometry<3;geometry++) {
        Fixture *f=fixture(sizes[geometry][0],sizes[geometry][1]);
        FatVolume v={0}; FatObject pool[4],before[4]; FatHandle root={0},writeonly={0};
        FatCheck c; FatCursor cursor; FatEntry entry;
        unsigned per=f->disk.bytes*f->disk.spc/32,i; uint64_t reads;
        OK(fat_volume_init(&v,&f->id,pool,4)); OK(fat_root(&v,3,&root));
        memcpy(before,pool,sizeof(pool));
        reads=f->provider_reads;
        CHECK(check_directory_call(f,&root,0,&c)==F_LIMIT && c.issue==FC_BUDGET);
        CHECK(c.examined==0 && c.budget==0 && c.sector==UINT64_MAX && f->provider_reads==reads);
        OK(check_directory_call(f,&root,1,&c));
        CHECK(c.examined==1 && c.count==0 && c.flags==FC_ENDMARKER && c.first==2 && c.last==2);
        CHECK(c.sector==f->disk.data && !memcmp(before,pool,sizeof(pool)));
        CHECK(fat_check_fresh(&root,&c)==F_STALE); /* immediate directory evidence */
        for(i=0;i<5;i++) {
            static const unsigned lengths[]={1,13,26,255,0};
            check_dir_clear(f); check_dir_lfn(f,0,lengths[i]);
            if(lengths[i]) { OK(check_directory_call(f,&root,64,&c)); CHECK(c.count==1); }
            else CHECK(check_directory_call(f,&root,64,&c)==F_CORRUPT && c.issue==FC_LFN);
        }
        /* A two-slot LFN crosses the cluster boundary into noncontiguous 7. */
        check_dir_clear(f);
        for(i=0;i<per-1;i++) check_dir_slot(f,i)[0]=0xe5;
        check_dir_lfn(f,per-1,14); reads=f->provider_reads;
        OK(check_directory_call(f,&root,per+3,&c));
        CHECK(c.examined==per+3 && c.count==1 && c.last==7);
        CHECK(f->provider_reads-reads==f->disk.spc+2);
        for(unsigned fail=0;fail<f->disk.spc+2;fail++) {
            f->disk.fail_read=(int)fail; reads=f->provider_reads;
            CHECK(check_directory_call(f,&root,per+3,&c)==F_IO && c.issue==FC_IO);
            CHECK(f->provider_reads-reads==fail+1);
        }
        f->disk.fail_read=-1;
        CHECK(check_directory_call(f,&root,per,&c)==F_LIMIT && c.examined==per);
        check_dir_slot(f,per+1)[0]^=1;
        CHECK(check_directory_call(f,&root,per+3,&c)==F_CORRUPT && c.issue==FC_LFN);
        CHECK(c.examined==per+2 && c.sector==f->disk.data+5*f->disk.spc);
        f->id.dir_lba=UINT64_MAX;
        OK(fat_dir_open(&f->id,2,&cursor)); OK(fat_dir_next(&f->id,&cursor,&entry));
        CHECK(entry.lfn_count==0); /* basic reader still permits SFN fallback */
        check_dir_slot(f,per+1)[0]^=1;
        check_dir_slot(f,per)[0]=2; /* expected ordinal 1 */
        CHECK(check_directory_call(f,&root,per+3,&c)==F_CORRUPT && c.issue==FC_LFN);
        check_dir_slot(f,per)[0]=1;
        check_dir_slot(f,per-1)[12]=1;
        CHECK(check_directory_call(f,&root,per+3,&c)==F_ATTENTION && c.issue==FC_EXTENSION);
        check_dir_slot(f,per-1)[12]=0; wr16(check_dir_slot(f,per-1)+26,1);
        CHECK(check_directory_call(f,&root,per+3,&c)==F_ATTENTION && c.issue==FC_EXTENSION);
        wr16(check_dir_slot(f,per-1)+26,0); check_dir_slot(f,per-1)[11]|=0x80;
        OK(check_directory_call(f,&root,per+3,&c)); /* reserved attribute bits ignored */
        check_dir_slot(f,per+1)[0]=0; /* orphan before end marker */
        CHECK(check_directory_call(f,&root,per+3,&c)==F_CORRUPT && c.issue==FC_LFN);
        check_dir_slot(f,per+1)[0]=0xe5;
        CHECK(check_directory_call(f,&root,per+3,&c)==F_CORRUPT && c.issue==FC_LFN);
        check_dir_clear(f); check_dir_lfn(f,0,1); wr16(check_dir_slot(f,0)+1,0xd800);
        CHECK(check_directory_call(f,&root,4,&c)==F_CORRUPT && c.issue==FC_LFN);
        check_dir_clear(f); check_dir_lfn(f,0,1); wr16(check_dir_slot(f,0)+5,'x');
        CHECK(check_directory_call(f,&root,4,&c)==F_CORRUPT && c.issue==FC_LFN);
        check_dir_clear(f); check_dir_lfn(f,0,256);
        CHECK(check_directory_call(f,&root,32,&c)==F_CORRUPT && c.issue==FC_LFN);
        check_dir_clear(f); raw_entry(check_dir_slot(f,0),"FILE    BIN",0,0);
        OK(check_directory_call(f,&root,2,&c));
        wr32(check_dir_slot(f,0)+28,1);
        CHECK(check_directory_call(f,&root,2,&c)==F_CORRUPT && c.issue==FC_DIRENT);
        wr16(check_dir_slot(f,0)+26,1);
        CHECK(check_directory_call(f,&root,2,&c)==F_CORRUPT && c.issue==FC_DIRENT);
        wr16(check_dir_slot(f,0)+26,5); check_dir_slot(f,0)[11]=0x18;
        CHECK(check_directory_call(f,&root,2,&c)==F_CORRUPT && c.issue==FC_DIRENT);
        check_dir_slot(f,0)[11]=0x10;
        CHECK(check_directory_call(f,&root,2,&c)==F_CORRUPT && c.issue==FC_DIRENT);
        check_dir_slot(f,0)[11]=8; wr16(check_dir_slot(f,0)+26,0); wr32(check_dir_slot(f,0)+28,0);
        OK(check_directory_call(f,&root,2,&c));
        memcpy(check_dir_slot(f,1),check_dir_slot(f,0),32);
        CHECK(check_directory_call(f,&root,3,&c)==F_CORRUPT && c.issue==FC_DIRENT);
        check_dir_clear(f);
        for(i=0;i<per;i++) check_dir_slot(f,i)[0]=0xe5;
        CHECK(check_directory_call(f,&root,per,&c)==F_LIMIT);
        OK(check_directory_call(f,&root,per+1,&c)); CHECK(c.examined==per && !c.flags);
        fat_value(&f->disk,0,2,2);
        CHECK(check_directory_call(f,&root,per+1,&c)==F_CORRUPT && c.issue==FC_CYCLE);
        fat_value(&f->disk,0,2,0);
        CHECK(check_directory_call(f,&root,per+1,&c)==F_CORRUPT && c.issue==FC_LINK);
        for(i=0;i<2*per;i++) check_dir_slot(f,i)[0]=0xe5;
        fat_value(&f->disk,0,2,7); fat_value(&f->disk,0,7,2);
        CHECK(check_directory_call(f,&root,3*per+1,&c)==F_CORRUPT && c.issue==FC_CYCLE);
        check_dir_clear(f); check_dir_lfn(f,per-1,1);
        for(i=0;i<per-1;i++) check_dir_slot(f,i)[0]=0xe5;
        fat_value(&f->disk,0,2,0x0fffffff); /* active LFN orphan at EOC */
        CHECK(check_directory_call(f,&root,per+1,&c)==F_CORRUPT && c.issue==FC_LFN);
        check_dir_clear(f); check_dir_lfn(f,0,1);
        f->disk.fail_read=0;
        CHECK(check_directory_call(f,&root,8,&c)==F_IO && c.issue==FC_IO && c.examined==0);
        f->disk.fail_read=-1;
        f->id.transaction=1; reads=f->provider_reads;
        CHECK(check_directory_call(f,&root,8,&c)==F_BUSY && f->provider_reads==reads);
        f->id.transaction=0; OK(fat_root(&v,2,&writeonly));
        CHECK(check_directory_call(f,&writeonly,8,&c)==F_ARGUMENT && f->provider_reads==reads);
        OK(fat_close(&writeonly)); OK(fat_close(&root));
        CHECK(check_directory_call(f,&root,8,&c)==F_STALE && f->provider_reads==reads);
        CHECK(!f->disk.writes && !f->buffer.pages); destroy(f);
    }
}
static void test_check_directory_parent(void) {
    Fixture *f=fixture(512,1); FatVolume v={0}; FatObject pool[5];
    FatHandle root={0},sub={0},nested={0},file={0}; FatCheck c;
    unsigned char *p=page(&f->disk,f->disk.data,1)->data;
    report("directory dot/dotdot evidence / root normalization / nested parent / stale handles");
    raw_entry(p,"SUB        ",5,0); p[11]=0x10;
    fat_value(&f->disk,0,5,0x0fffffff); fat_value(&f->disk,0,8,0x0fffffff);
    p=page(&f->disk,f->disk.data+3,1)->data;
    raw_entry(p,".          ",5,0); p[11]=0x10;
    raw_entry(p+32,"..         ",0,0); p[43]=0x10;
    raw_entry(p+64,"NESTED     ",8,0); p[75]=0x10;
    raw_entry(p+96,"FILE    BIN",0,0);
    p=page(&f->disk,f->disk.data+6,1)->data;
    raw_entry(p,".          ",8,0); p[11]=0x10;
    raw_entry(p+32,"..         ",5,0); p[43]=0x10;
    OK(fat_volume_init(&v,&f->id,pool,5)); OK(fat_root(&v,3,&root));
    OK(fat_open(&root,U("SUB"),3,&sub)); OK(fat_open(&sub,U("NESTED"),3,&nested));
    OK(fat_open(&sub,U("FILE.BIN"),3,&file));
    OK(check_directory_call(f,&sub,8,&c)); CHECK(c.count==4 && c.examined==5);
    OK(check_directory_call(f,&nested,3,&c)); CHECK(c.count==2 && c.examined==3);
    CHECK(check_directory_call(f,&file,8,&c)==F_ARGUMENT);
    p=page(&f->disk,f->disk.data+3,1)->data; wr16(p+26,6);
    CHECK(check_directory_call(f,&sub,8,&c)==F_CORRUPT && c.issue==FC_DOT && c.observed==6 && c.expected==5);
    wr16(p+26,5); wr16(p+32+26,2);
    CHECK(check_directory_call(f,&sub,8,&c)==F_CORRUPT && c.issue==FC_DOT && c.expected==0);
    wr16(p+32+26,0); p[0]=0xe5;
    CHECK(check_directory_call(f,&sub,8,&c)==F_CORRUPT && c.issue==FC_DOT);
    p[0]='.'; p[1]='x';
    CHECK(check_directory_call(f,&sub,8,&c)==F_CORRUPT && c.issue==FC_DOT);
    p=page(&f->disk,f->disk.data,1)->data; raw_entry(p,".          ",2,0); p[11]=0x10;
    CHECK(check_directory_call(f,&root,8,&c)==F_CORRUPT && c.issue==FC_DOT);
    OK(fat_close(&file)); OK(fat_close(&nested)); OK(fat_close(&sub)); OK(fat_close(&root));
    CHECK(!f->disk.writes); destroy(f);
}
