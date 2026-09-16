/* Retain actual formatter output as sparse-sector records for independent
   image assembly. Optional population uses public shared library operations. */
static void format_image_file(FatHandle *parent,const uint16_t *name,const char *path) {
    FILE *input=0; CHECK(fopen_s(&input,path,"rb")==0 && input);
    CHECK(!fseek(input,0,SEEK_END)); long size=ftell(input); CHECK(size>0);
    CHECK(!fseek(input,0,SEEK_SET)); unsigned char *bytes=malloc((size_t)size); CHECK(bytes);
    CHECK(fread(bytes,1,(size_t)size,input)==(size_t)size); CHECK(!fclose(input));
    FatHandle file={0}; FatCreate request={name,0,0}; OK(fat_new(parent,&request,&file));
    FatTransfer transfer={bytes,0,(uint32_t)size,0}; OK(fat_write_at(&file,&transfer));
    CHECK(transfer.done==(uint32_t)size); OK(fat_close(&file)); free(bytes);
}
static int export_format_trace(const char *output,unsigned bps,unsigned cb,unsigned verified,
                               const char *efi,const char *cache) {
    CHECK(bps>=512 && bps<=4096 && !(bps&(bps-1)) && cb>=bps && cb<=262144 && !(cb&(cb-1)) && cb/bps<=128);
    FormatFixture *f=format_fixture(bps,cb); Image *d=&f->base->disk;
    if(!strcmp(efi,"unlabeled")) memset(f->options.label,0,11);
    f->work.bytes=bps*(verified?2:1); OK(format_run(f,verified)); format_oracle(f,verified);
    if(strcmp(efi,"-") && strcmp(efi,"unlabeled")) {
        FatIdentity identity={0}; FatVolume volume={0}; FatObject pool[8]={0};
        FatHandle root={0},directory={0},boot={0}; unsigned char scratch[3*4096];
        FatWorkspace work={scratch,3*bps,0};
        OK(sb_init(&f->base->buffer,&d->ops)); OK(fat_mount(&identity,&f->base->buffer.ops,0,&work));
        OK(fat_volume_init(&volume,&identity,pool,8)); OK(fat_root(&volume,3,&root));
        shared_new(&root,U("EFI"),1,&directory); shared_new(&directory,U("BOOT"),1,&boot);
        format_image_file(&boot,U("BOOTX64.EFI"),efi);
        OK(fat_close(&boot)); OK(fat_close(&directory));
        shared_new(&root,U("CACHE"),1,&directory);
        format_image_file(&directory,U("2026090809.efi"),cache); OK(fat_close(&directory));
        static const char sentinel[]="Existing sentinel: native startup must preserve me.";
        FatCreate request={U("KEEP.TXT"),0,0}; OK(fat_new(&root,&request,&boot));
        FatTransfer t={(void *)sentinel,0,sizeof(sentinel)-1,0}; OK(fat_write_at(&boot,&t)); OK(fat_close(&boot));
        OK(fat_close(&root)); OK(fat_volume_close(&volume)); OK(sb_commit(&f->base->buffer));
    }
    FILE *trace=0; CHECK(fopen_s(&trace,output,"wb")==0 && trace);
    uint32_t geometry[2]={bps,cb/bps};
    CHECK(fwrite("FATFMT01",1,8,trace)==8); CHECK(fwrite(geometry,1,8,trace)==8);
    CHECK(fwrite(&d->ops.sectors,1,8,trace)==8);
    for(unsigned bucket=0;bucket<4096;++bucket) for(Page *p=d->bucket[bucket];p;p=p->next) {
        if(p->lba>=d->ops.sectors) continue; /* Outside-extent test sentinel. */
        CHECK(fwrite(&p->lba,1,8,trace)==8);
        CHECK(fwrite(p->data,1,bps,trace)==bps);
    }
    CHECK(!fclose(trace)); format_destroy(f); return 0;
}
