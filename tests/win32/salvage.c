static void salvage_data(Fixture *f,unsigned cluster) {
    for(unsigned s=0;s<f->disk.spc;++s) {
        unsigned char *p=page(&f->disk,f->disk.data+(uint64_t)(cluster-2)*f->disk.spc+s,1)->data;
        for(unsigned b=0;b<f->disk.bytes;++b) p[b]=(unsigned char)(cluster*13+s*7+b);
    }
}
static void salvage_bytes(Fixture *f,const unsigned *chain,unsigned offset,unsigned length,const unsigned char *data) {
    for(unsigned b=0;b<length;++b) {
        unsigned logical=offset+b,c=chain[logical/f->id.cluster_bytes],in=logical%f->id.cluster_bytes;
        CHECK(data[b]==(unsigned char)(c*13+(in/f->disk.bytes)*7+in%f->disk.bytes));
    }
}
static void test_salvage(void) {
    report("read-only salvage / all 31 geometries / contiguous and backward extents / clipped EOF / ABI");
    unsigned geometries=0;
    for(unsigned bps=512;bps<=4096;bps*=2) for(unsigned spc=1;spc<=128 && bps*spc<=262144;spc*=2) {
        ++geometries; Fixture *f=fixture(bps,spc); const unsigned chain[]={5,6,9,8};
        unsigned cb=f->id.cluster_bytes,size=4*cb-13;
        for(unsigned i=0;i<4;++i) { fat_value(&f->disk,0,chain[i],i==3?0x0fffffff:chain[i+1]); salvage_data(f,chain[i]); }
        struct {uint64_t before; FatExtent extents[3]; uint64_t after;} map={0}; map.before=map.after=0xabcdef0123456789;
        FatSalvageRequest request={map.extents,3,5,size,4,6,0}; FatSalvage plan;
        uint64_t writes=f->disk.writes,pages=f->buffer.pages;
        OK(ABI(fat_salvage_plan,&f->id,&request,&plan,0));
        CHECK(plan.check.scope==FC_SALVAGE && plan.available==size && plan.check.count==4);
        CHECK(plan.extent_count==3 && plan.check.examined==4 && plan.pairs==4 && plan.check.last==8);
        CHECK(map.extents[0].first==5 && map.extents[0].count==2 && map.extents[1].first==9 && map.extents[2].first==8);
        unsigned char *data=malloc(size+34); CHECK(data); memset(data,0xa5,size+34);
        FatTransfer transfer={data+16,0,size+1,99};
        CHECK(ABI(fat_salvage_read,&plan,&transfer,0,0)==F_END && transfer.done==size);
        salvage_bytes(f,chain,0,size,data+16);
        for(unsigned i=0;i<16;++i) CHECK(data[i]==0xa5 && data[size+16+i]==0xa5);
        for(unsigned start=cb-5;start<size;start+=cb) {
            unsigned length=size-start<29?size-start:29;
            transfer=(FatTransfer){data+1,start,length,99}; OK(fat_salvage_read(&plan,&transfer));
            CHECK(transfer.done==length); salvage_bytes(f,chain,start,length,data+1);
        }
        transfer=(FatTransfer){data,size,1,99}; CHECK(fat_salvage_read(&plan,&transfer)==F_END && !transfer.done);
        transfer=(FatTransfer){NULL,UINT64_MAX,0,99}; OK(fat_salvage_read(&plan,&transfer)); CHECK(!transfer.done);
        CHECK(f->disk.writes==writes && f->buffer.pages==pages && plan.check.status==F_OK);
        CHECK(map.before==0xabcdef0123456789 && map.after==0xabcdef0123456789);
        free(data); destroy(f);
    }
    CHECK(geometries==31);
}
static void test_salvage_damage(void) {
    report("salvage unique prefixes / cycle, short chain, invalid link / bounded work and capacity / data failures");
    Fixture *f=fixture(512,1); unsigned char data[1600]; FatExtent extents[3]; FatSalvage plan;
    FatSalvageRequest request={extents,3,5,1536,8,16,0};
    const unsigned chain[]={5,6,9}; for(unsigned i=0;i<3;++i) salvage_data(f,chain[i]);
    fat_value(&f->disk,0,5,6); fat_value(&f->disk,0,6,5);
    CHECK(ABI(fat_salvage_plan,&f->id,&request,&plan,0)==F_CORRUPT);
    CHECK(plan.check.issue==FC_CYCLE && plan.available==1024 && plan.check.count==2 && plan.check.examined==2);
    CHECK(plan.check.cluster==5 && plan.check.sector==UINT64_MAX && extents[0].count==2);
    FatSalvage saved=plan; memset(data,0xa5,sizeof(data)); FatTransfer transfer={data,0,sizeof(data),0};
    CHECK(fat_salvage_read(&plan,&transfer)==F_END && transfer.done==1024); salvage_bytes(f,chain,0,1024,data);
    for(unsigned i=1024;i<sizeof(data);++i) CHECK(data[i]==0xa5);
    CHECK(!memcmp(&saved,&plan,sizeof(plan)));
    request.bytes=1024; OK(fat_salvage_plan(&f->id,&request,&plan)); CHECK(plan.check.flags==FC_EXTRA);
    request.bytes=1536;
    const unsigned bad[]={0,1,0x0ffffff0,0x0ffffff7,65532,0x00fffffe};
    for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);++i) {
        fat_value(&f->disk,0,6,bad[i]); f->id.fat_lba=UINT64_MAX;
        CHECK(fat_salvage_plan(&f->id,&request,&plan)==F_CORRUPT && plan.check.issue==FC_LINK);
        CHECK(plan.available==512 && plan.check.observed==bad[i] && plan.check.cluster==6 && plan.check.examined==2);
    }
    fat_value(&f->disk,0,6,0xafffffff); f->id.fat_lba=UINT64_MAX;
    CHECK(fat_salvage_plan(&f->id,&request,&plan)==F_CORRUPT && plan.check.issue==FC_SHORT && plan.available==1024);
    fat_value(&f->disk,0,6,9); fat_value(&f->disk,0,9,0x0fffffff); f->id.fat_lba=UINT64_MAX;
    request.capacity=1;
    CHECK(fat_salvage_plan(&f->id,&request,&plan)==F_MEMORY && plan.check.issue==FC_WORKSPACE);
    CHECK(plan.available==1024 && plan.extent_count==1 && extents[0].count==2 && plan.check.examined==2);
    request.capacity=3; request.fat_budget=1;
    CHECK(fat_salvage_plan(&f->id,&request,&plan)==F_LIMIT && plan.check.issue==FC_BUDGET && plan.available==512);
    request.fat_budget=8; request.pair_budget=0;
    CHECK(fat_salvage_plan(&f->id,&request,&plan)==F_LIMIT && plan.check.issue==FC_PAIR_BUDGET && plan.available==512 && !plan.pairs);
    request.pair_budget=16; OK(fat_salvage_plan(&f->id,&request,&plan)); saved=plan;
    f->disk.fail_read=1; memset(data,0xa5,sizeof(data)); transfer=(FatTransfer){data,0,1536,0};
    CHECK(ABI(fat_salvage_read,&plan,&transfer,0,0)==F_IO && transfer.done==512);
    salvage_bytes(f,chain,0,512,data); for(unsigned i=512;i<sizeof(data);++i) CHECK(data[i]==0xa5);
    CHECK(!memcmp(&saved,&plan,sizeof(plan))); f->disk.fail_read=-1;
    OK(fat_invalidate(&f->id)); CHECK(fat_salvage_read(&plan,&transfer)==F_STALE && !transfer.done);
    request.first=0; CHECK(fat_salvage_plan(&f->id,&request,&plan)==F_CORRUPT && plan.check.issue==FC_SHORT && !plan.available);
    request.bytes=0; OK(fat_salvage_plan(&f->id,&request,&plan)); CHECK(!plan.available && !plan.check.examined);
    destroy(f);
}
static void test_salvage_large(void) {
    report("salvage maximum file size / 64-bit prefix arithmetic / exact budgets / last two bytes");
    Fixture *f=fixture(4096,64); FatExtent extent; FatSalvage plan;
    unsigned count=16384,last=5+count-1;
    for(unsigned c=5;c<=last;++c) fat_value(&f->disk,0,c,c==last?0x0fffffff:c+1);
    salvage_data(f,last);
    FatSalvageRequest request={&extent,1,5,UINT32_MAX,count,count-1,0};
    OK(fat_salvage_plan(&f->id,&request,&plan));
    CHECK(plan.available==UINT32_MAX && plan.extent_count==1 && extent.count==count && plan.pairs==count-1);
    CHECK(plan.check.examined==count && plan.check.count==count && plan.check.last==last);
    unsigned char data[4]={0xa5,0xa5,0xa5,0xa5}; FatTransfer transfer={data,UINT32_MAX-2ull,4,0};
    CHECK(fat_salvage_read(&plan,&transfer)==F_END && transfer.done==2);
    for(unsigned i=0;i<2;++i) CHECK(data[i]==(unsigned char)(last*13+63*7+4093+i));
    CHECK(data[2]==0xa5 && data[3]==0xa5 && !f->disk.writes); destroy(f);
}
static void test_salvage_admission(void) {
    report("salvage admission / attempted FAT budgets and IO evidence / immutable source / active FAT / accepted staging");
    Fixture *f=fixture(512,1); FatExtent extents[3]; FatSalvage plan; unsigned char data[1024];
    FatSalvageRequest request={extents,3,5,1024,8,16,0};
    fat_value(&f->disk,0,5,133); fat_value(&f->disk,0,133,0x0fffffff); salvage_data(f,5); salvage_data(f,133);
    f->disk.fail_read=1;
    CHECK(fat_salvage_plan(&f->id,&request,&plan)==F_IO && plan.check.issue==FC_IO);
    CHECK(plan.available==512 && plan.check.examined==2 && plan.check.cluster==133 && plan.check.sector==33);
    f->disk.fail_read=-1;
    const unsigned chain[]={5,133}; FatTransfer transfer={data,0,512,0}; OK(fat_salvage_read(&plan,&transfer)); salvage_bytes(f,chain,0,512,data);
    uint64_t reads=f->provider_reads;
    request.reserved=1; CHECK(fat_salvage_plan(&f->id,&request,&plan)==F_ARGUMENT && !plan.available); request.reserved=0;
    request.extents=NULL; CHECK(fat_salvage_plan(&f->id,&request,&plan)==F_ARGUMENT);
    request.extents=(FatExtent *)(uintptr_t)(UINT64_MAX-7); CHECK(fat_salvage_plan(&f->id,&request,&plan)==F_ARGUMENT);
    request.extents=extents; request.capacity=0; CHECK(fat_salvage_plan(&f->id,&request,&plan)==F_MEMORY && !plan.check.examined);
    request.capacity=3; request.fat_budget=0; CHECK(fat_salvage_plan(&f->id,&request,&plan)==F_LIMIT && !plan.check.examined);
    request.fat_budget=8; f->id.transaction=1; CHECK(fat_salvage_plan(&f->id,&request,&plan)==F_BUSY); f->id.transaction=0;
    CHECK(f->provider_reads==reads);
    OK(fat_salvage_plan(&f->id,&request,&plan)); reads=f->provider_reads;
    transfer=(FatTransfer){NULL,0,1,99}; CHECK(fat_salvage_read(&plan,&transfer)==F_ARGUMENT && !transfer.done);
    transfer=(FatTransfer){(void *)(uintptr_t)UINT64_MAX,0,1,99}; CHECK(fat_salvage_read(&plan,&transfer)==F_ARGUMENT && !transfer.done);
    transfer=(FatTransfer){data,UINT64_MAX,1,99}; CHECK(fat_salvage_read(&plan,&transfer)==F_RANGE && !transfer.done);
    f->id.transaction=1; transfer=(FatTransfer){data,0,1,99}; CHECK(fat_salvage_read(&plan,&transfer)==F_BUSY && !transfer.done); f->id.transaction=0;
    CHECK(f->provider_reads==reads);
    FatView view={0}; unsigned char scratch[1536]; FatWorkspace workspace={scratch,sizeof(scratch),0};
    fat_value(&f->disk,1,5,0x0fffffff); FatIdentity source=f->id;
    OK(fat_view_open(&view,&f->id,1,&workspace));
    CHECK(fat_salvage_plan(&view.identity,&request,&plan)==F_CORRUPT && plan.check.issue==FC_SHORT && plan.available==512);
    OK(fat_view_close(&view)); CHECK(fat_salvage_read(&plan,&transfer)==F_STALE && !transfer.done);
    CHECK(!memcmp(&source,&f->id,sizeof(source)) && !f->disk.writes);
    FatEntry entry=create(f,2,U("pending salvage"),0); memset(data,0x71,sizeof(data)); transfer=(FatTransfer){data,0,sizeof(data),0};
    OK(fat_write(&f->id,&entry,&transfer)); request.first=entry.cluster;
    OK(fat_salvage_plan(&f->id,&request,&plan)); memset(data,0,sizeof(data)); transfer=(FatTransfer){data,0,sizeof(data),0};
    OK(fat_salvage_read(&plan,&transfer)); for(unsigned i=0;i<sizeof(data);++i) CHECK(data[i]==0x71);
    CHECK(!f->disk.writes && f->buffer.pages); destroy(f);
}
