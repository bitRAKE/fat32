/* Included by test.c to reuse its independent sparse provider and ABI probes. */
static void shared_new(FatHandle *parent,const uint16_t *name,int directory,FatHandle *out) {
    FatCreate request={name,(uint32_t)directory,0};
    OK(ABI(fat_new,parent,&request,out,0));
}
static void test_shared(unsigned bytes,unsigned spc) {
    Fixture *f=fixture(bytes,spc); FatVolume v={0}; FatObject pool[8],saved[8];
    FatHandle root={0},a={0},a2={0},b={0},renamed={0},retired; FatIterator it={0};
    FatRecord info; FatEntry entry; FatTransfer t; FatBuffer next; FatStamp stamp={0},observed;
    uint32_t size=bytes*spc*2+17,value; unsigned char *in=malloc(size),*out=malloc(size);
    uint64_t generation=f->id.generation,chain_version,file_version,reads; unsigned i;
    report("shared canonical objects / independent positions / scoped publication / rename / slot reuse");
    CHECK(in && out); for(i=0;i<size;i++) in[i]=(unsigned char)(i*29+(i>>8)+37);
    /* A uint32_t argument does not promise zeroes in its register's high half. */
    OK(ABI(fat_volume_init,&v,&f->id,pool,0xFFFFFFFF00000008ull)); OK(ABI(fat_root,&v,3,&root,0));
    shared_new(&root,U("A.bin"),0,&a); shared_new(&root,U("B.bin"),0,&b);
    OK(ABI(fat_open,&root,U("a.BIN"),3,&a2)); CHECK(a.object==a2.object && a.object!=b.object);
    CHECK(f->id.generation==generation);
    t=(FatTransfer){in,0,size,0}; OK(ABI(fat_write_at,&a,&t,0,0)); CHECK(t.done==size);
    CHECK(a2.object->record.size==size && a2.object->record.cluster==a.object->record.cluster);
    file_version=a.object->file_version; chain_version=a.object->chain_version;
    t=(FatTransfer){in,0,size,0}; OK(fat_write_at(&b,&t));
    CHECK(a.object->file_version==file_version && a.object->chain_version==chain_version);
    CHECK(a.object->record.sector==b.object->record.sector); /* Same directory cache sector. */
    OK(fat_get(&f->id,65000,&value)); /* Evict shared FAT cache without retiring A. */
    t=(FatTransfer){out,0,size,0}; OK(ABI(fat_read_at,&a2,&t,0,0)); CHECK(t.done==size && !memcmp(in,out,size));
    reads=f->provider_reads; t=(FatTransfer){out,0,1,0}; OK(fat_read_at(&a,&t));
    CHECK(f->provider_reads==reads+1 && out[0]==in[0]); /* No directory/chain preflight. */
    next=(FatBuffer){out,7,0}; OK(ABI(fat_read_next,&a,&next,0,0)); CHECK(next.done==7 && a.position==7);
    next=(FatBuffer){out,11,0}; OK(fat_read_next(&a2,&next)); CHECK(a2.position==11 && a.position==7);
    OK(ABI(fat_seek,&a2,3,0,0));
    next=(FatBuffer){in,1,0}; OK(ABI(fat_write_next,&a2,&next,0,0)); CHECK(a2.position==4);
    CHECK(a.object->chain_version==chain_version && a.object->file_version==file_version+1);
    t=(FatTransfer){out,3,1,0}; OK(fat_read_at(&a,&t)); CHECK(out[0]==in[0] && a.position==7);
    OK(ABI(fat_iter_open,&root,&it,0,0));
    memcpy(saved,pool,sizeof(pool)); f->fail_stage=1;
    t=(FatTransfer){in,size,size,99}; CHECK(fat_write_at(&a,&t)==F_IO && t.done==0);
    f->fail_stage=-1; CHECK(!memcmp(saved,pool,sizeof(pool)) && f->id.generation==generation);
    OK(ABI(fat_iter_next,&it,&entry,0,0));
    OK(ABI(fat_handle_rename,&a2,U("Renamed Ω.bin"),0,0));
    CHECK(fat_iter_next(&it,&entry)==F_STALE); OK(ABI(fat_iter_close,&it,0,0,0));
    CHECK(fat_open(&root,U("A.bin"),1,&renamed)==F_NOTFOUND);
    OK(fat_open(&root,U("Renamed Ω.bin"),3,&renamed)); CHECK(renamed.object==a.object);
    CHECK(a.object->chain_version==chain_version);
    OK(ABI(fat_handle_resize,&renamed,2,0,0)); OK(ABI(fat_handle_info,&a2,&info,0,0)); CHECK(info.size==2);
    next=(FatBuffer){out,9,99}; OK(fat_read_next(&a,&next)); CHECK(next.done==0 && a.position==7);
    stamp=(FatStamp){0x4567,0x5B21,0x5B22,0x789A,0x5B23,197,0x21};
    OK(ABI(fat_handle_set_info,&a,&stamp,0,0));
    reads=f->provider_reads; memset(&observed,0xA5,sizeof(observed));
    OK(ABI(fat_handle_get_stamp,&a2,&observed,0,0));
    CHECK(!memcmp(&stamp,&observed,sizeof(stamp)) && f->provider_reads==reads);
    t=(FatTransfer){in,0,1,0}; CHECK(fat_write_at(&a2,&t)==F_READONLY);
    stamp.attributes=0x20; OK(fat_handle_set_info(&a2,&stamp));
    CHECK(fat_unlink(&root,U("Renamed Ω.bin"))==F_BUSY);
    retired=a; OK(ABI(fat_close,&a,0,0,0)); OK(fat_close(&a2)); OK(fat_close(&renamed));
    OK(ABI(fat_unlink,&root,U("Renamed Ω.bin"),0,0));
    shared_new(&root,U("Replacement.bin"),0,&a);
    CHECK(a.object==retired.object && a.incarnation!=retired.incarnation);
    CHECK(fat_handle_info(&retired,&info)==F_STALE);
    CHECK(ABI(fat_handle_get_stamp,&retired,&observed,0,0)==F_STALE);
    stamp.attributes=0x21; CHECK(!memcmp(&stamp,&observed,sizeof(stamp)));
    CHECK(fat_close(&retired)==F_STALE);
    OK(fat_close(&a)); OK(fat_close(&b)); OK(fat_close(&root));
    CHECK(pool[0].references==1); for(i=1;i<8;i++) CHECK(!pool[i].live);
    OK(sb_commit(&f->buffer)); OK(fat_mount(&f->id,&f->fault,NULL,&f->workspace));
    CHECK(fat_root(&v,3,&root)==F_STALE);
    OK(fat_volume_init(&v,&f->id,pool,8)); OK(fat_root(&v,3,&root)); retired=root;
    OK(fat_volume_init(&v,&f->id,pool,8)); CHECK(fat_handle_info(&retired,&info)==F_STALE);
    CHECK(fat_close(&root)==F_STALE && pool[0].references==1);
    free(in); free(out); destroy(f);
}

