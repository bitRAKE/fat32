/* Independent range adapter over the mounted logical provider, including its
   staged view. Physical batching belongs to the actual caller's device layer. */
typedef struct StreamRange {
    SectorOps *ops; unsigned calls,sectors,maximum;
    int fail_after,short_success,overreport;
} StreamRange;
extern int abi_range(void *,FatRangeRequest *);
static int stream_range(void *context,FatRangeRequest *request) {
    StreamRange *r=context; unsigned i; int status;
    CHECK(request->count && request->count<=r->maximum);
    ++r->calls; r->sectors+=request->count; request->done=0;
    if(r->overreport) { request->done=request->count+1; return F_OK; }
    for(i=0;i<request->count;i++) {
        if(r->fail_after==0) return r->short_success?F_OK:F_IO;
        if(r->fail_after>0) --r->fail_after;
        status=r->ops->read(r->ops->context,request->lba+i,(unsigned char*)request->data+(size_t)i*r->ops->sector_bytes);
        if(status) return status;
        ++request->done;
    }
    return F_OK;
}
static void test_stream(unsigned bytes,unsigned spc) {
    Fixture *f=fixture(bytes,spc); FatVolume v={0}; FatObject pool[6];
    FatHandle root={0},a={0},b={0},other={0}; FatStream stream={0},second={0};
    uint32_t map[6]={0},map2[6]={0}; FatStreamWorkspace workspace={map,6,0},space2={map2,6,0};
    StreamRange range={&f->fault,0,0,13,-1,0,0}; FatRangeOps inner={&range,stream_range,13,0},ops={&inner,abi_range,13,0};
    unsigned length=bytes*spc*4+17,i,offset,chunk; unsigned char *data=malloc(length),*out=malloc(length+2);
    FatTransfer request; uint64_t reads,version; uint32_t value;
    report("stream maps / sector and coalesced reads / independent handle / zero steady metadata / coherent overwrite");
    CHECK(data && out); for(i=0;i<length;i++) data[i]=(unsigned char)(i*37+(i>>8)+19);
    OK(fat_volume_init(&v,&f->id,pool,6)); OK(fat_root(&v,3,&root));
    shared_new(&root,U("stream"),0,&a); shared_new(&root,U("other"),0,&other);
    request=(FatTransfer){data,0,length,0}; OK(fat_write_at(&a,&request));
    OK(ABI(fat_stream_open,&a,&stream,&workspace,0)); CHECK(stream.count==5);
    OK(fat_open(&root,U("stream"),3,&b)); OK(fat_close(&a));
    OK(fat_stream_open(&b,&second,&space2)); CHECK(!memcmp(map,map2,20));
    reads=f->provider_reads;
    for(i=0;i<(length+bytes-1)/bytes;i++) {
        uint64_t lba=UINT64_MAX;
        OK(ABI(fat_stream_sector,&stream,i,&lba,0));
        CHECK(lba==f->disk.data+(uint64_t)(map[i/spc]-2)*spc+i%spc);
    }
    { uint64_t lba=UINT64_MAX;
      CHECK(fat_stream_sector(&stream,(length+bytes-1)/bytes,&lba)==F_END && lba==UINT64_MAX);
      CHECK(fat_stream_sector(&stream,UINT64_MAX,&lba)==F_END && lba==UINT64_MAX); }
    CHECK(f->provider_reads==reads);
    OK(fat_get(&f->id,65000,&value)); reads=f->provider_reads;
    request=(FatTransfer){out+1,0,length,99}; OK(ABI(fat_stream_read,&stream,&request,0,0));
    CHECK(request.done==length && !memcmp(data,out+1,length));
    CHECK(f->provider_reads-reads==(length+bytes-1)/bytes); /* Data only, no FAT/dir reads. */
    for(i=0;i<4;i++) {
        offset=i==0?0:i==1?3:i==2?bytes-1:bytes*spc-7;
        chunk=length-offset; range.calls=range.sectors=0;
        request=(FatTransfer){out+1,offset,chunk,99}; OK(ABI(fat_stream_read_range,&stream,&request,&ops,0));
        CHECK(request.done==chunk && !memcmp(data+offset,out+1,chunk));
        CHECK(range.calls && range.calls<=(chunk/bytes+12)/13+1);
    }
    /* Same-file content overwrite and unrelated chain changes preserve maps. */
    version=stream.version; data[7]^=0x67;
    request=(FatTransfer){data+7,7,1,0}; OK(fat_write_at(&b,&request));
    OK(fat_handle_resize(&other,bytes*spc*2)); OK(fat_handle_rename(&b,U("renamed")));
    CHECK(b.object->chain_version==version);
    request=(FatTransfer){out,0,31,0}; OK(fat_stream_read_range(&stream,&request,&ops)); CHECK(!memcmp(data,out,31));
    CHECK(fat_unlink(&root,U("renamed"))==F_BUSY);
    f->fail_stage=0; CHECK(fat_handle_resize(&b,length+1)==F_IO); f->fail_stage=-1;
    OK(fat_stream_read(&second,&request));
    request=(FatTransfer){out,UINT64_MAX,31,99}; reads=f->provider_reads;
    OK(fat_stream_read_range(&stream,&request,&ops)); CHECK(!request.done && f->provider_reads==reads);
    OK(fat_handle_resize(&b,length+1)); request=(FatTransfer){out,0,31,99};
    reads=f->provider_reads; CHECK(fat_stream_read(&stream,&request)==F_STALE && request.done==0 && f->provider_reads==reads);
    CHECK(fat_stream_read_range(&second,&request,&ops)==F_STALE);
    { uint64_t lba=UINT64_MAX; CHECK(fat_stream_sector(&stream,0,&lba)==F_STALE && lba==UINT64_MAX); }
    OK(ABI(fat_stream_close,&stream,0,0,0)); OK(fat_stream_close(&second));
    OK(fat_close(&b)); OK(fat_close(&other)); OK(fat_close(&root)); OK(fat_volume_close(&v));
    free(data); free(out); destroy(f);
}
static void test_stream_limits(void) {
    Fixture *f=fixture(512,1); FatVolume v={0}; FatObject pool[4]; FatHandle root={0},a={0};
    FatStream stream={0}; struct { uint32_t before,map[5],after; } guarded={0xCA123456,{0},0xFE789123};
    FatStreamWorkspace workspace={guarded.map,2,0}; unsigned char bytes[2200]={0x53},out[2200]; FatTransfer request;
    StreamRange range={&f->fault,0,0,8,-1,0,0}; FatRangeOps inner={&range,stream_range,8,0},ops={&inner,abi_range,8,0};
    uint64_t reads; unsigned fault;
    report("stream setup bounds / prefix failure / short and invalid provider completions / empty file / stale mount");
    OK(fat_volume_init(&v,&f->id,pool,4)); OK(fat_root(&v,3,&root)); shared_new(&root,U("data"),0,&a);
    workspace.capacity=0; workspace.map=NULL; OK(fat_stream_open(&a,&stream,&workspace)); CHECK(!stream.count);
    { uint64_t lba=UINT64_MAX; CHECK(fat_stream_sector(&stream,0,&lba)==F_END && lba==UINT64_MAX); }
    request=(FatTransfer){out,0,sizeof(out),99}; reads=f->provider_reads;
    OK(fat_stream_read(&stream,&request)); CHECK(!request.done && f->provider_reads==reads); OK(fat_stream_close(&stream));
    workspace.map=guarded.map; workspace.capacity=2;
    request=(FatTransfer){bytes,0,sizeof(bytes),0}; OK(fat_write_at(&a,&request)); reads=f->provider_reads;
    CHECK(fat_stream_open(&a,&stream,&workspace)==F_MEMORY && !stream.handle.volume && f->provider_reads==reads);
    workspace.capacity=5; workspace.reserved=1; CHECK(fat_stream_open(&a,&stream,&workspace)==F_ARGUMENT);
    workspace.reserved=0; workspace.map=(uint32_t*)(UINTPTR_MAX-4);
    CHECK(fat_stream_open(&a,&stream,&workspace)==F_ARGUMENT); workspace.map=guarded.map;
    OK(fat_stream_open(&a,&stream,&workspace)); CHECK(guarded.before==0xCA123456 && guarded.after==0xFE789123);
    CHECK(fat_stream_open(&a,&stream,&workspace)==F_BUSY);
    OK(sb_commit(&f->buffer)); f->disk.fail_read=1;
    request=(FatTransfer){out,3,sizeof(out),99}; CHECK(fat_stream_read(&stream,&request)==F_IO);
    CHECK(request.done==509 && !memcmp(out,bytes+3,509)); f->disk.fail_read=-1;
    range.fail_after=2; request=(FatTransfer){out,3,sizeof(out),99};
    CHECK(fat_stream_read_range(&stream,&request,&ops)==F_IO);
    CHECK(request.done==1533 && !memcmp(out,bytes+3,1533));
    for(fault=0;fault<3;fault++) {
        range.fail_after=2; range.short_success=fault==1; range.overreport=fault==2;
        request=(FatTransfer){out,0,sizeof(out),99};
        CHECK(ABI(fat_stream_read_range,&stream,&request,&ops,0)==F_IO);
        CHECK(request.done==(fault==2?0u:1024u));
        if(request.done) CHECK(!memcmp(out,bytes,request.done));
    }
    range.overreport=0; range.short_success=0; range.fail_after=-1;
    ops.maximum=0; CHECK(fat_stream_read_range(&stream,&request,&ops)==F_ARGUMENT); ops.maximum=8;
    ops.reserved=1; CHECK(fat_stream_read_range(&stream,&request,&ops)==F_ARGUMENT); ops.reserved=0;
    OK(fat_invalidate(&f->id)); CHECK(fat_stream_read(&stream,&request)==F_STALE && !request.done);
    CHECK(fat_stream_close(&stream)==F_STALE); CHECK(fat_close(&a)==F_STALE); CHECK(fat_close(&root)==F_STALE);
    destroy(f);
}
static void test_stream_fragmented(void) {
    Fixture *f=fixture(512,1); Image *d=&f->disk; FatVolume volume={0}; FatObject pool[3];
    FatHandle root={0},opened={0}; FatStream stream={0}; uint32_t map[600],chain[600],value;
    FatStreamWorkspace workspace={map,600,0}; unsigned i,copy,length=600*512-17;
    unsigned char *out=malloc(length); FatTransfer request={out,0,length,99}; uint64_t reads;
    StreamRange range={&f->fault,0,0,64,-1,0,0}; FatRangeOps inner={&range,stream_range,64,0},ops={&inner,abi_range,64,0};
    report("600-cluster map / contiguous and backward fragmented extents / exact range limit / damaged setup");
    CHECK(out);
    for(i=0;i<600;i++) chain[i]=i<300?30000+i:10000+(i-300)*2;
    raw_entry(page(d,d->data,1)->data,"MAPPED  BIN",chain[0],length);
    for(i=0;i<600;i++) {
        for(copy=0;copy<2;copy++) fat_value(d,copy,chain[i],i==599?0x0FFFFFFF:chain[i+1]);
        memset(page(d,d->data+(uint64_t)chain[i]-2,1)->data,(int)(i^(i>>8)),512);
    }
    OK(fat_mount(&f->id,&f->fault,NULL,&f->workspace));
    OK(fat_volume_init(&volume,&f->id,pool,3)); OK(fat_root(&volume,1,&root));
    OK(fat_open(&root,U("mapped.bin"),1,&opened)); reads=f->provider_reads;
    OK(fat_stream_open(&opened,&stream,&workspace)); CHECK(f->provider_reads-reads==8);
    CHECK(stream.count==600 && !memcmp(map,chain,sizeof(map)));
    reads=f->provider_reads;
    for(i=0;i<600;i++) { uint64_t lba=0; OK(fat_stream_sector(&stream,i,&lba)); CHECK(lba==d->data+(uint64_t)chain[i]-2); }
    CHECK(f->provider_reads==reads);
    OK(fat_get(&f->id,65000,&value)); reads=f->provider_reads;
    OK(fat_stream_read_range(&stream,&request,&ops));
    CHECK(request.done==length && range.calls==304 && f->provider_reads-reads==600);
    printf("    setup: 599 links, 8 FAT-sector reads; steady: 304 ranges, 1 edge sector, 0 metadata reads\n");
    for(i=0;i<length;i++) CHECK(out[i]==(unsigned char)((i/512)^((i/512)>>8)));
    OK(fat_stream_close(&stream));
    fat_value(d,0,chain[10],0x0FFFFFFF); f->id.fat_lba=UINT64_MAX;
    CHECK(fat_stream_open(&opened,&stream,&workspace)==F_CORRUPT && !stream.handle.volume);
    fat_value(d,0,chain[10],chain[11]); f->id.fat_lba=UINT64_MAX; f->disk.fail_read=0;
    CHECK(fat_stream_open(&opened,&stream,&workspace)==F_IO && !stream.handle.volume);
    f->disk.fail_read=-1; OK(fat_close(&opened)); OK(fat_close(&root)); free(out); destroy(f);
}
static void test_stream_ordered(void) {
    CommitFixture *c=commit_fixture(512,1,128,128); FatStream stream={0}; uint32_t map[6];
    FatStreamWorkspace workspace={map,6,0}; unsigned char input[3000],out[3200]; unsigned i;
    StreamRange device={&c->backend,0,0,8,-1,0,0}; FatRangeOps inner={&device,stream_range,8,0},backend={&inner,abi_range,8,0};
    FatOrderRange order_range={&c->order,&backend}; FatRangeOps logical={&order_range,fat_order_read_range,13,0};
    FatTransfer request={input,0,sizeof(input),0}; FatRangeRequest direct={0,out,2,99};
    report("ordered range adapter / pending and backend segments / coherent overwrite / bounded provider / partial failures");
    for(i=0;i<sizeof(input);i++) input[i]=(unsigned char)(i*19+(i>>8));
    OK(fat_write_at(&c->opened,&request)); OK(fat_stream_open(&c->opened,&stream,&workspace));
    request=(FatTransfer){out,0,sizeof(input),99};
    OK(fat_stream_read_range(&stream,&request,&logical)); CHECK(!device.calls && !memcmp(input,out,sizeof(input)));
    OK(commit_run(c,0));
    device.calls=device.sectors=0; OK(fat_stream_read_range(&stream,&request,&logical));
    CHECK(device.calls==1 && device.sectors==5 && !memcmp(input,out,sizeof(input)));
    for(i=1024;i<1536;i++) input[i]^=0x67;
    request=(FatTransfer){input+1024,1024,512,0}; OK(fat_write_at(&c->opened,&request));
    device.calls=device.sectors=0; request=(FatTransfer){out,0,sizeof(input),99};
    OK(ABI(fat_stream_read_range,&stream,&request,&logical,0));
    CHECK(device.calls==2 && device.sectors==4 && !memcmp(input,out,sizeof(input)));
    device.fail_after=2; CHECK(fat_stream_read_range(&stream,&request,&logical)==F_IO);
    CHECK(request.done==1536 && !memcmp(input,out,1536)); device.fail_after=-1;
    device.overreport=1; CHECK(fat_stream_read_range(&stream,&request,&logical)==F_IO && !request.done); device.overreport=0;
    direct.lba=c->base->id.total_sectors-1;
    CHECK(ABI(fat_order_read_range,&order_range,&direct,0,0)==F_RANGE && !direct.done);
    direct.lba=UINT64_MAX; CHECK(fat_order_read_range(&order_range,&direct)==F_RANGE);
    direct.lba=0; direct.data=(void*)(UINTPTR_MAX-100); CHECK(fat_order_read_range(&order_range,&direct)==F_ARGUMENT);
    direct.data=out; c->order.active=2; CHECK(fat_order_read_range(&order_range,&direct)==F_BUSY); c->order.active=0;
    c->order.poisoned=1; CHECK(fat_order_read_range(&order_range,&direct)==F_IO); c->order.poisoned=0;
    OK(commit_run(c,0)); OK(fat_stream_close(&stream)); commit_destroy(c);
}
