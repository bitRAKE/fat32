/* Git-blob fragmentation workload. The PowerShell driver exports immutable
   blobs and compares both raw-library and native readback with git hash-object.
   Every write/delete is committed; final files remain on the test volume. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "api.h"

#define MAX_FILES 4096
#define MAX_PATH_UNITS 2048
#define MAX_BLOB_BYTES (16u*1024u*1024u)
typedef struct RepoFile {
    char oid[65],path[MAX_PATH_UNITS*3];
    wchar_t name[MAX_PATH_UNITS];
    unsigned char *bytes;
    DWORD size;
    int present;
} RepoFile;
static RepoFile *files;
static unsigned count,writes,deletes,commits;
static WinVolume volume;
static SectorBuffer buffer;
static FatIdentity identity;
static wchar_t device[]=L"\\\\.\\X:",mount[]=L"X:\\",session[128];
static DWORD expected_serial;
static uint32_t session_cluster;

static __declspec(noreturn) void fail(const char *where,int status) {
    fprintf(stderr,"FAIL: %s: FAT=%d, Win32=%lu, adapter=%u\n",where,status,GetLastError(),volume.error);
    sb_discard(&buffer);
    if(volume.handle && volume.handle!=INVALID_HANDLE_VALUE) win_close(&volume);
    ExitProcess(1);
}
static void check(const char *where,int status) { if(status) fail(where,status); }
static const char *utf8(const wchar_t *value) {
    static char text[MAX_PATH_UNITS*3];
    if(!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value,-1,text,sizeof(text),NULL,NULL)) fail("UTF-8 output",F_NAME);
    return text;
}
static void join(wchar_t *out,const wchar_t *base,const wchar_t *leaf) {
    if(swprintf_s(out,MAX_PATH_UNITS,L"%s\\%s",base,leaf)<0) fail("local path too long",F_RANGE);
}
static void commit(void) { check("physical commit (never retried)",sb_commit(&buffer)); ++commits; }
static void close_raw(void) { check("discard",sb_discard(&buffer)); check("dismount/close",win_close(&volume)); }
static void open_raw(int write) {
    unsigned attempt; int status; wchar_t label[64],format[32]; DWORD serial;
    if(!GetVolumeInformationW(mount,label,64,&serial,NULL,NULL,format,32)) fail("volume identity",F_IO);
    if(wcscmp(label,L"TESTING") || wcscmp(format,L"FAT32") || serial!=expected_serial) fail("wrong test volume",F_FORMAT);
    for(attempt=0;;attempt++) {
        status=win_open(&volume,device,write);
        if(!status || !write || attempt==20 || status!=F_IO ||
           (volume.error!=ERROR_ACCESS_DENIED && volume.error!=ERROR_SHARING_VIOLATION && volume.error!=ERROR_LOCK_VIOLATION)) break;
        printf("ACQUIRE retry=%u Win32=%u; no changes staged\n",attempt+1,volume.error); Sleep(250);
    }
    check("open raw",status);
    check("init buffer",sb_init(&buffer,&volume.ops));
    check("mount",fat_mount(&identity,&buffer.ops,NULL));
    if(identity.serial!=expected_serial || identity.sector_bytes!=512 || identity.cluster_bytes!=512)
        fail("expected serial and 512-byte sectors/clusters",F_FORMAT);
    printf("MOUNT writable=%d serial=%08X sector=%u cluster=%u sectors=%u\n",write,identity.serial,
           identity.sector_bytes,identity.cluster_bytes,identity.total_sectors);
}
static void load_manifest(const wchar_t *manifest,const wchar_t *blob_dir) {
    FILE *input; char line[MAX_PATH_UNITS*3+128]; uint64_t total=0;
    files=calloc(MAX_FILES,sizeof(*files)); if(!files) fail("manifest memory",F_MEMORY);
    if(_wfopen_s(&input,manifest,L"rb")) fail("open manifest",F_IO);
    while(fgets(line,sizeof(line),input)) {
        char *size,*path,*end; unsigned long length; RepoFile *f; wchar_t oid[65],local[MAX_PATH_UNITS];
        HANDLE h; LARGE_INTEGER actual; DWORD got;
        if(count==MAX_FILES || !strchr(line,'\n')) fail("manifest record bounds",F_RANGE);
        line[strcspn(line,"\r\n")]=0;
        size=strchr(line,'\t'); if(!size) fail("manifest size",F_ARGUMENT); *size++=0;
        path=strchr(size,'\t'); if(!path) fail("manifest path",F_ARGUMENT); *path++=0;
        if(strlen(line)!=40 || strspn(line,"0123456789abcdef")!=40) fail("SHA-1 manifest object ID",F_ARGUMENT);
        length=strtoul(size,&end,10); if(*end || !*size || length>MAX_BLOB_BYTES) fail("blob size limit",F_RANGE);
        if(!*path || *path=='/' || strpbrk(path,"\t\r\n\\:")) fail("manifest relative path",F_NAME);
        f=&files[count]; strcpy_s(f->oid,sizeof(f->oid),line); strcpy_s(f->path,sizeof(f->path),path); f->size=length;
        if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path,-1,f->name,MAX_PATH_UNITS)) fail("manifest UTF-8",F_NAME);
        { wchar_t parts[MAX_PATH_UNITS],*p,*next; wcscpy_s(parts,MAX_PATH_UNITS,f->name); p=parts;
          do { next=wcschr(p,L'/'); if(next) *next++=0;
               if(!*p || !wcscmp(p,L".") || !wcscmp(p,L"..") || wcslen(p)>255) fail("manifest component",F_NAME);
               p=next; } while(p); }
        if(count && (f->size<files[count-1].size || !strcmp(f->path,files[count-1].path))) fail("manifest sorting/duplicate",F_ARGUMENT);
        if(!MultiByteToWideChar(CP_UTF8,0,line,-1,oid,65)) fail("object ID conversion",F_NAME);
        join(local,blob_dir,oid);
        h=CreateFileW(local,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
        if(h==INVALID_HANDLE_VALUE) fail("open exported blob",F_IO);
        if(!GetFileSizeEx(h,&actual) || actual.QuadPart!=f->size) fail("exported blob size",F_CORRUPT);
        f->bytes=malloc(f->size?f->size:1); if(!f->bytes) fail("blob memory",F_MEMORY);
        if(!ReadFile(h,f->bytes,f->size,&got,NULL) || got!=f->size) fail("read exported blob",F_IO);
        CloseHandle(h); total+=f->size; ++count;
    }
    if(ferror(input) || fclose(input) || !count) fail("read manifest",F_IO);
    printf("MANIFEST files=%u bytes=%llu\n",count,(unsigned long long)total);
}
/* Resolve/create only the parent components under our unique session directory.
   Numeric cluster IDs survive entry generation invalidation after mutation. */
