/* Export production commit callbacks, with an independently encoded starting
   volume. The Python consumer materializes interrupted states and runs fsck. */
static int export_commit_trace(const char *path,unsigned operation,unsigned geometry) {
    unsigned bps=geometry?4096:512,spc=geometry?16:1;
    CommitFixture *c=commit_fixture(bps,spc,2048,4096); Image *d=&c->base->disk;
    unsigned length=bps*spc*2+17,i; unsigned char *data=malloc(length);
    FatTransfer request={data,0,length,0}; FatStamp stamp={0,0,0,0,0,0,0x21};
    FatHandle created={0}; uint16_t name[256]; FatCreate create_request={name,1,0};
    uint32_t geometry_header[2]={bps,spc};
    CHECK(data && operation<6 && geometry<2);
    /* The ABI fixture intentionally gives FAT copies different reserved high
       nibbles. Remove that irrelevant oracle distraction for these images. */
    for(i=0;i<2;i++) fat_value(d,i,1,0x0FFFFFFF);
    c->base->id.fat_lba=UINT64_MAX;
    for(i=0;i<2;i++) {
        unsigned char *boot=page(d,i?6:0,0)->data;
        memcpy(boot+71,"NO NAME    ",11); wr16(boot+24,63); wr16(boot+26,255); boot[64]=0x80;
    }
    for(i=0;i<length;i++) data[i]=(unsigned char)(i*29+(i>>8)+71);
    if(operation!=0 && operation!=4) { OK(fat_write_at(&c->opened,&request)); OK(commit_run(c,0)); }
    CHECK(fopen_s(&commit_trace,path,"wb")==0 && commit_trace);
    CHECK(fwrite("FATORD01",1,8,commit_trace)==8);
    CHECK(fwrite(geometry_header,1,8,commit_trace)==8);
    CHECK(fwrite(&d->ops.sectors,1,8,commit_trace)==8);
    for(i=0;i<4096;i++) {
        Page *p; for(p=d->bucket[i];p;p=p->next) commit_trace_event(1,0,p->lba,p->data,bps);
    }
    commit_trace_event(0,0,0,NULL,0);
    switch(operation) {
    case 0: OK(fat_write_at(&c->opened,&request)); break;
    case 1: OK(fat_handle_resize(&c->opened,17)); break;
    case 2: OK(fat_handle_rename(&c->opened,U("renamed"))); break;
    case 3: OK(fat_close(&c->opened)); OK(fat_unlink(&c->root,U("saved"))); break;
    case 4:
        for(i=0;i<255;i++) name[i]=(uint16_t)('a'+i%26); name[255]=0;
        OK(fat_new(&c->root,&create_request,&created)); break;
    case 5: OK(fat_handle_set_info(&c->opened,&stamp)); break;
    }
    OK(commit_run(c,0)); CHECK(c->report.effect==FE_COMMITTED);
    commit_trace_event(4,c->report.phase,0,NULL,0);
    CHECK(fclose(commit_trace)==0); commit_trace=NULL;
    if(created.volume) OK(fat_close(&created));
    free(data); commit_destroy(c); return 0;
}
