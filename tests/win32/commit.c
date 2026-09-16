typedef struct CommitFixture {
    Fixture *base; FatOrder order; FatOrderWorkspace workspace;
    SectorOps inner,backend; FatVolume volume; FatObject pool[6];
    FatHandle root,opened; FatCommitReport report;
    int fail_read,fail_write,fail_flush,corrupt_write,strict,dirty_flushed;
    uint64_t reads,writes,flushes;
} CommitFixture;
static struct { HANDLE entered,resume; volatile LONG armed; } commit_pause;
static FILE *commit_trace;
static void commit_trace_event(unsigned kind,unsigned phase,uint64_t lba,const void *data,unsigned bytes) {
    uint32_t header[2]={kind,phase};
    if(!commit_trace) return;
    CHECK(fwrite(header,1,sizeof(header),commit_trace)==sizeof(header));
    CHECK(fwrite(&lba,1,sizeof(lba),commit_trace)==sizeof(lba));
    if(data) CHECK(fwrite(data,1,bytes,commit_trace)==bytes);
}
static int commit_selected_status(CommitFixture *c,int clean) {
    FatIdentity *id=&c->base->id; unsigned copy=id->mirrored?0:id->active_fat;
    unsigned limit=id->mirrored?id->fat_count:copy+1;
    for(;copy<limit;copy++) {
        Page *p=page(&c->base->disk,id->fat_start+(uint64_t)copy*id->fat_sectors,0);
        if(!p || !!(rd32(p->data+4)&0x8000000)!=clean) return 0;
    }
    return 1;
}
static int commit_read(void *v,uint64_t lba,void *out) {
    CommitFixture *c=v; ++c->reads;
    if(c->fail_read==0) return F_IO;
    if(c->fail_read>0) --c->fail_read;
    return read_image(&c->base->disk,lba,out);
}
static int commit_write(void *v,uint64_t lba,const void *in) {
    CommitFixture *c=v; Image *d=&c->base->disk; int status;
    if(InterlockedCompareExchange(&commit_pause.armed,0,1)==1) {
        CHECK(SetEvent(commit_pause.entered)); CHECK(WaitForSingleObject(commit_pause.resume,10000)==WAIT_OBJECT_0);
    }
    ++c->writes;
    if(c->report.phase<FP_ADMIT) {
        CHECK(c->dirty_flushed);
        CHECK(commit_selected_status(c,0));
        if(lba==32 || lba==32+d->fat_sectors) CHECK(!(rd32((const unsigned char*)in+4)&0x8000000));
    }
    if(c->fail_write==0) {
        /* A failed callback can already have modified part of its sector. */
        memcpy(page(d,lba,1)->data,in,d->bytes/2); return F_IO;
    }
    if(c->fail_write>0) --c->fail_write;
    status=write_image(d,lba,in);
    if(!status) commit_trace_event(2,c->report.phase,lba,in,d->bytes);
    if(!status && c->corrupt_write==0) page(d,lba,0)->data[17]^=1;
    if(c->corrupt_write>0) --c->corrupt_write;
    return status;
}
static int commit_flush(void *v) {
    CommitFixture *c=v; Image *d=&c->base->disk; ++c->flushes;
    if(c->fail_flush==0) return F_IO;
    if(c->fail_flush>0) --c->fail_flush;
    commit_trace_event(3,c->report.phase,UINT64_MAX,NULL,0);
    if(c->report.phase==FP_DIRTY && commit_selected_status(c,0))
        c->dirty_flushed=1;
    /* Normal policy flushes whole phases. Strict additionally flushes each
       sector, whose phase can still have unapplied mirror/metadata writes. */
    if(!c->strict && c->base->id.mirrored && c->base->id.fat_count==2) (void)order_directory(d,2,0);
    return F_OK;
}
static CommitFixture *commit_fixture(unsigned bps,unsigned spc,unsigned capacity,unsigned slots) {
    CommitFixture *c=calloc(1,sizeof(*c)); FatEntry original; CHECK(c);
    c->base=fixture(bps,spc); original=create(c->base,2,U("saved"),0);
    OK(fat_resize(&c->base->id,&original,17)); OK(sb_commit(&c->base->buffer));
    fat_value(&c->base->disk,0,1,0xAFFFFFFF); fat_value(&c->base->disk,1,1,0xBFFFFFFF);
    c->workspace.bytes=(uint64_t)capacity*(bps+16);
    c->workspace.data=malloc((size_t)c->workspace.bytes); c->workspace.index=malloc((size_t)slots*4);
    c->workspace.slots=slots; CHECK(c->workspace.data && c->workspace.index);
    c->inner=c->base->disk.ops; c->inner.context=c; c->inner.read=commit_read; c->inner.write=commit_write;
    c->inner.begin=commit_flush;
    c->backend=c->inner; c->backend.context=&c->inner;
    c->backend.read=abi_read; c->backend.write=abi_write; c->backend.flush=abi_begin;
    c->fail_read=c->fail_write=c->fail_flush=c->corrupt_write=-1;
    OK(ABI(fat_order_init,&c->order,&c->backend,&c->workspace,0));
    OK(fat_mount(&c->base->id,&c->order.ops.base,NULL,&c->base->workspace));
    OK(fat_volume_init(&c->volume,&c->base->id,c->pool,6)); OK(fat_root(&c->volume,3,&c->root));
    OK(fat_open(&c->root,U("saved"),3,&c->opened));
    c->reads=c->writes=c->flushes=0; return c;
}
static void commit_destroy(CommitFixture *c) {
    int status;
    if(c->opened.volume) { status=fat_close(&c->opened); CHECK(status==F_OK || status==F_STALE); }
    if(c->root.volume) { status=fat_close(&c->root); CHECK(status==F_OK || status==F_STALE); }
    status=fat_volume_close(&c->volume); CHECK(status==F_OK || status==F_STALE);
    OK(fat_order_discard(&c->order,&c->base->id));
    free(c->workspace.data); free(c->workspace.index); destroy(c->base); free(c);
}
static int commit_run(CommitFixture *c,int strict) {
    c->reads=c->writes=c->flushes=0; c->dirty_flushed=0; c->strict=strict;
    return strict?ABI(fat_order_commit_verified,&c->order,&c->base->id,&c->report,0):
                  ABI(fat_order_commit,&c->order,&c->base->id,&c->report,0);
}
static void test_commit(void) {
    unsigned geometry,strict;
    report("indexed phase commit / backend-only verification / dirty throughout mutation / coherent handles / four geometries");
    for(geometry=0;geometry<4;geometry++) for(strict=0;strict<2;strict++) {
        unsigned bps=geometry&1?4096:512,spc=geometry<2?1:geometry==2?128:16;
        CommitFixture *c=commit_fixture(bps,spc,1024,2048); unsigned length=bps*spc*2+17,i;
        unsigned char *data=malloc(length),*out=malloc(length); FatTransfer request;
        FatCheck evidence; uint64_t version,generation; FatRecord record;
        CHECK(data && out); for(i=0;i<length;i++) data[i]=(unsigned char)(i*29+71);
        request=(FatTransfer){data,0,length,0}; OK(fat_write_at(&c->opened,&request)); CHECK(request.done==length);
        CHECK(c->order.accepted && !c->writes && !c->flushes);
        CHECK(fat_handle_resize(&c->opened,1)==F_BUSY);
        request=(FatTransfer){out,0,length,0}; OK(fat_read_at(&c->opened,&request)); CHECK(!memcmp(data,out,length));
        OK(fat_check_file(&c->opened,8,&evidence));
        version=c->opened.object->chain_version; generation=c->base->id.generation;
        OK(commit_run(c,(int)strict)); CHECK(c->report.effect==FE_COMMITTED);
        CHECK(c->report.writes==c->writes && c->report.flushes==c->flushes);
        CHECK(c->flushes>=4 && (!strict || c->flushes>c->writes));
        CHECK(!c->order.used && !c->order.accepted && !c->order.poisoned && c->dirty_flushed);
        CHECK(c->base->id.generation==generation && c->opened.object->chain_version==version);
        OK(fat_check_fresh(&c->opened,&evidence)); OK(fat_handle_info(&c->opened,&record));
        CHECK(order_fat(&c->base->disk,1,0)==0xFFFFFFF && order_fat(&c->base->disk,1,1)==0xFFFFFFF);
        CHECK(rd32(page(&c->base->disk,32,0)->data+4)==0xAFFFFFFF);
        CHECK(rd32(page(&c->base->disk,32+c->base->disk.fat_sectors,0)->data+4)==0xBFFFFFFF);
        CHECK(order_directory(&c->base->disk,2,0)==1);
        for(i=0;i<length;i++) CHECK(order_byte(&c->base->disk,record.cluster,i)==data[i]);
        OK(fat_handle_resize(&c->opened,17)); OK(commit_run(c,(int)strict));
        CHECK(order_chain(&c->base->disk,record.cluster)==1);
        OK(fat_handle_rename(&c->opened,U("renamed"))); OK(commit_run(c,(int)strict));
        OK(fat_close(&c->opened)); OK(fat_unlink(&c->root,U("renamed"))); OK(commit_run(c,(int)strict));
        CHECK(order_directory(&c->base->disk,2,0)==0 && order_fat(&c->base->disk,record.cluster,0)==0);
        OK(commit_run(c,(int)strict)); CHECK(c->report.effect==FE_NONE && !c->writes && !c->flushes);
        free(data); free(out); commit_destroy(c);
    }
}
static void test_commit_limits(void) {
    CommitFixture *c=commit_fixture(512,1,1,2); FatObject saved[6]; unsigned char bytes[1400]={0};
    FatTransfer request={bytes,0,sizeof(bytes),99}; FatOrder bad={0}; FatOrderWorkspace invalid=c->workspace;
    report("caller-funded plan capacity / hash collision and occupancy / phase versions / rollback / discard");
    memcpy(saved,c->pool,sizeof(saved));
    CHECK(fat_write_at(&c->opened,&request)==F_MEMORY && request.done==0);
    CHECK(!c->order.used && !c->order.active && !c->order.accepted && !memcmp(saved,c->pool,sizeof(saved)));
    CHECK(!c->writes && !c->flushes);
    invalid.bytes=527; CHECK(fat_order_init(&bad,&c->backend,&invalid)==F_MEMORY);
    invalid=c->workspace; invalid.slots=3; CHECK(fat_order_init(&bad,&c->backend,&invalid)==F_ARGUMENT);
    invalid=c->workspace; invalid.reserved=1; CHECK(fat_order_init(&bad,&c->backend,&invalid)==F_ARGUMENT);
    invalid=c->workspace; invalid.data=(void*)(UINTPTR_MAX-100);
    CHECK(fat_order_init(&bad,&c->backend,&invalid)==F_ARGUMENT);
    commit_destroy(c);
    c=commit_fixture(512,1,8,8);
    {
        SectorOps *o=&c->order.ops.base; unsigned char a[512],b[512]; unsigned i;
        memset(a,0x37,sizeof(a));
        OK(ABI(o->begin,o->context,0,0,0));
        for(i=0;i<4;i++) OK(ABI(o->write,o->context,1000+i*8,a,0)); /* deliberate hash collisions */
        CHECK(c->order.unique==4 && c->order.used==4);
        CHECK(o->write(o->context,1032,a)==F_MEMORY && c->order.used==4);
        memset(a,0x71,sizeof(a)); OK(o->write(o->context,1000,a)); CHECK(c->order.used==4);
        OK(ABI(c->order.ops.barrier,o->context,FO_PUBLISH,0,0));
        memset(a,0x93,sizeof(a)); OK(o->write(o->context,1000,a)); CHECK(c->order.used==5);
        OK(ABI(o->read,o->context,1000,b,0)); CHECK(b[0]==0x93);
        CHECK(c->order.data[16]==0x71 && c->order.data[4*c->order.stride+16]==0x93);
        (void)ABI(o->end,o->context,0,0,0); CHECK(!c->order.used && !c->order.unique);
        OK(o->read(o->context,1000,b)); CHECK(b[0]==0);
    }
    OK(fat_handle_set_info(&c->opened,&(FatStamp){0,0,0,0,0,0,0x20}));
    OK(ABI(fat_order_discard,&c->order,&c->base->id,0,0));
    CHECK(fat_check_fresh(&c->opened,&(FatCheck){0})==F_STALE && !c->writes);
    commit_destroy(c);
}
static void test_commit_failures(void) {
    unsigned kind,fail,strict;
    report("every write/flush/read failure in both policies / uncertainty / first evidence / no blind retry / handle retirement");
    for(strict=0;strict<2;strict++) for(kind=0;kind<3;kind++) {
      for(fail=0;fail<80;fail++) {
        CommitFixture *c=commit_fixture(512,1,64,64); unsigned char bytes[1400]={0x39};
        FatTransfer request={bytes,0,sizeof(bytes),0}; FatCommitReport before; uint64_t writes,flushes,generation;
        int status;
        OK(fat_write_at(&c->opened,&request)); generation=c->base->id.generation;
        if(!kind) c->fail_write=(int)fail;
        else if(kind==1) c->fail_flush=(int)fail;
        else c->fail_read=(int)fail;
        status=commit_run(c,(int)strict);
        if(!status) { commit_destroy(c); break; }
        if(c->report.effect==FE_NONE) {
            CHECK(kind==2 && !c->writes && !c->order.poisoned && c->base->id.generation==generation);
            c->fail_read=-1; OK(commit_run(c,(int)strict)); commit_destroy(c); continue;
        }
        CHECK(status==F_IO && c->report.effect==FE_UNCERTAIN && c->order.poisoned==1);
        CHECK(c->order.used && c->order.accepted && !c->order.active && c->base->id.generation==generation+1);
        if(kind<2) CHECK(c->report.operation==(uint32_t)(kind?FI_FLUSH:FI_WRITE));
        else CHECK(c->report.operation==FI_READ || c->report.operation==FI_VERIFY);
        CHECK(fat_read_at(&c->opened,&request)==F_STALE && fat_handle_resize(&c->opened,1)==F_STALE);
        before=c->report; writes=c->writes; flushes=c->flushes;
        CHECK(fat_order_commit(&c->order,&c->base->id,&c->report)==F_IO);
        CHECK(!memcmp(&before,&c->report,sizeof(before)) && c->writes==writes && c->flushes==flushes);
        CHECK(c->order.ops.base.begin(&c->order)==F_IO);
        OK(fat_order_discard(&c->order,&c->base->id)); CHECK(c->order.poisoned==1);
        commit_destroy(c);
      }
      CHECK(fail>2 && fail<80);
    }
}
static void test_commit_gate_variants(void) {
    unsigned variant;
    report("commit gate across suspended backend / active FAT only / single FAT / creation and directory growth");
    for(variant=0;variant<3;variant++) {
        CommitFixture *c=commit_fixture(512,1,256,256); unsigned char input[1200],out[1200],inactive[512];
        FatTransfer write={input,0,sizeof(input),0},read={out,0,sizeof(out),99};
        memset(input,0x57,sizeof(input));
        if(variant) {
            unsigned char *boot=page(&c->base->disk,0,0)->data;
            memcpy(inactive,page(&c->base->disk,32+c->base->disk.fat_sectors,0)->data,512);
            if(variant==1) { wr16(boot+40,0x81); memcpy(inactive,page(&c->base->disk,32,0)->data,512); }
            else { boot[16]=1; wr32(boot+36,c->base->disk.fat_sectors*2); }
            OK(fat_mount(&c->base->id,&c->order.ops.base,NULL,&c->base->workspace));
            CHECK(fat_close(&c->opened)==F_STALE && fat_close(&c->root)==F_STALE);
            OK(fat_volume_init(&c->volume,&c->base->id,c->pool,6)); OK(fat_root(&c->volume,3,&c->root));
            OK(fat_open(&c->root,U("saved"),3,&c->opened));
        }
        OK(fat_write_at(&c->opened,&write));
        if(!variant) {
            SharedThread work={&c->volume,{(uintptr_t)fat_order_commit,{(uintptr_t)&c->order,(uintptr_t)&c->base->id,(uintptr_t)&c->report,0}}};
            FatCall competing={(uintptr_t)fat_read_at,{(uintptr_t)&c->opened,(uintptr_t)&read,0,0}};
            HANDLE thread; DWORD code;
            commit_pause.entered=CreateEventW(NULL,TRUE,FALSE,NULL); commit_pause.resume=CreateEventW(NULL,TRUE,FALSE,NULL);
            CHECK(commit_pause.entered && commit_pause.resume); commit_pause.armed=1;
            thread=CreateThread(NULL,0,shared_worker,&work,0,NULL); CHECK(thread);
            CHECK(WaitForSingleObject(commit_pause.entered,10000)==WAIT_OBJECT_0);
            CHECK(c->volume.gate==1 && c->order.active==2);
            CHECK(fat_call_locked(&c->volume,&competing)==F_BUSY && read.done==99);
            CHECK(SetEvent(commit_pause.resume)); CHECK(WaitForSingleObject(thread,10000)==WAIT_OBJECT_0);
            CHECK(GetExitCodeThread(thread,&code) && code==F_OK && !c->volume.gate && !c->order.active);
            OK(fat_call_locked(&c->volume,&competing)); CHECK(read.done==sizeof(out) && !memcmp(input,out,sizeof(input)));
            CHECK(CloseHandle(thread)); CHECK(CloseHandle(commit_pause.entered)); CHECK(CloseHandle(commit_pause.resume));
        } else {
            OK(commit_run(c,0)); CHECK(commit_selected_status(c,1));
            CHECK(!memcmp(inactive,page(&c->base->disk,variant==1?32:32+c->base->disk.fat_sectors,0)->data,512));
            OK(fat_read_at(&c->opened,&read)); CHECK(!memcmp(input,out,sizeof(input)));
        }
        {
            uint16_t name[256]; FatHandle created={0}; FatCreate request={name,1,0}; unsigned i;
            for(i=0;i<255;i++) name[i]=(uint16_t)('a'+i%26); name[255]=0;
            OK(fat_new(&c->root,&request,&created)); OK(commit_run(c,0));
            CHECK(c->root.object->record.cluster==2); OK(fat_close(&created));
        }
        commit_destroy(c);
    }
}
static void test_commit_admission(void) {
    unsigned fault;
    report("pre-write read failure versus known dirty/damaged admission / strict mismatch / failed verification flush");
    for(fault=0;fault<6;fault++) {
        CommitFixture *c=commit_fixture(512,1,64,64); unsigned char bytes[800]={0x71};
        FatTransfer request={bytes,0,sizeof(bytes),0}; uint64_t generation;
        OK(fat_write_at(&c->opened,&request)); generation=c->base->id.generation;
        if(fault==0) c->fail_read=0;
        if(fault==1) fat_value(&c->base->disk,1,1,0x07FFFFFF);
        if(fault==2) fat_value(&c->base->disk,1,1,0x0FFFFFFE);
        if(fault==3) c->corrupt_write=0;
        if(fault==4) c->fail_flush=0;
        if(fault==5) c->fail_read=3; /* admission copies, dirty reload, first readback */
        if(fault<3) {
            int expected=fault==0?F_IO:fault==1?F_ATTENTION:F_CORRUPT;
            CHECK(commit_run(c,0)==expected);
            CHECK(c->report.effect==FE_NONE && !c->writes && !c->flushes && c->base->id.generation==generation);
            if(!fault) { CHECK(!c->order.poisoned); c->fail_read=-1; OK(commit_run(c,0)); }
            else {
                CHECK(c->order.poisoned==2);
                OK(fat_order_discard(&c->order,&c->base->id));
                CHECK(c->order.ops.base.begin(&c->order)==expected);
            }
        } else {
            CHECK(commit_run(c,1)==(fault==3?F_VERIFY:F_IO));
            CHECK(c->report.effect==FE_UNCERTAIN && c->order.poisoned==1 && c->report.writes==1);
            CHECK(c->report.operation==(uint32_t)(fault==4?FI_FLUSH:FI_VERIFY));
            CHECK(c->base->id.generation==generation+1);
        }
        commit_destroy(c);
    }
}