static void test_shared_capacity(void) {
    Fixture *f=fixture(512,1),*g=fixture(512,1); FatVolume v={0},other={0}; FatObject pool[2],other_pool[2]; FatRecord info;
    FatHandle root={0},a={0},a2={0},b={0}; FatCreate req={U("B"),0,0};
    void *head; uint64_t pages; FatObject saved[2];
    report("bounded canonical pool / no mutation on pool exhaustion / reference/version limits");
    OK(fat_volume_init(&v,&f->id,pool,2)); OK(fat_root(&v,3,&root)); shared_new(&root,U("A"),0,&a);
    CHECK(fat_volume_init(&other,&f->id,other_pool,2)==F_BUSY);
    CHECK(fat_volume_init(&v,&g->id,other_pool,2)==F_BUSY);
    CHECK(fat_volume_close(&v)==F_BUSY);
    OK(fat_open(&root,U("A"),1,&a2)); CHECK(a.object==a2.object);
    memcpy(saved,pool,sizeof(pool)); head=f->buffer.head; pages=f->buffer.pages;
    CHECK(fat_new(&root,&req,&b)==F_MEMORY && !b.volume);
    CHECK(!memcmp(saved,pool,sizeof(pool)) && head==f->buffer.head && pages==f->buffer.pages);
    CHECK(fat_open(&root,U("B"),1,&b)==F_NOTFOUND);
    CHECK(fat_open(&root,U("."),1,&b)==F_NAME && fat_open(&root,U(".."),1,&b)==F_NAME);
    CHECK(fat_root(&v,3,&root)==F_BUSY);
    a.object->file_version=UINT64_MAX; CHECK(fat_handle_resize(&a,10)==F_RANGE);
    a.object->file_version=1; a.object->references=UINT32_MAX;
    CHECK(fat_open(&root,U("A"),1,&b)==F_RANGE && !b.volume); a.object->references=2;
    OK(fat_close(&a)); OK(fat_close(&a2)); shared_new(&root,U("B"),0,&b);
    OK(fat_handle_info(&b,&info)); CHECK(info.size==0); OK(fat_close(&b)); OK(fat_close(&root));
    CHECK(pool[0].references==1); OK(ABI(fat_volume_close,&v,0,0,0));
    CHECK(fat_root(&v,1,&root)==F_STALE);
    OK(fat_volume_init(&other,&f->id,other_pool,2)); OK(fat_root(&other,1,&root));
    OK(fat_close(&root)); OK(fat_volume_close(&other));
    OK(fat_volume_init(&v,&g->id,other_pool,2)); OK(fat_volume_close(&v)); destroy(f); destroy(g);
}

