/* Encode names independently; the library must diagnose ambiguous lookup keys. */
static unsigned name_slots(Fixture *f,unsigned start,const uint16_t *name,const char *alias) {
    static const unsigned offsets[]={1,3,5,7,9,14,16,18,20,22,24,28,30};
    unsigned length=0,count=0,checksum=0,i,j; unsigned char *p;
    if(name) { while(name[length]) ++length; CHECK(length && length<=255); count=(length+12)/13; }
    if(start+count+1>=f->disk.bytes*f->disk.spc/32) check_dir_extend(f);
    for(i=0;i<11;i++) checksum=(((checksum&1)?128:0)+(checksum>>1)+(unsigned char)alias[i])&255;
    for(i=0;i<count;i++) {
        unsigned ordinal=count-i;
        p=check_dir_slot(f,start+i); memset(p,0,32);
        p[0]=(unsigned char)(ordinal|(i?0:0x40)); p[11]=0x0f; p[13]=(unsigned char)checksum;
        for(j=0;j<13;j++) {
            unsigned at=(ordinal-1)*13+j;
            wr16(p+offsets[j],at<length?name[at]:at==length?0:0xffff);
        }
    }
    raw_entry(check_dir_slot(f,start+count),alias,0,0);
    return start+count+1;
}
static int name_check_call(Fixture *f,FatHandle *directory,const FatNameCheck *request,FatCheck *out) {
    struct { uint64_t before; FatCheck c; uint64_t after; } guarded;
    FatNameCheck unchanged=*request; int status;
    memset(&guarded,0xa5,sizeof(guarded));
    f->id.dir_lba=f->id.fat_lba=UINT64_MAX;
    status=ABI(fat_check_names,directory,request,&guarded.c,0);
    CHECK(guarded.before==UINT64_C(0xa5a5a5a5a5a5a5a5) && guarded.after==guarded.before);
    CHECK(guarded.c.status==(unsigned)status && !memcmp(&unchanged,request,sizeof(unchanged)));
    *out=guarded.c; return status;
}
static void test_check_names(void) {
    const unsigned sizes[][2]={{512,1},{512,128},{4096,16}};
    const struct { const uint16_t *first,*second; const char *a,*b; } cases[]={
        {NULL,NULL,"TARGET  TXT","TARGET  TXT"},
        {U("Same long name"),U("same LONG name"),"FIRST~1 TXT","SECOND~1TXT"},
        {U("TARGET.TXT"),NULL,"FIRST~1 TXT","TARGET  TXT"},
        {U("Different name"),NULL,"TARGET  TXT","TARGET  TXT"},
        {NULL,U("Different name"),"TARGET  TXT","TARGET  TXT"},
        {U("First name"),U("Second name"),"TARGET  TXT","TARGET  TXT"},
        {NULL,U("target.txt"),"TARGET  TXT","SECOND~1TXT"},
        {U("First name"),U("target.txt"),"TARGET  TXT","SECOND~1TXT"},
        {U("target.txt"),U("Second name"),"FIRST~1 TXT","TARGET  TXT"},
    };
    report("bounded namespace diagnosis / LFN-SFN collision matrix / lookup equivalence / OEM mapping");
    for(unsigned geometry=0;geometry<3;geometry++) {
        Fixture *f=fixture(sizes[geometry][0],sizes[geometry][1]);
        FatVolume v={0}; FatObject pool[3],saved[3]; FatHandle root={0}; FatCheck c;
        struct { uint64_t before; FatEntry entries[5]; uint64_t after; } memory;
        FatNameCheck request={memory.entries,5,64,10,0};
        unsigned i,at,first,second; uint16_t oem[256],long_a[256],long_b[256];
        memset(&memory,0xa5,sizeof(memory));
        OK(fat_volume_init(&v,&f->id,pool,3)); OK(fat_root(&v,3,&root));
        memcpy(saved,pool,sizeof(saved));
        for(i=0;i<sizeof(cases)/sizeof(cases[0]);i++) {
            check_dir_clear(f);
            first=name_slots(f,0,cases[i].first,cases[i].a);
            second=name_slots(f,first,cases[i].second,cases[i].b);
            if(i==0) check_dir_slot(f,first)[12]=0x18; /* NT case flags do not distinguish keys. */
            CHECK(name_check_call(f,&root,&request,&c)==F_CORRUPT && c.scope==FC_NAMES && c.issue==FC_DUPLICATE);
            CHECK(c.observed==second-1 && c.expected==first-1 && c.examined==1 && c.count==1);
            CHECK(c.sector==f->disk.data+(second-1)*32/f->disk.bytes && c.cluster==2);
            CHECK(!memcmp(saved,pool,sizeof(saved)) && fat_check_fresh(&root,&c)==F_STALE);
        }
        check_dir_clear(f);
        at=name_slots(f,0,U("target.txt"),"TARGET  TXT"); /* own LFN and alias may be equal */
        at=name_slots(f,at,U("\u03a9"),"UPPER~1 TXT");
        at=name_slots(f,at,U("\u03c9"),"LOWER~1 TXT"); /* non-ASCII code units compare exactly */
        at=name_slots(f,at,U("A longer name"),"LONGER~1TXT");
        at=name_slots(f,at,NULL,"OTHER   TXT");
        OK(name_check_call(f,&root,&request,&c));
        CHECK(c.scope==FC_NAMES && c.count==5 && c.examined==10 && c.budget==10 && c.flags==FC_ENDMARKER);
        CHECK(c.sector==UINT64_MAX && c.first==2 && c.last==2);
        CHECK(memory.entries[0].name[0]=='t' && memory.entries[4].index==at-1);
        if(geometry) { /* Two 255-unit names need 42 slots in this two-cluster fixture. */
            check_dir_clear(f);
            for(i=0;i<255;i++) long_a[i]=long_b[i]='a';
            long_a[255]=long_b[255]=0; long_b[254]='b';
            at=name_slots(f,0,long_a,"FIRST~1 TXT");
            name_slots(f,at,long_b,"SECOND~1TXT");
            OK(name_check_call(f,&root,&request,&c)); CHECK(c.count==2 && c.examined==1);
            long_b[254]='A'; name_slots(f,at,long_b,"SECOND~1TXT");
            CHECK(name_check_call(f,&root,&request,&c)==F_CORRUPT && c.issue==FC_DUPLICATE);
        }
        check_dir_clear(f);
        name_slots(f,0,NULL,"\x80" "       TXT"); name_slots(f,1,NULL,"\x81" "       TXT");
        for(i=0;i<256;i++) oem[i]=(uint16_t)i;
        oem[0x80]=oem[0x81]=0x1234; f->id.oem=oem;
        CHECK(name_check_call(f,&root,&request,&c)==F_CORRUPT && c.issue==FC_DUPLICATE);
        oem[0x81]=0x1235; OK(name_check_call(f,&root,&request,&c));
        f->id.oem=NULL;
        CHECK(memory.before==UINT64_C(0xa5a5a5a5a5a5a5a5) && memory.after==memory.before);
        CHECK(!memcmp(saved,pool,sizeof(saved)) && !f->disk.writes && !f->buffer.pages);
        OK(fat_close(&root)); OK(fat_volume_close(&v)); destroy(f);
    }
}
static void test_check_names_limits(void) {
    Fixture *f=fixture(512,1); FatVolume v={0}; FatObject pool[3];
    FatHandle root={0},writeonly={0},file={0}; FatCheck c; FatEntry memory[4],saved[4];
    FatNameCheck request={memory,4,40,6,0}; unsigned i,at,first_errors=0,second_errors=0; uint64_t before,reads;
    report("namespace work budgets / exact workspace / fragmented directory / every read failure / no mutation");
    OK(fat_volume_init(&v,&f->id,pool,3)); OK(fat_root(&v,3,&root));
    request.entries=NULL; request.capacity=0; request.pair_budget=0;
    OK(name_check_call(f,&root,&request,&c)); CHECK(c.count==0 && c.examined==0);
    name_slots(f,0,NULL,"ONE     TXT");
    CHECK(name_check_call(f,&root,&request,&c)==F_MEMORY && c.scope==FC_NAMES && c.issue==FC_WORKSPACE);
    CHECK(c.observed==0 && c.expected==1);
    request.entries=memory; request.capacity=1;
    OK(name_check_call(f,&root,&request,&c)); CHECK(c.count==1 && c.examined==0);
    name_slots(f,1,NULL,"TWO     TXT");
    CHECK(name_check_call(f,&root,&request,&c)==F_MEMORY && c.count==1 && c.expected==2);
    request.capacity=2;
    CHECK(name_check_call(f,&root,&request,&c)==F_LIMIT && c.scope==FC_NAMES && c.issue==FC_BUDGET && c.examined==0);
    request.pair_budget=1;
    OK(name_check_call(f,&root,&request,&c)); CHECK(c.count==2 && c.examined==1);
    request.slot_budget=2; memset(memory,0xa5,sizeof(memory)); memcpy(saved,memory,sizeof(memory));
    CHECK(name_check_call(f,&root,&request,&c)==F_LIMIT && c.scope==FC_DIRECTORY && c.examined==2);
    CHECK(!memcmp(saved,memory,sizeof(memory)));
    request.slot_budget=0; before=f->provider_reads;
    CHECK(name_check_call(f,&root,&request,&c)==F_LIMIT && f->provider_reads==before);
    request.slot_budget=40; request.reserved=1;
    CHECK(name_check_call(f,&root,&request,&c)==F_ARGUMENT && c.scope==FC_NAMES && f->provider_reads==before);
    request.reserved=0; request.entries=NULL;
    CHECK(name_check_call(f,&root,&request,&c)==F_ARGUMENT && f->provider_reads==before);
    request.entries=(FatEntry *)(uintptr_t)(UINT64_MAX-32);
    CHECK(name_check_call(f,&root,&request,&c)==F_RANGE && f->provider_reads==before);
    request.entries=memory; request.capacity=4; request.pair_budget=6;
    check_dir_clear(f);
    for(i=0;i<15;i++) check_dir_slot(f,i)[0]=0xe5;
    at=name_slots(f,15,U("Across boundary"),"ACROSS~1TXT");
    at=name_slots(f,at,NULL,"TWO     TXT");
    name_slots(f,at,NULL,"THREE   TXT");
    before=f->provider_reads;
    OK(name_check_call(f,&root,&request,&c)); reads=f->provider_reads-before;
    CHECK(reads>2 && c.count==3 && c.examined==3 && c.last==7);
    request.pair_budget=2;
    CHECK(name_check_call(f,&root,&request,&c)==F_LIMIT && c.scope==FC_NAMES && c.examined==2 && c.count==2);
    request.pair_budget=3;
    for(i=0;i<reads;i++) {
        f->disk.fail_read=(int)i;
        CHECK(name_check_call(f,&root,&request,&c)==F_IO && c.issue==FC_IO);
        CHECK(c.scope==FC_DIRECTORY || c.scope==FC_NAMES);
        first_errors+=c.scope==FC_DIRECTORY; second_errors+=c.scope==FC_NAMES;
        CHECK(c.sector==UINT64_MAX);
    }
    CHECK(first_errors && second_errors);
    f->disk.fail_read=-1;
    check_dir_slot(f,15)[13]^=1;
    CHECK(name_check_call(f,&root,&request,&c)==F_CORRUPT && c.scope==FC_DIRECTORY && c.issue==FC_LFN);
    check_dir_slot(f,15)[13]^=1;
    check_dir_slot(f,15)[12]=1;
    CHECK(name_check_call(f,&root,&request,&c)==F_ATTENTION && c.scope==FC_DIRECTORY && c.issue==FC_EXTENSION);
    check_dir_slot(f,15)[12]=0;
    f->id.transaction=1; before=f->provider_reads;
    CHECK(name_check_call(f,&root,&request,&c)==F_BUSY && f->provider_reads==before);
    f->id.transaction=0; OK(fat_root(&v,FH_WRITE,&writeonly));
    CHECK(name_check_call(f,&writeonly,&request,&c)==F_ARGUMENT && f->provider_reads==before);
    OK(fat_open(&root,U("TWO.TXT"),FH_READ,&file)); before=f->provider_reads;
    CHECK(name_check_call(f,&file,&request,&c)==F_ARGUMENT && f->provider_reads==before);
    OK(fat_close(&file)); OK(fat_close(&writeonly)); OK(fat_close(&root));
    CHECK(name_check_call(f,&root,&request,&c)==F_STALE && f->provider_reads==before);
    CHECK(!f->disk.writes && !f->buffer.pages); OK(fat_volume_close(&v)); destroy(f);
}