static uint32_t parent_of(const RepoFile *f,int create,wchar_t *leaf) {
    wchar_t path[MAX_PATH_UNITS],*part,*slash; uint32_t parent=session_cluster; FatEntry entry;
    wcscpy_s(path,MAX_PATH_UNITS,f->name); part=path;
    while((slash=wcschr(part,L'/'))!=NULL) {
        int status; *slash=0;
        status=fat_lookup(&identity,parent,(const uint16_t *)part,&entry);
        if(status==F_NOTFOUND && create) {
            FatCreate request={(const uint16_t *)part,1,0};
            check("create parent",fat_create(&identity,parent,&request,&entry)); commit();
        } else check("lookup parent",status);
        if(!(entry.raw[11]&0x10)) fail("parent is not directory",F_CORRUPT);
        parent=entry.cluster; part=slash+1;
    }
    wcscpy_s(leaf,MAX_PATH_UNITS,part); return parent;
}
static FatEntry find_file(const RepoFile *f) {
    wchar_t leaf[MAX_PATH_UNITS]; uint32_t parent=parent_of(f,0,leaf); FatEntry entry;
    check("lookup file",fat_lookup(&identity,parent,(const uint16_t *)leaf,&entry)); return entry;
}
static void put_file(unsigned index,unsigned pass) {
    RepoFile *f=&files[index]; FatEntry entry; wchar_t leaf[MAX_PATH_UNITS];
    uint32_t parent=parent_of(f,1,leaf); FatCreate request={(const uint16_t *)leaf,0,0};
    FatTransfer transfer={f->bytes,0,f->size,0};
    if(f->present) fail("write already present file",F_ARGUMENT);
    check("create repo file",fat_create(&identity,parent,&request,&entry));
    check("write repo blob",fat_write(&identity,&entry,&transfer));
    if(transfer.done!=f->size) fail("short blob write",F_IO);
    commit(); f->present=1; ++writes;
    printf("WRITE pass=%u index=%u bytes=%lu first=%u path=%s\n",pass,index,f->size,entry.cluster,f->path);
}
static void delete_file(unsigned index,unsigned pass) {
    RepoFile *f=&files[index]; FatEntry entry=find_file(f);
    if(!f->present) fail("delete missing file",F_ARGUMENT);
    check("delete first of pair",fat_remove(&identity,&entry)); commit(); f->present=0; ++deletes;
    printf("DELETE pass=%u index=%u path=%s\n",pass,index,f->path);
}
static unsigned readback(const wchar_t *directory) {
    FILE *chains; wchar_t local[MAX_PATH_UNITS]; unsigned i,fragmented=0; uint64_t bytes=0;
    join(local,directory,L"chains.tsv"); if(_wfopen_s(&chains,local,L"wb")) fail("chain evidence",F_IO);
    fprintf(chains,"Index\tBytes\tClusters\tExtents\tPath\tChain\n");
    for(i=0;i<count;i++) {
        RepoFile *f=&files[i]; FatEntry entry=find_file(f); uint32_t ends[2],cluster=entry.cluster,k,extents=0,previous=0;
        HANDLE out; wchar_t name[32]; unsigned char data[2039]; uint64_t offset=0;
        if(entry.size!=f->size || (entry.raw[11]&0x10)) fail("readback size/type",F_CORRUPT);
        check("validate chain",fat_chain(&identity,cluster,ends));
        if(ends[0]!=(f->size+511u)/512u) fail("unexpected cluster count",F_CORRUPT);
        for(k=0;k<ends[0];k++) { if(!k || cluster!=previous+1) ++extents; previous=cluster; check("chain link",fat_get(&identity,cluster,&cluster)); }
        if(extents>1) ++fragmented;
        fprintf(chains,"%u\t%lu\t%u\t%u\t%s\t",i,f->size,ends[0],extents,f->path);
        cluster=entry.cluster;
        for(k=0;k<ends[0];k++) { fprintf(chains,"%s%u",k?",":"",cluster); check("record link",fat_get(&identity,cluster,&cluster)); }
        fprintf(chains,"\n");
        swprintf_s(name,32,L"%04u.bin",i); join(local,directory,name);
        out=CreateFileW(local,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
        if(out==INVALID_HANDLE_VALUE) fail("create raw readback file",F_IO);
        while(offset<f->size) {
            FatTransfer transfer={data,offset,sizeof(data),0}; DWORD written;
            check("raw file read",fat_read(&identity,&entry,&transfer));
            if(!transfer.done || transfer.done>f->size-offset) fail("raw read progress",F_CORRUPT);
            if(!WriteFile(out,data,transfer.done,&written,NULL) || written!=transfer.done) fail("save raw readback",F_IO);
            offset+=transfer.done;
        }
        if(!CloseHandle(out)) fail("close readback",F_IO);
        bytes+=offset; printf("READ index=%u bytes=%lu clusters=%u extents=%u path=%s\n",i,f->size,ends[0],extents,f->path);
    }
    if(fclose(chains)) fail("close chain evidence",F_IO);
    printf("READ SUMMARY files=%u bytes=%llu fragmented=%u\n",count,(unsigned long long)bytes,fragmented);
    return fragmented;
}
int wmain(int argc,wchar_t **argv) {
    FILETIME now; FatEntry entry; FatCreate request; unsigned pass=0,i,n,fragmented,*missing;
    wchar_t *end,local[MAX_PATH_UNITS]; FILE *result;
    setvbuf(stdout,NULL,_IONBF,0);
    if(argc!=7 || wcslen(argv[1])!=2 || argv[1][1]!=L':' || wcslen(argv[2])!=8) {
        puts("repocheck X: SERIAL8 manifest.tsv blob_directory readback_directory session-name.txt"); return 2;
    }
    device[4]=mount[0]=argv[1][0]; expected_serial=wcstoul(argv[2],&end,16);
    if(*end || !expected_serial || mount[0]<L'A' || mount[0]>L'Z') fail("drive/serial argument",F_ARGUMENT);
    load_manifest(argv[3],argv[4]);
    missing=malloc(count*sizeof(*missing)); if(!missing) fail("pass memory",F_MEMORY);
    GetSystemTimeAsFileTime(&now);
    swprintf_s(session,128,L"FAT32-repo-%08X%08X-%lu",now.dwHighDateTime,now.dwLowDateTime,GetCurrentProcessId());
    if(_wfopen_s(&result,argv[6],L"wb")) fail("session output",F_IO);
    fprintf(result,"%s\n",utf8(session)); if(fclose(result)) fail("session output close",F_IO);
    printf("SESSION %s\n",utf8(session));
    open_raw(1); request=(FatCreate){(const uint16_t *)session,1,0};
    check("create unique session",fat_create(&identity,identity.root_cluster,&request,&entry)); session_cluster=entry.cluster; commit();
    for(;;) {
        n=0; for(i=0;i<count;i++) if(!files[i].present) missing[n++]=i;
        if(!n) break;
        printf("PASS BEGIN number=%u missing=%u\n",++pass,n);
        for(i=0;i<n;i+=2) {
            put_file(missing[i],pass);
            if(i+1<n) { put_file(missing[i+1],pass); delete_file(missing[i],pass); }
        }
    }
    printf("WRITE SUMMARY passes=%u writes=%u deletes=%u commits=%u remaining=%u\n",pass,writes,deletes,commits,count);
    close_raw();
    /* Drop all identities and caches. Read only sectors from the reopened device. */
    open_raw(0);
    check("find persisted session",fat_lookup(&identity,identity.root_cluster,(const uint16_t *)session,&entry));
    session_cluster=entry.cluster; fragmented=readback(argv[5]); close_raw();
    join(local,argv[5],L"summary.tsv"); if(_wfopen_s(&result,local,L"wb")) fail("summary output",F_IO);
    fprintf(result,"Files\tPasses\tWrites\tDeletes\tCommits\tFragmented\n%u\t%u\t%u\t%u\t%u\t%u\n",count,pass,writes,deletes,commits,fragmented);
    if(fclose(result)) fail("close summary",F_IO);
    if(!fragmented) fail("no fragmented file was produced",F_CORRUPT);
    puts("PASS: committed double loop and persisted raw readback; Git hashes must be checked by the driver");
    for(i=0;i<count;i++) free(files[i].bytes); free(files); free(missing); return 0;
}