static void test_shared_directories(void) {
    Fixture *f=fixture(512,1); FatVolume v={0}; FatObject pool[12];
    FatHandle root={0},d={0},e={0},child={0},other={0},d2={0};
    FatIterator di={0},ri={0}; FatEntry entry; FatObject *parent;
    FatStamp stamp={0x1234,0x5B21,0x5B22,0x5678,0x5B23,13,0x12},observed;
    report("parent pins / directory rename with live children / scoped iterator invalidation");
    OK(fat_volume_init(&v,&f->id,pool,12)); OK(fat_root(&v,3,&root));
    shared_new(&root,U("D"),1,&d); shared_new(&root,U("E"),1,&e);
    OK(fat_handle_set_info(&d,&stamp)); OK(fat_handle_get_stamp(&d,&observed));
    CHECK(!memcmp(&stamp,&observed,sizeof(stamp)));
    shared_new(&d,U("child"),0,&child); parent=d.object;
    OK(fat_iter_open(&d,&di)); OK(fat_iter_open(&root,&ri));
    shared_new(&e,U("other"),0,&other); OK(fat_iter_next(&di,&entry)); OK(fat_iter_next(&ri,&entry));
    OK(fat_close(&d)); CHECK(parent->live && child.object->parent==parent);
    CHECK(fat_unlink(&root,U("D"))==F_BUSY);
    OK(fat_open(&root,U("D"),3,&d2)); CHECK(d2.object==parent);
    OK(fat_handle_rename(&d2,U("Renamed D"))); CHECK(child.object->parent==parent);
    OK(fat_iter_next(&di,&entry)); CHECK(fat_iter_next(&ri,&entry)==F_STALE);
    OK(fat_iter_close(&ri)); OK(fat_close(&child)); OK(fat_unlink(&d2,U("child")));
    CHECK(fat_iter_next(&di,&entry)==F_STALE); OK(fat_iter_close(&di)); OK(fat_close(&d2));
    CHECK(!parent->live); OK(fat_unlink(&root,U("Renamed D")));
    OK(fat_close(&other)); OK(fat_unlink(&e,U("other"))); OK(fat_close(&e));
    OK(fat_unlink(&root,U("E"))); OK(fat_close(&root)); CHECK(pool[0].references==1); destroy(f);
}

