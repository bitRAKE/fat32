/* Read-only image observations for tests/images.py; no device access or writes.
   "legacy" expectations deliberately record the current snapshot API. Future
   basic/checked interfaces must add their own explicit expectation profiles. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "../../fat32.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); exit(1); } } while (0)
typedef struct Image { FILE *file; uint64_t sectors, reads,ranges; uint32_t bps; int deny_primary; } Image;
static FatIdentity identity;
static unsigned char workspace_data[3*4096];
static FatWorkspace workspace={workspace_data,sizeof(workspace_data),0};
static int image_read(void *context, uint64_t lba, void *out) {
    Image *image=context;
    ++image->reads;
    if(image->deny_primary && !lba) return F_IO;
    if(lba>=image->sectors) return F_RANGE;
    if(_fseeki64(image->file,(__int64)(lba*image->bps),SEEK_SET)) return F_IO;
    return fread(out,1,image->bps,image->file)==image->bps ? F_OK:F_IO;
}
static int image_range(void *context,FatRangeRequest *request) {
    Image *image=context; size_t bytes,got;
    ++image->ranges; image->reads+=request->count; request->done=0;
    if(!request->count || request->count>8 || request->lba>=image->sectors || request->count>image->sectors-request->lba) return F_RANGE;
    if(_fseeki64(image->file,(__int64)(request->lba*image->bps),SEEK_SET)) return F_IO;
    bytes=(size_t)request->count*image->bps; got=fread(request->data,1,bytes,image->file);
    request->done=(uint32_t)(got/image->bps); return got==bytes?F_OK:F_IO;
}
typedef struct Expected { const wchar_t *name; uint32_t seed,size; } Expected;
static void check_views(const FatIdentity *source) {
    static const wchar_t *paths[]={L"KEEP.TXT",L"ALPHA.BIN",L"BETA.BIN",L"EMPTY.BIN",L"Boundary \u03A9.bin"};
    FatIdentity original=*source; FatView view={0}; unsigned char scratch[3*4096];
    FatWorkspace space={scratch,sizeof(scratch),0};
    printf("{\"copies\":[");
    for(unsigned copy=0;copy<2;++copy) {
        FatVolume volume={0}; FatObject objects[4]; FatHandle root={0},work={0},file={0};
        CHECK(fat_view_open(&view,source,copy,&space)==F_OK);
        CHECK(!view.provider.write && !view.provider.begin && !view.provider.end && !view.provider.flush);
        CHECK(fat_volume_init(&volume,&view.identity,objects,4)==F_OK);
        CHECK(fat_root(&volume,FH_READ,&root)==F_OK);
        CHECK(fat_open(&root,(const uint16_t*)L"WORK",FH_READ,&work)==F_OK);
        printf("%s{\"fat_copy\":%u,\"files\":[",copy?",":"",copy);
        for(unsigned i=0;i<5;++i) {
            int status=fat_open(i?&work:&root,(const uint16_t*)paths[i],FH_READ,&file);
            if(i==4 && status==F_NOTFOUND) status=fat_open(&work,(const uint16_t*)L"BOUND~1.BIN",FH_READ,&file);
            CHECK(status==F_OK); FatRecord record; FatCheck check;
            CHECK(fat_handle_info(&file,&record)==F_OK);
            status=fat_check_file(&file,32,&check);
            printf("%s{\"first\":%u,\"size\":%u,\"status\":%d,\"issue\":%u,\"count\":%u,\"flags\":%u",
                   i?",":"",record.cluster,record.size,status,check.issue,check.count,check.flags);
            FatExtent extents[32]; FatSalvage plan;
            FatSalvageRequest request={extents,32,record.cluster,record.size,32,1024,0};
            int planned=fat_salvage_plan(&view.identity,&request,&plan);
            unsigned char *bytes=malloc((size_t)record.size+1); CHECK(bytes);
            memset(bytes,0xa5,(size_t)record.size+1);
            FatTransfer transfer={bytes,0,record.size+1,0};
            int copied=fat_salvage_read(&plan,&transfer); uint64_t hash=UINT64_C(14695981039346656037);
            for(unsigned b=0;b<transfer.done;++b) hash=(hash^bytes[b])*UINT64_C(1099511628211);
            for(unsigned b=transfer.done;b<=record.size;++b) CHECK(bytes[b]==0xa5);
            CHECK(copied==F_END && transfer.done==plan.available);
            printf(",\"salvage_status\":%d,\"salvage_issue\":%u,\"available\":%u,\"done\":%u,\"fnv64\":\"%016llx\",\"extents\":[",
                   planned,plan.check.issue,plan.available,transfer.done,(unsigned long long)hash);
            for(unsigned e=0;e<plan.extent_count;++e) printf("%s[%u,%u]",e?",":"",extents[e].first,extents[e].count);
            printf("]}"); free(bytes);
            CHECK(fat_close(&file)==F_OK);
        }
        CHECK(fat_close(&work)==F_OK && fat_close(&root)==F_OK && fat_volume_close(&volume)==F_OK);
        CHECK(fat_view_close(&view)==F_OK && !memcmp(source,&original,sizeof(original)));
        printf("]}");
    }
    printf("]}\n");
}
static void check_boot_view(SectorOps *ops) {
    int primary=fat_mount(&identity,ops,NULL,&workspace);
    FatView view={0},saved=view; FatBootSource boot={6,0,NULL};
    int candidate=fat_view_boot(&view,ops,&boot,&workspace);
    printf("{\"primary_status\":%d,\"candidate_status\":%d",primary,candidate);
    if(!candidate) {
        FatCheck check; int diagnosed=fat_check_backup(&view.identity,&check);
        printf(",\"backup_status\":%d,\"backup_issue\":%u,\"backup_sector\":%llu,\"serial\":%u,\"cluster_bytes\":%u,\"observations\":",
               diagnosed,check.issue,(unsigned long long)check.sector,view.identity.serial,view.identity.cluster_bytes);
        check_views(&view.identity); CHECK(fat_view_close(&view)==F_OK);
    } else CHECK(!memcmp(&view,&saved,sizeof(view)));
    printf("}\n");
}
int wmain(int argc,wchar_t **argv) {
    Image image={0}; SectorOps ops={0}; FatEntry directory={0},entry={0};
    unsigned bps,cluster_bytes,i; const wchar_t *fault; __int64 size;
    Expected files[5]; int shared=argc==6; int checked=shared && !wcscmp(argv[5],L"checked");
    int ranged=shared && !wcscmp(argv[5],L"range");
    int streaming=ranged || (shared && !wcscmp(argv[5],L"stream"));
    int views=shared && !wcscmp(argv[5],L"views");
    int boot=shared && (!wcscmp(argv[5],L"boot") || !wcscmp(argv[5],L"boot-io"));
    FatVolume volume={0}; FatObject objects[4]; FatRecord info;
    FatHandle root={0},work={0},opened={0};
    FatCheck diagnostic={0},file_check={0}; FatCheckedRead policy={&file_check,32,0};
    FatFatRange range; int reserved_status=0,backup_status=0,mirror_status=0;
    int root_directory_status=0,work_directory_status=0;
    uint32_t work_directory_issue=0;
    int root_names_status=0,work_names_status=0;
    uint32_t work_names_scope=0,work_names_issue=0;
    FatEntry names[8]; FatNameCheck name_request={names,8,32,28,0};
    FatOwnershipReport ownership={0}; int ownership_status=0;
    FatRangeOps ranges={&image,image_range,8,0}; FatStream stream={0}; uint32_t map[32];
    FatStreamWorkspace map_workspace={map,32,0};
    CHECK(argc==5 || (argc==6 && (!wcscmp(argv[5],L"shared") || checked || streaming || views || boot)));
    CHECK(wcslen(argv[1])>4 && !wcscmp(argv[1]+wcslen(argv[1])-4,L".img"));
    CHECK(wcsncmp(argv[1],L"\\\\.\\",4) && wcsncmp(argv[1],L"\\\\?\\",4));
    bps=(unsigned)wcstoul(argv[2],NULL,10);
    cluster_bytes=(unsigned)wcstoul(argv[3],NULL,10); fault=argv[4];
    CHECK(bps==512 || bps==4096);
    CHECK(cluster_bytes>=bps && cluster_bytes<=65536 && !(cluster_bytes&(cluster_bytes-1)));
    CHECK(!_wfopen_s(&image.file,argv[1],L"rb"));
    CHECK(!_fseeki64(image.file,0,SEEK_END)); size=_ftelli64(image.file);
    CHECK(size>0 && size%bps==0);
    image.bps=bps; image.sectors=(uint64_t)size/bps;
    ops.context=&image; ops.read=image_read; ops.sectors=image.sectors; ops.sector_bytes=bps;
    if(boot) {
        image.deny_primary=!wcscmp(argv[5],L"boot-io");
        check_boot_view(&ops); CHECK(!fclose(image.file)); return 0;
    }
    CHECK(fat_mount(&identity,&ops,NULL,&workspace)==F_OK);
    CHECK(identity.sector_bytes==bps && identity.cluster_bytes==cluster_bytes && identity.cluster_count==65530);
    CHECK(image.reads==1); /* Basic mount does not read any FAT sector. */
    if(views) { check_views(&identity); CHECK(!fclose(image.file)); return 0; }
    if(checked) {
        reserved_status=fat_check_reserved(&identity,&diagnostic);
        CHECK(reserved_status==(!wcscmp(fault,L"dirty")?F_ATTENTION:F_OK));
        if(reserved_status) CHECK(diagnostic.issue==FC_STATUS && diagnostic.flags==FC_DIRTY);
        backup_status=fat_check_backup(&identity,&diagnostic);
        CHECK(backup_status==(!wcscmp(fault,L"backup-conflict")?F_CORRUPT:F_OK));
        if(backup_status) CHECK(diagnostic.issue==FC_BACKUP_MISMATCH && diagnostic.sector==6);
        range=(FatFatRange){0,identity.cluster_count+2};
        mirror_status=fat_check_mirrors(&identity,&range,&diagnostic);
        CHECK(mirror_status==(!wcscmp(fault,L"mirror-conflict")?F_CORRUPT:F_OK));
        if(mirror_status) CHECK(diagnostic.issue==FC_MIRROR_MISMATCH);
    }
    if(shared) {
        CHECK(fat_volume_init(&volume,&identity,objects,4)==F_OK && fat_root(&volume,FH_READ,&root)==F_OK);
        CHECK(fat_open(&root,(const uint16_t*)L"WORK",FH_READ,&work)==F_OK);
        CHECK(fat_handle_info(&work,&info)==F_OK); memcpy(&directory,&info,sizeof(info));
    } else CHECK(fat_lookup(&identity,identity.root_cluster,(const uint16_t*)L"WORK",&directory)==F_OK);
    CHECK(directory.raw[11]&16);
    if(checked) {
        int duplicate=!wcscmp(fault,L"duplicate-sfn") || !wcscmp(fault,L"duplicate-name");
        root_directory_status=fat_check_directory(&root,32,&diagnostic);
        CHECK(root_directory_status==F_OK && diagnostic.scope==FC_DIRECTORY);
        CHECK(diagnostic.count==3 && diagnostic.flags==FC_ENDMARKER);
        work_directory_status=fat_check_directory(&work,32,&diagnostic);
        work_directory_issue=diagnostic.issue;
        CHECK(work_directory_status==(!wcscmp(fault,L"bad-lfn")?F_CORRUPT:F_OK));
        CHECK(diagnostic.scope==FC_DIRECTORY);
        if(work_directory_status) CHECK(work_directory_issue==FC_LFN);
        else CHECK(diagnostic.count==(duplicate?7u:6u) && diagnostic.flags==FC_ENDMARKER);
        /* File-chain damage and shared child clusters are outside this scan's
           scope. Their fixtures must not be mislabeled as directory damage. */
        root_names_status=fat_check_names(&root,&name_request,&diagnostic);
        CHECK(root_names_status==F_OK && diagnostic.scope==FC_NAMES && diagnostic.count==2);
        work_names_status=fat_check_names(&work,&name_request,&diagnostic);
        work_names_scope=diagnostic.scope; work_names_issue=diagnostic.issue;
        CHECK(work_names_status==((duplicate || !wcscmp(fault,L"bad-lfn"))?F_CORRUPT:F_OK));
        if(duplicate) {
            CHECK(work_names_scope==FC_NAMES && work_names_issue==FC_DUPLICATE);
            CHECK(diagnostic.expected==2 && diagnostic.observed==(uint32_t)(!wcscmp(fault,L"duplicate-sfn")?8:9));
        } else if(work_names_status) CHECK(work_names_scope==FC_DIRECTORY && work_names_issue==FC_LFN);
        else CHECK(work_names_scope==FC_NAMES && diagnostic.count==6 && diagnostic.examined==15);
        {
            FatDirectoryTask directories[4];
            FatOwner *owners=malloc((size_t)identity.cluster_count*sizeof(*owners));
            FatOwnershipCheck request={owners,directories,identity.cluster_count,4,identity.cluster_count+32,64,0};
            int expected_issue=!wcscmp(fault,L"late-cycle")?FC_CYCLE:
                !wcscmp(fault,L"short-chain")?FC_SHORT:!wcscmp(fault,L"cross-link")?FC_CROSSLINK:
                !wcscmp(fault,L"orphan-chain")?FC_ORPHAN:0;
            CHECK(owners);
            ownership_status=fat_check_ownership(&identity,&request,&ownership);
            CHECK(ownership_status==(expected_issue?F_CORRUPT:F_OK));
            CHECK(ownership.check.scope==FC_OWNERSHIP && ownership.check.issue==(unsigned)expected_issue);
            if(!expected_issue || expected_issue==FC_ORPHAN) {
                CHECK(ownership.check.count==11 && ownership.directories==2 && ownership.files==(duplicate?6u:5u));
                CHECK(ownership.orphan_clusters==(!wcscmp(fault,L"orphan-chain")?2u:0u));
                CHECK(ownership.bad_clusters==(!wcscmp(fault,L"bad-cluster")?1u:0u));
                CHECK(ownership.check.count+ownership.free_clusters+ownership.bad_clusters+ownership.orphan_clusters==identity.cluster_count);
            }
            free(owners);
        }
    }
    files[0]=(Expected){L"KEEP.TXT",11,97};
    files[1]=(Expected){L"ALPHA.BIN",37,2*cluster_bytes+17};
    files[2]=(Expected){L"BETA.BIN",71,cluster_bytes+1};
    files[3]=(Expected){L"EMPTY.BIN",0,0};
    files[4]=(Expected){L"Boundary \u03A9.bin",93,2*cluster_bytes+(cluster_bytes<513?cluster_bytes:513)};
    printf("{\"profile\":\"%s\",\"sector_bytes\":%u,\"cluster_bytes\":%u,\"reserved_status\":%d,\"backup_status\":%d,\"mirror_status\":%d,\"directory_checks_run\":%s,\"root_directory_status\":%d,\"work_directory_status\":%d,\"work_directory_issue\":%u,\"root_names_status\":%d,\"work_names_status\":%d,\"work_names_scope\":%u,\"work_names_issue\":%u,\"files\":[",
           ranged?"range":streaming?"stream":checked?"checked":shared?"shared":"legacy",bps,cluster_bytes,reserved_status,backup_status,mirror_status,
           checked?"true":"false",root_directory_status,work_directory_status,work_directory_issue,
           root_names_status,work_names_status,work_names_scope,work_names_issue);
    for(i=0;i<5;i++) {
        uint32_t parent=i?directory.cluster:identity.root_cluster;
        FatHandle *parent_handle=i?&work:&root;
        int status=shared?fat_open(parent_handle,(const uint16_t*)files[i].name,FH_READ,&opened):
                          fat_lookup(&identity,parent,(const uint16_t*)files[i].name,&entry);
        int broken_lfn=i==4 && !wcscmp(fault,L"bad-lfn");
        int broken_chain=i==1 && (!wcscmp(fault,L"late-cycle") || !wcscmp(fault,L"short-chain"));
        int cross_link=i==2 && !wcscmp(fault,L"cross-link");
        unsigned char first=0,*bytes; FatTransfer transfer; uint64_t before;
        int first_status,full_status,match=1; uint64_t first_reads,full_reads; uint32_t at;
        int setup_status=0; uint64_t setup_reads=0;
        if(broken_lfn) {
            CHECK(status==F_NOTFOUND);
            status=shared?fat_open(parent_handle,(const uint16_t*)L"BOUND~1.BIN",FH_READ,&opened):
                          fat_lookup(&identity,parent,(const uint16_t*)L"BOUND~1.BIN",&entry);
        }
        if(shared && status==F_OK) { CHECK(fat_handle_info(&opened,&info)==F_OK); memcpy(&entry,&info,sizeof(info)); }
        CHECK(status==F_OK && entry.size==files[i].size);
        if(streaming) {
            before=image.reads; setup_status=fat_stream_open(&opened,&stream,&map_workspace); setup_reads=image.reads-before;
            CHECK(setup_status==((broken_chain && !wcscmp(fault,L"short-chain"))?F_CORRUPT:F_OK));
        }
        transfer=(FatTransfer){&first,0,1,0}; before=image.reads;
        first_status=setup_status?setup_status:ranged?fat_stream_read_range(&stream,&transfer,&ranges):
                     streaming?fat_stream_read(&stream,&transfer):checked?fat_read_checked(&opened,&transfer,&policy):
                     shared?fat_read_at(&opened,&transfer):fat_read(&identity,&entry,&transfer); first_reads=image.reads-before;
        if(setup_status || (broken_chain && (!shared || checked))) { CHECK(first_status==F_CORRUPT && transfer.done==0); }
        else {
            CHECK(first_status==F_OK && transfer.done==(files[i].size?1u:0u));
            if(files[i].size && !cross_link) CHECK(first==(unsigned char)files[i].seed);
            if(shared && !checked) CHECK(first_reads==(files[i].size?1u:0u));
        }
        bytes=malloc((size_t)files[i].size+19); CHECK(bytes); memset(bytes,0xA5,(size_t)files[i].size+19);
        transfer=(FatTransfer){bytes,0,files[i].size+19,0}; before=image.reads;
        full_status=setup_status?setup_status:ranged?fat_stream_read_range(&stream,&transfer,&ranges):
                    streaming?fat_stream_read(&stream,&transfer):checked?fat_read_checked(&opened,&transfer,&policy):
                    shared?fat_read_at(&opened,&transfer):fat_read(&identity,&entry,&transfer); full_reads=image.reads-before;
        if(broken_chain && (!shared || checked || !wcscmp(fault,L"short-chain"))) {
            CHECK(full_status==F_CORRUPT && transfer.done==((shared && !checked && !streaming)?cluster_bytes:0)); match=0;
            if(checked) CHECK(full_reads==0 && file_check.issue==(uint32_t)(!wcscmp(fault,L"late-cycle")?FC_CYCLE:FC_SHORT));
            for(at=0;at<transfer.done;at++) CHECK(bytes[at]==(unsigned char)(at*29+(at>>8)+files[i].seed));
        }
        else {
            CHECK(full_status==F_OK && transfer.done==files[i].size);
            for(at=0;at<files[i].size;at++) {
                unsigned char expected=(unsigned char)(at*29+(at>>8)+files[i].seed);
                if(bytes[at]!=expected) match=0;
            }
            CHECK(match==!cross_link);
        }
        for(at=transfer.done;at<files[i].size+19;at++) CHECK(bytes[at]==0xA5);
        printf("%s{\"index\":%u,\"short_alias_fallback\":%s,\"first_status\":%d,\"first_reads\":%llu,\"full_status\":%d,\"full_reads\":%llu,\"done\":%u,\"matches_original\":%s,\"setup_status\":%d,\"setup_reads\":%llu}",
               i?",":"",i,broken_lfn?"true":"false",first_status,(unsigned long long)first_reads,
               full_status,(unsigned long long)full_reads,transfer.done,match?"true":"false",setup_status,(unsigned long long)setup_reads);
        free(bytes);
        if(stream.handle.volume) CHECK(fat_stream_close(&stream)==F_OK);
        if(shared) CHECK(fat_close(&opened)==F_OK);
    }
    printf("],\"provider_reads\":%llu,\"range_requests\":%llu,\"ownership_status\":%d,\"ownership_issue\":%u,\"owned_clusters\":%u,\"free_clusters\":%u,\"bad_clusters\":%u,\"orphan_clusters\":%u}\n",
           (unsigned long long)image.reads,(unsigned long long)image.ranges,ownership_status,ownership.check.issue,
           ownership.check.count,ownership.free_clusters,ownership.bad_clusters,ownership.orphan_clusters);
    if(shared) CHECK(fat_close(&work)==F_OK && fat_close(&root)==F_OK && fat_volume_close(&volume)==F_OK);
    CHECK(!fclose(image.file)); return 0;
}
