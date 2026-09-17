typedef struct PolicyTest {
    Fixture *f; FatVolume volume; FatObject objects[6];
    FatHandle root,bad,good,third; FatPolicy policy; FatCheck ledger[3];
} PolicyTest;
static PolicyTest *policy_fixture(unsigned bps,unsigned spc,int damaged,unsigned capacity) {
    PolicyTest *p=calloc(1,sizeof(*p)); CHECK(p); p->f=fixture(bps,spc);
    unsigned cb=p->f->id.cluster_bytes; unsigned char *root=owner_data(p->f,2);
    raw_entry(root,"BAD     BIN",5,2*cb+17); raw_entry(root+32,"GOOD    BIN",9,7);
    raw_entry(root+64,"THIRD   BIN",10,7);
    for(unsigned copy=0;copy<2;++copy) {
        fat_value(&p->f->disk,copy,5,6); fat_value(&p->f->disk,copy,6,damaged?0:7);
        fat_value(&p->f->disk,copy,7,0x0fffffff);
        fat_value(&p->f->disk,copy,9,0x0fffffff); fat_value(&p->f->disk,copy,10,0);
    }
    for(unsigned c=5;c<=10;++c) salvage_data(p->f,c);
    OK(fat_volume_init(&p->volume,&p->f->id,p->objects,6));
    OK(fat_root(&p->volume,FH_READ|FH_WRITE,&p->root));
    OK(fat_open(&p->root,U("BAD.BIN"),FH_READ|FH_WRITE,&p->bad));
    OK(fat_open(&p->root,U("GOOD.BIN"),FH_READ|FH_WRITE,&p->good));
    OK(fat_open(&p->root,U("THIRD.BIN"),FH_READ|FH_WRITE,&p->third));
    /* Retaining the incoming count must still honor its uint32_t contract. */
    OK(ABI(fat_policy_init,&p->policy,&p->volume,p->ledger,0xFFFFFFFF00000000ull|capacity));
    return p;
}
static void policy_destroy(PolicyTest *p) {
    OK(fat_policy_close(&p->policy));
    OK(fat_close(&p->third)); OK(fat_close(&p->good)); OK(fat_close(&p->bad)); OK(fat_close(&p->root));
    OK(fat_volume_close(&p->volume)); destroy(p->f); free(p);
}
static FatPolicyCall policy_read(FatHandle *file,FatTransfer *transfer) {
    FatPolicyCall request={0};
    request.action=(FatCall){(uintptr_t)fat_read_at,{(uintptr_t)file,(uintptr_t)transfer,0,0}};
    request.subject=file; request.kind=FP_READ; return request;
}
typedef struct PolicyProbe { PolicyTest *p; unsigned calls,diagnoses; int status,reset_failure,reenter; FatTransfer *transfer; FatCheck *report; } PolicyProbe;
static int policy_action_probe(PolicyProbe *probe) {
    ++probe->calls;
    int status=probe->transfer?fat_read_at(&probe->p->bad,probe->transfer):probe->status;
    if(probe->reset_failure) probe->p->f->disk.fail_read=-1;
    if(probe->reenter) {
        FatPolicyCall nested={0}; nested.action.target=(uintptr_t)policy_action_probe;
        nested.kind=FP_INSPECT; FatCheck check={0};
        CHECK(fat_policy_call(&probe->p->policy,&nested)==F_BUSY && !nested.invoked);
        CHECK(fat_policy_note(&probe->p->policy,&check)==F_BUSY);
        CHECK(fat_policy_close(&probe->p->policy)==F_BUSY);
    }
    return status;
}
static int policy_diagnose_probe(PolicyProbe *probe) {
    ++probe->diagnoses; return fat_check_file(&probe->p->bad,8,probe->report);
}
static void test_policy_basic(void) {
    report("optional policy / ordinary reads and accepted writes have no diagnostic IO / operation results / no implicit retry");
    PolicyTest *p=policy_fixture(512,1,0,2); unsigned char bytes[1200]; FatTransfer transfer={bytes,0,7,0};
    FatPolicyCall call=policy_read(&p->good,&transfer); uint64_t reads=p->f->provider_reads;
    OK(ABI(fat_policy_call,&p->policy,&call,0,0));
    CHECK(call.invoked && !call.diagnosed && transfer.done==7 && p->f->provider_reads==reads+1);
    CHECK(!p->policy.flags && !p->policy.count && !p->policy.cause.scope);
    memset(bytes,0x63,23); transfer=(FatTransfer){bytes,0,23,0};
    call.action.target=(uintptr_t)fat_write_at; call.kind=FP_WRITE;
    OK(fat_policy_call(&p->policy,&call)); CHECK(call.invoked && !call.diagnosed && transfer.done==23);
    CHECK(!p->policy.flags && !p->policy.count && p->f->buffer.pages && !p->f->disk.writes);
    memset(bytes,0,23); transfer=(FatTransfer){bytes,0,23,0}; call.action.target=(uintptr_t)fat_read_at; call.kind=FP_READ;
    OK(fat_policy_call(&p->policy,&call)); for(unsigned i=0;i<23;++i) CHECK(bytes[i]==0x63);
    FatCheck check={0}; PolicyProbe probe={p,0,0,F_OK,0,1,NULL,&check};
    FatCall diagnosis={(uintptr_t)policy_diagnose_probe,{(uintptr_t)&probe,0,0,0}};
    call.action=(FatCall){(uintptr_t)policy_action_probe,{(uintptr_t)&probe,0,0,0}};
    call.subject=&p->bad; call.diagnosis=&diagnosis; call.report=&check;
    const int statuses[]={F_OK,F_END,F_NOTFOUND,F_BUSY,F_MEMORY,F_NOSPACE,F_ARGUMENT,F_STALE,F_RANGE};
    for(unsigned i=0;i<sizeof(statuses)/sizeof(*statuses);++i) {
        probe.status=statuses[i]; CHECK(fat_policy_call(&p->policy,&call)==statuses[i]);
        CHECK(call.invoked && !call.diagnosed && !p->policy.flags && !p->policy.active);
    }
    CHECK(probe.calls==sizeof(statuses)/sizeof(*statuses) && !probe.diagnoses);
    policy_destroy(p);
}
static void test_policy_damage(void) {
    report("policy file diagnosis / original prefix / same-file consumers / diagnostic pins survive close-reopen / mutation refusal");
    const unsigned geometry[][2]={{512,1},{512,128},{4096,64}};
    for(unsigned g=0;g<sizeof(geometry)/sizeof(*geometry);++g) {
        PolicyTest *p=policy_fixture(geometry[g][0],geometry[g][1],1,2);
        unsigned cb=p->f->id.cluster_bytes,length=2*cb+17; unsigned char *bytes=malloc(length); CHECK(bytes); memset(bytes,0xa5,length);
        FatTransfer transfer={bytes,0,length,0}; FatPolicyCall call=policy_read(&p->bad,&transfer);
        FatCheck report; FatCall diagnosis={(uintptr_t)fat_check_file,{(uintptr_t)&p->bad,8,(uintptr_t)&report,0}};
        call.diagnosis=&diagnosis; call.report=&report; unsigned refs=p->bad.object->references;
        CHECK(ABI(fat_policy_call,&p->policy,&call,0,0)==F_CORRUPT);
        CHECK(call.invoked && call.diagnosed && transfer.done==2*cb && report.scope==FC_FILE && report.issue==FC_LINK);
        const unsigned chain[]={5,6}; salvage_bytes(p->f,chain,0,2*cb,bytes);
        for(unsigned i=2*cb;i<length;++i) CHECK(bytes[i]==0xa5);
        CHECK(p->policy.flags==FP_NO_WRITE && p->policy.count==1 && p->bad.object->references==refs+1);
        CHECK(p->policy.cause.status==F_CORRUPT && !p->policy.cause.scope && p->policy.cause_target==(uintptr_t)fat_read_at);
        FatCheck cause=p->policy.cause; uint64_t reads=p->f->provider_reads,pages=p->f->buffer.pages;
        transfer.done=77; CHECK(fat_policy_call(&p->policy,&call)==F_CORRUPT);
        CHECK(!call.invoked && !call.diagnosed && transfer.done==77 && p->f->provider_reads==reads);
        FatObject *object=p->bad.object; uint64_t incarnation=object->incarnation;
        OK(fat_close(&p->bad)); CHECK(object->live && object->references==1);
        OK(fat_open(&p->root,U("BAD.BIN"),FH_READ|FH_WRITE,&p->bad));
        CHECK(p->bad.object==object && object->incarnation==incarnation);
        CHECK(fat_policy_call(&p->policy,&call)==F_CORRUPT && !call.invoked);
        transfer=(FatTransfer){bytes,0,7,0}; call=policy_read(&p->good,&transfer);
        OK(fat_policy_call(&p->policy,&call)); CHECK(transfer.done==7);
        call.kind=FP_WRITE; call.action.target=(uintptr_t)fat_write_at; reads=p->f->provider_reads;
        CHECK(fat_policy_call(&p->policy,&call)==F_READONLY && !call.invoked && p->f->provider_reads==reads);
        CHECK(p->f->buffer.pages==pages && !p->f->disk.writes);
        OK(fat_check_file(&p->good,8,&report)); OK(fat_policy_note(&p->policy,&report));
        CHECK(p->policy.flags==FP_NO_WRITE && !memcmp(&cause,&p->policy.cause,sizeof(cause)));
        /* Inspection remains available but cannot silently clear restrictions. */
        call=(FatPolicyCall){0}; call.kind=FP_INSPECT;
        call.action=(FatCall){(uintptr_t)fat_check_file,{(uintptr_t)&p->bad,8,(uintptr_t)&report,0}};
        CHECK(fat_policy_call(&p->policy,&call)==F_CORRUPT && call.invoked && !call.diagnosed);
        OK(fat_policy_note(&p->policy,&report)); CHECK(p->policy.count==1 && object->references==2);
        free(bytes); policy_destroy(p);
    }
}
static void test_policy_io(void) {
    report("policy transient read failure / one diagnosis / original status and prefix retained / unknown health / metadata latch");
    PolicyTest *p=policy_fixture(512,1,0,2); unsigned char bytes[1100]; memset(bytes,0xa5,sizeof(bytes));
    FatTransfer transfer={bytes,0,sizeof(bytes),0}; FatCheck report;
    PolicyProbe probe={p,0,0,F_OK,1,0,&transfer,&report};
    FatCall diagnosis={(uintptr_t)policy_diagnose_probe,{(uintptr_t)&probe,0,0,0}};
    FatPolicyCall call=policy_read(&p->bad,&transfer);
    call.action=(FatCall){(uintptr_t)policy_action_probe,{(uintptr_t)&probe,0,0,0}};
    call.diagnosis=&diagnosis; call.report=&report;
    p->f->disk.fail_read=1; CHECK(fat_policy_call(&p->policy,&call)==F_IO);
    CHECK(transfer.done==512 && probe.calls==1 && probe.diagnoses==1 && call.diagnosed);
    const unsigned chain[]={5}; salvage_bytes(p->f,chain,0,512,bytes);
    for(unsigned i=512;i<sizeof(bytes);++i) CHECK(bytes[i]==0xa5);
    CHECK(report.status==F_OK && !p->policy.flags && !p->policy.count && !p->policy.active);
    /* A status/metadata report is consumed under this same maintenance lease. */
    fat_value(&p->f->disk,0,1,0x07ffffff); /* injected preexisting dirty observation */
    CHECK(fat_check_reserved(&p->f->id,&report)==F_ATTENTION);
    OK(ABI(fat_policy_note,&p->policy,&report,0,0)); CHECK(p->policy.flags==FP_NO_WRITE && p->policy.cause.scope==FC_RESERVED);
    FatCheck first=p->policy.cause; call=policy_read(&p->good,&transfer); transfer.length=7;
    OK(fat_policy_call(&p->policy,&call));
    FatFatRange range={0,2}; CHECK(fat_check_mirrors(&p->f->id,&range,&report)==F_CORRUPT);
    OK(fat_policy_note(&p->policy,&report)); CHECK(p->policy.flags==(FP_NO_READ|FP_NO_WRITE));
    CHECK(!memcmp(&first,&p->policy.cause,sizeof(first)));
    CHECK(fat_policy_call(&p->policy,&call)==F_CORRUPT && !call.invoked);
    call.kind=FP_INSPECT; OK(fat_policy_call(&p->policy,&call)); CHECK(call.invoked);
    policy_destroy(p);
}
static void test_policy_limits(void) {
    report("policy exact ledger capacity / reference limits / stale report and owner / admission / mount retirement");
    PolicyTest *p=policy_fixture(512,1,1,1); FatCheck report; FatPolicy saved=p->policy;
    CHECK(fat_policy_init(&p->policy,&p->volume,p->ledger,1)==F_BUSY && !memcmp(&saved,&p->policy,sizeof(saved)));
    FatPolicy empty={0}; CHECK(fat_policy_init(&empty,&p->volume,NULL,1)==F_ARGUMENT);
    CHECK(fat_policy_init(&empty,&p->volume,(FatCheck *)(uintptr_t)(UINT64_MAX-7),2)==F_ARGUMENT);
    CHECK(!empty.volume);
    CHECK(fat_check_file(&p->bad,8,&report)==F_CORRUPT); FatCheck stale=report; ++stale.generation;
    CHECK(fat_policy_note(&p->policy,&stale)==F_STALE && !p->policy.flags);
    stale=report; stale.object=(FatObject *)(uintptr_t)1;
    CHECK(fat_policy_note(&p->policy,&stale)==F_STALE && !p->policy.flags);
    OK(fat_policy_note(&p->policy,&report)); CHECK(p->policy.count==1);
    CHECK(fat_check_file(&p->third,8,&report)==F_CORRUPT);
    unsigned references=p->third.object->references;
    CHECK(fat_policy_note(&p->policy,&report)==F_MEMORY && p->third.object->references==references);
    CHECK(p->policy.count==1 && p->policy.flags==(FP_NO_READ|FP_NO_WRITE));
    unsigned char bytes[7]; FatTransfer transfer={bytes,0,7,0}; FatPolicyCall call=policy_read(&p->good,&transfer);
    uint64_t reads=p->f->provider_reads;
    CHECK(fat_policy_call(&p->policy,&call)==F_CORRUPT && !call.invoked && p->f->provider_reads==reads);
    call.kind=FP_INSPECT; call.reserved=1; CHECK(fat_policy_call(&p->policy,&call)==F_ARGUMENT && !call.invoked);
    call.reserved=0; call.kind=99; CHECK(fat_policy_call(&p->policy,&call)==F_ARGUMENT && !call.invoked);
    call.kind=FP_INSPECT; call.action.target=0; CHECK(fat_policy_call(&p->policy,&call)==F_ARGUMENT && !call.invoked);
    policy_destroy(p);
    p=policy_fixture(512,1,1,1); CHECK(fat_check_file(&p->bad,8,&report)==F_CORRUPT);
    references=p->bad.object->references; p->bad.object->references=UINT32_MAX;
    CHECK(fat_policy_note(&p->policy,&report)==F_LIMIT && !p->policy.count && p->policy.flags==(FP_NO_READ|FP_NO_WRITE));
    p->bad.object->references=references; policy_destroy(p);
    p=policy_fixture(512,1,1,0); CHECK(fat_check_file(&p->bad,8,&report)==F_CORRUPT);
    CHECK(fat_policy_note(&p->policy,&report)==F_MEMORY && p->policy.flags==(FP_NO_READ|FP_NO_WRITE));
    /* Mount retirement follows the same rule used by uncertain ordered commit:
       stale policy invokes neither an action nor its diagnostic callback. */
    OK(fat_invalidate(&p->f->id)); call=policy_read(&p->good,&transfer);
    CHECK(fat_policy_call(&p->policy,&call)==F_STALE && !call.invoked && !call.diagnosed);
    CHECK(fat_policy_note(&p->policy,&report)==F_STALE); OK(fat_policy_close(&p->policy));
    CHECK(fat_policy_close(&p->policy)==F_STALE); destroy(p->f); free(p);
}
static int policy_count_diagnosis(unsigned *count) { ++*count; return F_IO; }
static void test_policy_fallback(void) {
    report("policy bounded or failed diagnosis / no stale evidence reuse / unknown corruption fails closed / call admission");
    for(unsigned variant=0;variant<5;++variant) {
        PolicyTest *p=policy_fixture(512,1,1,2); unsigned char bytes[1100];
        memset(bytes,0xa5,sizeof(bytes)); FatTransfer transfer={bytes,0,sizeof(bytes),0};
        FatPolicyCall call=policy_read(&p->bad,&transfer); FatCheck check;
        CHECK(fat_check_file(&p->bad,8,&check)==F_CORRUPT); /* old output, not noted */
        unsigned diagnoses=0;
        FatCall diagnosis={(uintptr_t)fat_check_file,{(uintptr_t)&p->bad,0,(uintptr_t)&check,0}};
        if(variant==1) diagnosis=(FatCall){(uintptr_t)policy_count_diagnosis,{(uintptr_t)&diagnoses,0,0,0}};
        if(variant==2) diagnosis.args[0]=(uintptr_t)&p->good;
        if(variant==3) diagnosis.args[1]=8;
        if(variant!=4) { call.diagnosis=&diagnosis; call.report=&check; }
        if(variant==3) call.kind=FP_WRITE; /* a corrupt mutation cannot be restricted to read scope */
        CHECK(fat_policy_call(&p->policy,&call)==F_CORRUPT && call.invoked);
        CHECK(transfer.done==1024 && p->policy.flags==(FP_NO_READ|FP_NO_WRITE));
        CHECK(call.diagnosed==(unsigned)(variant!=4));
        if(variant==0 || variant==2) CHECK(check.status==F_LIMIT && !p->policy.count);
        if(variant==1) CHECK(diagnoses==1 && !check.scope && !p->policy.count);
        if(variant==3) CHECK(check.status==F_CORRUPT && p->policy.count==1);
        policy_destroy(p);
    }
    PolicyTest *p=policy_fixture(512,1,0,2),*other=policy_fixture(512,1,0,2);
    unsigned char bytes[7]; FatTransfer transfer={bytes,0,7,0}; FatPolicyCall call=policy_read(&other->good,&transfer);
    CHECK(fat_policy_call(&p->policy,&call)==F_ARGUMENT && !call.invoked);
    FatCheck check; OK(fat_check_file(&other->good,8,&check));
    CHECK(fat_policy_note(&p->policy,&check)==F_STALE && !p->policy.flags);
    call=policy_read(&p->good,&transfer); call.report=&check;
    CHECK(fat_policy_call(&p->policy,&call)==F_ARGUMENT && !call.invoked);
    FatCall diagnosis={0}; call.diagnosis=&diagnosis;
    CHECK(fat_policy_call(&p->policy,&call)==F_ARGUMENT && !call.invoked);
    unsigned count=0; diagnosis=(FatCall){(uintptr_t)policy_count_diagnosis,{(uintptr_t)&count,0,0,0}};
    call.kind=FP_INSPECT; CHECK(fat_policy_call(&p->policy,&call)==F_ARGUMENT && !count);
    policy_destroy(other); policy_destroy(p);
}
static void test_policy_commit(void) {
    report("policy actual ordered commit failure / original uncertainty evidence / no diagnostic or operation retry / session retirement");
    for(unsigned strict=0;strict<2;++strict) for(unsigned kind=0;kind<2;++kind) {
        CommitFixture *c=commit_fixture(512,1,64,64); FatPolicy policy={0}; FatCheck ledger[1],check;
        OK(fat_policy_init(&policy,&c->volume,ledger,1));
        unsigned char bytes[1400]={0x39}; FatTransfer transfer={bytes,0,sizeof(bytes),0};
        FatPolicyCall call=policy_read(&c->opened,&transfer); call.kind=FP_WRITE;
        call.action.target=(uintptr_t)fat_write_at; OK(fat_policy_call(&policy,&call));
        CHECK(call.invoked && transfer.done==sizeof(bytes) && c->order.accepted && !c->writes);
        unsigned diagnoses=0; memset(&check,0xa5,sizeof(check)); FatCheck before_check=check;
        FatCall diagnosis={(uintptr_t)policy_count_diagnosis,{(uintptr_t)&diagnoses,0,0,0}};
        call=(FatPolicyCall){0}; call.kind=FP_WRITE; call.diagnosis=&diagnosis; call.report=&check;
        call.action=(FatCall){(uintptr_t)commit_run,{(uintptr_t)c,strict,0,0}};
        uint64_t generation=c->base->id.generation;
        if(kind) c->fail_flush=0; else c->fail_write=0;
        CHECK(fat_policy_call(&policy,&call)==F_IO && call.invoked && !call.diagnosed);
        CHECK(!diagnoses && !memcmp(&check,&before_check,sizeof(check)));
        CHECK(c->report.effect==FE_UNCERTAIN && c->order.poisoned && c->base->id.generation==generation+1);
        FatCommitReport evidence=c->report; uint64_t reads=c->reads,writes=c->writes,flushes=c->flushes;
        CHECK(fat_policy_call(&policy,&call)==F_STALE && !call.invoked && !call.diagnosed);
        call=policy_read(&c->opened,&transfer);
        CHECK(fat_policy_call(&policy,&call)==F_STALE && !call.invoked);
        CHECK(!memcmp(&evidence,&c->report,sizeof(evidence)) && c->reads==reads && c->writes==writes && c->flushes==flushes);
        OK(fat_policy_close(&policy)); commit_destroy(c);
    }
}