static void test_shared_rollback(void) {
    unsigned op,fail,failed;
    report("shared rollback at every staged write / no stale unrelated or same-file handles");
    for(op=0;op<6;op++) {
        failed=0;
        for(fail=0;fail<100;fail++) {
            Fixture *f=fixture(512,1); FatVolume v={0}; FatObject pool[8],before[8];
            FatHandle root={0},a={0},a2={0},b={0},fresh={0}; FatRecord info;
            FatStamp stamp={0,0,0,0,0,0,0x21}; FatCreate request={U("new dir"),1,0};
            unsigned char data[2000]; FatTransfer t={data,0,sizeof(data),0}; int s;
            uint64_t generation; void *head; uint64_t pages;
            memset(data,0x73,sizeof(data)); OK(fat_volume_init(&v,&f->id,pool,8)); OK(fat_root(&v,3,&root));
            shared_new(&root,U("A"),0,&a); shared_new(&root,U("B"),0,&b);
            OK(fat_write_at(&a,&t)); OK(fat_open(&root,U("A"),3,&a2));
            if(op==4) { OK(fat_close(&a)); OK(fat_close(&a2)); }
            memcpy(before,pool,sizeof(pool)); generation=f->id.generation;
            head=f->buffer.head; pages=f->buffer.pages; f->fail_stage=(int)fail;
            if(op==0) { t=(FatTransfer){data,2000,sizeof(data),0}; s=fat_write_at(&a,&t); }
            else if(op==1) s=fat_handle_resize(&a,17);
            else if(op==2) s=fat_new(&root,&request,&fresh);
            else if(op==3) s=fat_handle_rename(&a,U("renamed"));
            else if(op==4) s=fat_unlink(&root,U("A"));
            else s=fat_handle_set_info(&a,&stamp);
            if(s==F_OK) { destroy(f); break; }
            CHECK(s==F_IO); failed++; CHECK(!memcmp(before,pool,sizeof(pool)));
            CHECK(f->id.next_free==2 && f->id.free_hint==UINT32_MAX);
            CHECK(f->id.fat_lba==UINT64_MAX && f->id.dir_lba==UINT64_MAX);
            CHECK(f->id.generation==generation && head==f->buffer.head && pages==f->buffer.pages);
            if(op==0) CHECK(t.done==0);
            f->fail_stage=-1; OK(fat_handle_info(&b,&info)); CHECK(info.size==0);
            if(op!=4) {
                unsigned char restored[2000]; FatTransfer check={restored,0,sizeof(restored),0};
                OK(fat_handle_info(&a2,&info)); CHECK(info.size==2000);
                OK(fat_read_at(&a2,&check)); CHECK(check.done==sizeof(restored) && !memcmp(data,restored,sizeof(restored)));
            }
            CHECK(!fresh.volume); destroy(f);
        }
        CHECK(failed>0 && fail<100);
    }
}

