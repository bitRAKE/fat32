/* This test-only trace is not a production commit buffer. It materializes each
   phase independently so on-disk references can be checked without calling
   the library's FAT/chain/directory routines as an oracle. */
typedef struct OrderEvent {
    uint64_t lba;
    uint32_t reason; /* zero = staged sector write, nonzero = phase boundary */
    unsigned char data[4096];
} OrderEvent;
typedef struct OrderTrace {
    FatOrderedOps ops; Fixture *fixture;
    OrderEvent *events; unsigned count;
    int fail_barrier,accepted;
} OrderTrace;
static int order_read(void *v,uint64_t lba,void *out) {
    OrderTrace *t=v; SectorOps *o=&t->fixture->fault; return o->read(o->context,lba,out);
}
static int order_write(void *v,uint64_t lba,const void *in) {
    OrderTrace *t=v; SectorOps *o=&t->fixture->fault; int s=o->write(o->context,lba,in);
    if(s) return s;
    CHECK(t->count<1024); t->events[t->count].reason=0; t->events[t->count].lba=lba;
    memcpy(t->events[t->count++].data,in,o->sector_bytes); return F_OK;
}
static int order_begin(void *v) {
    OrderTrace *t=v; SectorOps *o=&t->fixture->fault;
    t->count=0; t->accepted=0; return o->begin(o->context);
}
static void order_end(void *v,int accept) {
    OrderTrace *t=v; SectorOps *o=&t->fixture->fault;
    o->end(o->context,accept); t->accepted=accept; if(!accept) t->count=0;
}
static int order_barrier(void *v,uint32_t reason) {
    OrderTrace *t=v;
    CHECK(reason>=FO_PREPARED && reason<=FO_DETACH);
    if(t->fail_barrier==0) return F_MEMORY;
    if(t->fail_barrier>0) --t->fail_barrier;
    CHECK(t->count<1024); t->events[t->count++].reason=reason; return F_OK;
}
static void order_attach(OrderTrace *t,Fixture *f) {
    memset(t,0,sizeof(*t)); t->fixture=f; t->fail_barrier=-1;
    t->events=calloc(1024,sizeof(*t->events)); CHECK(t->events);
    t->ops.base=f->fault; t->ops.base.context=t; t->ops.base.read=order_read;
    t->ops.base.write=order_write; t->ops.base.begin=order_begin; t->ops.base.end=order_end;
    t->ops.base.reserved=sizeof(t->ops); t->ops.barrier=order_barrier; t->ops.revision=1;
    OK(fat_mount(&f->id,&t->ops.base,NULL,&f->workspace));
}
static Image *order_clone(Image *source) {
    Image *out=calloc(1,sizeof(*out)); unsigned i; Page *p; CHECK(out);
    out->bytes=source->bytes; out->spc=source->spc; out->fat_sectors=source->fat_sectors;
    out->data=source->data; out->clusters=source->clusters;
    for(i=0;i<4096;i++) for(p=source->bucket[i];p;p=p->next)
        memcpy(page(out,p->lba,1)->data,p->data,source->bytes);
    return out;
}
static void order_destroy_image(Image *d) {
    unsigned i; Page *p;
    for(i=0;i<4096;i++) { p=d->bucket[i]; while(p) { Page *next=p->next; free(p); p=next; } }
    free(d);
}
static uint32_t order_fat(Image *d,uint32_t cluster,unsigned copy) {
    uint64_t offset=(uint64_t)cluster*4; Page *p=page(d,32+(uint64_t)copy*d->fat_sectors+offset/d->bytes,0);
    return p?rd32(p->data+offset%d->bytes)&0xFFFFFFF:0;
}
static uint32_t order_chain(Image *d,uint32_t first) {
    uint32_t count=0,next;
    if(!first) return 0;
    do {
        CHECK(first>=2 && first<=d->clusters+1 && ++count<=d->clusters);
        next=order_fat(d,first,0); CHECK(next==order_fat(d,first,1)); first=next;
    } while(next<0xFFFFFF8);
    return count;
}
static unsigned order_directory(Image *d,uint32_t first,unsigned depth) {
    uint32_t cluster=first,steps=0; unsigned children=0,sector,offset;
    CHECK(depth<4); CHECK(order_chain(d,first)>0);
    do {
        CHECK(cluster>=2 && cluster<=d->clusters+1 && ++steps<=d->clusters);
        for(sector=0;sector<d->spc;sector++) {
            Page *p=page(d,d->data+(uint64_t)(cluster-2)*d->spc+sector,0);
            if(!p) return children;
            for(offset=0;offset<d->bytes;offset+=32) {
                unsigned char *r=p->data+offset; uint32_t start,n;
                if(!r[0]) return children;
                if(r[0]==0xE5 || (r[11]&8) || r[0]=='.') continue;
                start=(((uint32_t)rd16(r+20)<<16)|rd16(r+26))&0xFFFFFFF;
                n=order_chain(d,start); ++children;
                if(r[11]&16) (void)order_directory(d,start,depth+1);
                else CHECK((uint64_t)n*d->bytes*d->spc>=rd32(r+28));
            }
        }
        cluster=order_fat(d,cluster,0);
    } while(cluster<0xFFFFFF8);
    return children;
}
static unsigned char *order_record(Image *d,const FatEntry *entry) {
    Page *p=page(d,entry->sector,0); CHECK(p); return p->data+entry->offset;
}
static unsigned char order_byte(Image *d,uint32_t cluster,uint32_t position) {
    uint32_t cb=d->bytes*d->spc; Page *p;
    while(position>=cb) { position-=cb; cluster=order_fat(d,cluster,0); }
    CHECK(cluster>=2 && cluster<=d->clusters+1);
    p=page(d,d->data+(uint64_t)(cluster-2)*d->spc+position/d->bytes,0);
    return p?p->data[position%d->bytes]:0;
}
static void order_replay(OrderTrace *trace,Image *disk,unsigned operation,const FatEntry *old,const FatEntry *now,
                         unsigned target,const unsigned char *written) {
    unsigned i,j,prepared=0,published=0,unpublished=0,detached=0;
    for(i=0;i<=trace->count;i++) {
        OrderEvent *event=i<trace->count?&trace->events[i]:NULL;
        if(event && !event->reason) {
            memcpy(page(disk,event->lba,1)->data,event->data,disk->bytes); continue;
        }
        (void)order_directory(disk,2,0);
        if(!event) break;
        prepared+=event->reason==FO_PREPARED; published+=event->reason==FO_PUBLISH;
        unpublished+=event->reason==FO_UNPUBLISH; detached+=event->reason==FO_DETACH;
        if(operation==0 && event->reason==FO_PREPARED) {
            CHECK(rd32(order_record(disk,old)+28)==old->size);
            CHECK(order_fat(disk,old->cluster,0)>=0xFFFFFF8); /* old one-cluster tail is detached */
        }
        if(operation==0 && event->reason==FO_PUBLISH) {
            CHECK(rd32(order_record(disk,old)+28)==old->size);
            for(j=0;j<target;j++) CHECK(order_byte(disk,now->cluster,j)==written[j]);
        }
        if(operation==1 && event->reason==FO_UNPUBLISH) {
            CHECK(rd32(order_record(disk,old)+28)==target);
            CHECK(order_chain(disk,old->cluster)==3); /* no free/cut before publication */
        }
        if(operation==1 && event->reason==FO_DETACH) CHECK(order_chain(disk,now->cluster)==1);
        if(operation==2 && event->reason==FO_UNPUBLISH) {
            CHECK(order_record(disk,old)[0]==0xE5); CHECK(order_chain(disk,old->cluster)==3);
        }
        if(operation==3 && event->reason==FO_PUBLISH && published==2) {
            CHECK(order_record(disk,old)[0]!=0xE5);
            CHECK(rd32(order_record(disk,now)+28)==old->size);
        }
    }
    if(operation==0) CHECK(prepared==1 && published==1 && rd32(order_record(disk,now)+28)==target);
    if(operation==1) CHECK(unpublished==1 && detached==1);
    if(operation==2) CHECK(unpublished==1 && order_fat(disk,old->cluster,0)==0);
    if(operation==3) CHECK(published==2 && order_record(disk,old)[0]==0xE5);
    if(operation==4) CHECK(prepared>=1 && published==1 && order_directory(disk,2,0)==1);
}
static void test_ordering(void) {
    unsigned operation,geometry;
    report("mutation phases / detached growth / data before size / unpublish before free / safe end-marker replacement");
    for(geometry=0;geometry<4;geometry++) for(operation=0;operation<5;operation++) {
        Fixture *f=fixture(geometry&1?4096:512,geometry<2?1:geometry==2?128:16);
        OrderTrace trace; FatEntry old={0},now={0};
        Image *model; unsigned cb=f->id.cluster_bytes,target=cb*2+17;
        unsigned char *data=malloc(target); FatTransfer request; unsigned i;
        CHECK(data); for(i=0;i<target;i++) data[i]=(unsigned char)(i*29+37);
        if(operation!=4) {
            old=create(f,2,U("before"),0); OK(fat_resize(&f->id,&old,operation==0?17:target));
            OK(sb_commit(&f->buffer));
        } else {
            unsigned char *p=page(&f->disk,f->disk.data,1)->data;
            for(i=32;i<f->id.sector_bytes;i+=32) raw_entry(p+i,"GHOST   BIN",5,1);
        }
        model=order_clone(&f->disk); order_attach(&trace,f);
        if(operation!=4) old=lookup(f,2,U("before")); now=old;
        if(operation==0) { request=(FatTransfer){data,0,target,0}; OK(ABI(fat_write,&f->id,&now,&request,0)); }
        if(operation==1) { target=17; OK(ABI(fat_resize,&f->id,&now,target,0)); }
        if(operation==2) OK(ABI(fat_remove,&f->id,&old,0,0));
        if(operation==3) OK(ABI(fat_rename,&f->id,&now,U("after"),0));
        if(operation==4) now=create(f,2,U("new directory"),1);
        CHECK(trace.accepted && trace.count); order_replay(&trace,model,operation,&old,&now,target,data);
        order_destroy_image(model); free(trace.events); free(data); destroy(f);
    }
}
static void test_ordering_failures(void) {
    unsigned fail,operation;
    report("ordering callback failures / rollback / extension version checks / overallocated equal-size resize version");
    for(operation=0;operation<4;operation++) {
      for(fail=0;fail<8;fail++) {
        Fixture *f=fixture(512,1); OrderTrace trace; FatEntry entry=create(f,2,U("kept"),0),before;
        unsigned char bytes[1800]={0}; FatTransfer request={bytes,0,sizeof(bytes),0};
        void *head; uint64_t pages; int status;
        OK(fat_resize(&f->id,&entry,operation?1300:1)); OK(sb_commit(&f->buffer)); order_attach(&trace,f);
        entry=lookup(f,2,U("kept")); before=entry; head=f->buffer.head; pages=f->buffer.pages;
        trace.fail_barrier=(int)fail;
        if(operation==0) status=fat_write(&f->id,&entry,&request);
        else if(operation==1) status=fat_resize(&f->id,&entry,17);
        else if(operation==2) status=fat_remove(&f->id,&entry);
        else status=fat_rename(&f->id,&entry,U("renamed"));
        if(status) {
            CHECK(status==F_MEMORY && !memcmp(&entry,&before,sizeof(entry)));
            CHECK(!trace.accepted && !trace.count && f->buffer.head==head && f->buffer.pages==pages);
        }
        free(trace.events); destroy(f); if(!status) break;
      }
      CHECK(fail<8);
    }
    {
        Fixture *f=fixture(512,1); OrderTrace trace; FatEntry entry; FatCreate request={U("x"),0,0};
        order_attach(&trace,f); trace.ops.base.reserved=72;
        CHECK(fat_create(&f->id,2,&request,&entry)==F_ARGUMENT && !f->buffer.active);
        trace.ops.base.reserved=80; trace.ops.revision=2;
        CHECK(fat_create(&f->id,2,&request,&entry)==F_ARGUMENT && !f->buffer.active);
        trace.ops.revision=1; trace.ops.flags=1;
        CHECK(fat_create(&f->id,2,&request,&entry)==F_ARGUMENT && !f->buffer.active);
        trace.ops.flags=0; trace.ops.barrier=NULL;
        CHECK(fat_create(&f->id,2,&request,&entry)==F_ARGUMENT && !f->buffer.active);
        free(trace.events); destroy(f);
    }
    {
        Fixture *f=fixture(512,1); FatVolume v={0}; FatObject pool[3]; FatHandle root={0},opened={0};
        FatCheck evidence; uint64_t version;
        raw_entry(page(&f->disk,f->disk.data,1)->data,"EXCESS  BIN",5,1);
        fat_value(&f->disk,0,5,6); fat_value(&f->disk,1,5,6);
        fat_value(&f->disk,0,6,0xFFFFFFF); fat_value(&f->disk,1,6,0xFFFFFFF);
        OK(fat_volume_init(&v,&f->id,pool,3)); OK(fat_root(&v,3,&root));
        OK(fat_open(&root,U("EXCESS.BIN"),3,&opened)); OK(fat_check_file(&opened,3,&evidence));
        CHECK(evidence.count==2 && evidence.flags==FC_EXTRA); version=opened.object->chain_version;
        OK(fat_handle_resize(&opened,1)); CHECK(opened.object->chain_version==version+1);
        CHECK(fat_check_fresh(&opened,&evidence)==F_STALE);
        OK(fat_check_file(&opened,3,&evidence)); CHECK(evidence.count==1 && evidence.flags==0);
        OK(fat_close(&opened)); OK(fat_close(&root)); destroy(f);
    }
}