static void test_shared_workspace(void) {
    unsigned bytes;
    report("sector-sized workspace / exact capacity / canaries / rejected workspace performs no I/O");
    for(bytes=512;bytes<=4096;bytes*=2) {
        Fixture *f=fixture(bytes,1); unsigned char memory[3*4096+32],data[101],out[101];
        FatWorkspace workspace={memory+16,bytes*3,0}; FatVolume v={0}; FatObject pool[3];
        FatHandle root={0},file={0}; FatTransfer t; uint64_t before; unsigned i;
        memset(memory,0xA5,sizeof(memory)); memset(data,0x19,sizeof(data)); before=f->provider_reads;
        workspace.bytes--; CHECK(fat_mount(&f->id,&f->fault,NULL,&workspace)==F_MEMORY);
        CHECK(f->provider_reads==before && !f->id.magic); workspace.bytes++;
        workspace.reserved=1; CHECK(fat_mount(&f->id,&f->fault,NULL,&workspace)==F_ARGUMENT);
        workspace.reserved=0; workspace.data=(void *)(UINTPTR_MAX-32);
        CHECK(fat_mount(&f->id,&f->fault,NULL,&workspace)==F_ARGUMENT && f->provider_reads==before);
        workspace.data=memory+16; OK(fat_mount(&f->id,&f->fault,NULL,&workspace));
        CHECK(f->id.fat==memory+16 && f->id.directory==memory+16+bytes && f->id.scratch==memory+16+bytes*2);
        OK(fat_volume_init(&v,&f->id,pool,3)); OK(fat_root(&v,3,&root)); shared_new(&root,U("small"),0,&file);
        t=(FatTransfer){data,bytes-13,sizeof(data),0}; OK(fat_write_at(&file,&t));
        t=(FatTransfer){out,bytes-13,sizeof(out),0}; OK(fat_read_at(&file,&t)); CHECK(!memcmp(data,out,sizeof(data)));
        OK(fat_close(&file)); OK(fat_unlink(&root,U("small"))); OK(fat_close(&root));
        for(i=0;i<16;i++) CHECK(memory[i]==0xA5);
        for(i=16+bytes*3;i<sizeof(memory);i++) CHECK(memory[i]==0xA5);
        destroy(f);
    }
}

static void test_shared_basic_read(void) {
    Fixture *f=fixture(512,1); FatVolume v={0}; FatObject pool[4];
    FatHandle root={0},a={0}; FatEntry legacy; FatTransfer t; unsigned char out[1030]; uint64_t reads;
    report("ordinary read prefix / malformed encountered link / legacy checked tail / partial I/O");
    raw_entry(page(&f->disk,f->disk.data,1)->data,"SHORT   BIN",5,1024);
    fat_value(&f->disk,0,5,0x0FFFFFFF); fat_value(&f->disk,1,5,0x0FFFFFFF);
    memset(page(&f->disk,f->disk.data+3,1)->data,0x37,512);
    OK(fat_mount(&f->id,&f->fault,NULL,&f->workspace)); legacy=lookup(f,2,U("SHORT.BIN"));
    t=(FatTransfer){out,0,1,0}; CHECK(fat_read(&f->id,&legacy,&t)==F_CORRUPT && t.done==0);
    OK(fat_volume_init(&v,&f->id,pool,4)); OK(fat_root(&v,3,&root)); OK(fat_open(&root,U("SHORT.BIN"),3,&a));
    reads=f->disk.reads; t=(FatTransfer){out,0,1,0}; OK(fat_read_at(&a,&t)); CHECK(out[0]==0x37 && f->disk.reads==reads+1);
    memset(out,0xA5,sizeof(out)); t=(FatTransfer){out,0,1024,0}; CHECK(fat_read_at(&a,&t)==F_CORRUPT && t.done==512);
    CHECK(out[511]==0x37 && out[512]==0xA5);
    t=(FatTransfer){out,0x100000000ull,3,99}; OK(fat_read_at(&a,&t)); CHECK(t.done==0);
    /* Repair fixture outside a managed mount, then start a fresh coherent view. */
    fat_value(&f->disk,0,5,6); fat_value(&f->disk,1,5,6);
    fat_value(&f->disk,0,6,0x0FFFFFFF); fat_value(&f->disk,1,6,0x0FFFFFFF);
    memset(page(&f->disk,f->disk.data+4,1)->data,0x71,512);
    OK(fat_mount(&f->id,&f->fault,NULL,&f->workspace)); CHECK(fat_close(&a)==F_STALE); CHECK(fat_close(&root)==F_STALE);
    OK(fat_volume_init(&v,&f->id,pool,4)); OK(fat_root(&v,1,&root)); OK(fat_open(&root,U("SHORT.BIN"),1,&a));
    f->disk.fail_read=1; memset(out,0xA5,sizeof(out)); t=(FatTransfer){out,0,1024,0};
    CHECK(fat_read_at(&a,&t)==F_IO && t.done==512); CHECK(out[511]==0x37 && out[512]==0xA5);
    f->disk.fail_read=-1; OK(fat_close(&a)); OK(fat_close(&root)); destroy(f);
}

static struct { HANDLE entered,resume; volatile LONG armed; } shared_pause;
static int shared_blocking_read(void *context,uint64_t lba,void *out) {
    if(InterlockedCompareExchange(&shared_pause.armed,0,1)==1) {
        CHECK(SetEvent(shared_pause.entered)); CHECK(WaitForSingleObject(shared_pause.resume,10000)==WAIT_OBJECT_0);
    }
    return fault_read(context,lba,out);
}
typedef struct SharedThread { FatVolume *volume; FatCall call; } SharedThread;
static DWORD WINAPI shared_worker(void *arg) {
    SharedThread *work=arg; return (DWORD)fat_call_locked(work->volume,&work->call);
}
static void test_shared_gate(void) {
    Fixture *f=fixture(512,1); FatVolume v={0}; FatObject pool[4]; FatHandle root={0},a={0},b={0};
    unsigned char in[600],out[600]; FatTransfer write={in,0,sizeof(in),0},read={out,0,sizeof(out),0};
    SharedThread work; FatCall second; HANDLE thread; DWORD code;
    report("actual overlapping callers / gate held across suspended provider / ABI dispatch");
    memset(in,0x51,sizeof(in)); OK(fat_volume_init(&v,&f->id,pool,4)); OK(fat_root(&v,3,&root));
    shared_new(&root,U("A"),0,&a); shared_new(&root,U("B"),0,&b); OK(fat_write_at(&a,&write));
    shared_pause.entered=CreateEventW(NULL,TRUE,FALSE,NULL); shared_pause.resume=CreateEventW(NULL,TRUE,FALSE,NULL);
    CHECK(shared_pause.entered && shared_pause.resume); shared_pause.armed=1; f->inner.read=shared_blocking_read;
    work=(SharedThread){&v,{(uintptr_t)fat_read_at,{(uintptr_t)&a,(uintptr_t)&read,0,0}}};
    thread=CreateThread(NULL,0,shared_worker,&work,0,NULL); CHECK(thread);
    CHECK(WaitForSingleObject(shared_pause.entered,10000)==WAIT_OBJECT_0); CHECK(v.gate==1);
    write.done=99; second=(FatCall){(uintptr_t)fat_write_at,{(uintptr_t)&b,(uintptr_t)&write,0,0}};
    CHECK(ABI(fat_call_locked,&v,&second,0,0)==F_BUSY && write.done==99 && b.object->record.size==0);
    CHECK(SetEvent(shared_pause.resume)); CHECK(WaitForSingleObject(thread,10000)==WAIT_OBJECT_0);
    CHECK(GetExitCodeThread(thread,&code) && code==F_OK && v.gate==0);
    CHECK(read.done==sizeof(out) && !memcmp(in,out,sizeof(in)));
    OK(ABI(fat_call_locked,&v,&second,0,0)); CHECK(write.done==sizeof(in) && b.object->record.size==sizeof(in));
    CHECK(CloseHandle(thread)); CHECK(CloseHandle(shared_pause.entered)); CHECK(CloseHandle(shared_pause.resume));
    f->inner.read=fault_read; OK(fat_close(&a)); OK(fat_close(&b)); OK(fat_close(&root)); destroy(f);
}
