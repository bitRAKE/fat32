/* Integration harness: compare raw FAT32 reads with normal Win32 file reads.
   Physical writes occur only with --write-test and only in a new unique directory. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "api.h"
static WinVolume volume;
static SectorBuffer buffer;
static FatIdentity identity;
static unsigned files,dirs,skipped,corrupt;
static uint64_t verified_bytes;
static __declspec(noreturn) void fail(const char *where,int status) {
    fprintf(stderr,"FAIL: %s: FAT status %d, Win32 error %lu (adapter %u)\n",where,status,GetLastError(),volume.error);
    sb_discard(&buffer); if(volume.handle && volume.handle!=INVALID_HANDLE_VALUE) win_close(&volume); ExitProcess(1);
}
static void check(const char *where,int status) { if(status) fail(where,status); }
static const char *utf8(const wchar_t *path) {
    static char text[131072];
    if(!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,path,-1,text,sizeof(text),NULL,NULL))
        fail("encode evidence path",F_NAME);
    return text;
}
static unsigned word_at(const unsigned char *p) { return p[0] | (unsigned)p[1]<<8; }
static void compare_time(const FILETIME *utc,unsigned date,unsigned time,int tenth) {
    FILETIME local; SYSTEMTIME civil; unsigned native_date,native_time;
    if(!date) return; /* unspecified on-disk dates */
    if(!FileTimeToLocalFileTime(utc,&local) || !FileTimeToSystemTime(&local,&civil)) fail("timestamp conversion",F_IO);
    native_date=((unsigned)civil.wYear-1980)*512+civil.wMonth*32+civil.wDay;
    native_time=civil.wHour*2048+civil.wMinute*32+civil.wSecond/2;
    if(native_date!=date || (time!=0xFFFFFFFFu && native_time!=time)) fail("timestamp disagrees",F_CORRUPT);
    if(tenth>=0 && (unsigned)tenth!=(civil.wSecond%2)*100u+civil.wMilliseconds/10u) fail("creation fraction disagrees",F_CORRUPT);
}
static void verify_dir(uint32_t cluster,const wchar_t *parent,unsigned depth) {
    FatCursor cursor; FatEntry e; int s; wchar_t path[32768];
    if(depth>32) fail("directory depth",F_CORRUPT);
    check("open directory",fat_dir_open(&identity,cluster,&cursor));
    while((s=fat_dir_next(&identity,&cursor,&e))==F_OK) {
        WIN32_FILE_ATTRIBUTE_DATA info;
        if(e.raw[0]=='.') continue;
        if(swprintf_s(path,32768,L"%s\\%s",parent,(const wchar_t *)e.name)<0) fail("path length",F_RANGE);
        if(!GetFileAttributesExW(path,GetFileExInfoStandard,&info)) {
            unsigned n;
            DWORD error=GetLastError();
            printf("  native lookup failed (Win32 %lu): %s\n",error,utf8(path));
            printf("    cluster=%u, slot=%llu, raw=",e.cluster,(unsigned long long)e.index);
            for(n=0;n<32;n++) printf("%02X",e.raw[n]);
            puts(""); ++skipped; continue;
        }
        if((info.dwFileAttributes&0x3F)!=(DWORD)(e.raw[11]&0x3F)) fail("attributes disagree",F_CORRUPT);
        compare_time(&info.ftCreationTime,word_at(e.raw+16),word_at(e.raw+14),e.raw[13]);
        compare_time(&info.ftLastWriteTime,word_at(e.raw+24),word_at(e.raw+22),-1);
        compare_time(&info.ftLastAccessTime,word_at(e.raw+18),0xFFFFFFFFu,-1);
        if(e.raw[11]&0x10) { printf("  directory cluster=%u: %s\n",e.cluster,utf8(path)); ++dirs; verify_dir(e.cluster,path,depth+1); }
        else {
            HANDLE h; unsigned char raw[65536],native[65536]; uint64_t offset=0;
            if(info.nFileSizeHigh || info.nFileSizeLow!=e.size) fail("size disagrees",F_CORRUPT);
            h=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
            if(h==INVALID_HANDLE_VALUE) { ++skipped; continue; }
            while(offset<e.size) {
                DWORD got=0; FatTransfer t={raw,offset,sizeof(raw),0};
                check("raw file read",fat_read(&identity,&e,&t));
                if(!ReadFile(h,native,t.done,&got,NULL) || got!=t.done || memcmp(raw,native,got)) fail("content disagrees",F_CORRUPT);
                if(!got) fail("unexpected EOF",F_CORRUPT); offset+=got;
            }
            CloseHandle(h); ++files; verified_bytes+=offset;
            printf("  matched %10u bytes: %s\n",e.size,utf8(path));
        }
    }
    if(s==F_CORRUPT) { printf("  corrupt directory: %s\n",utf8(parent)); ++corrupt; }
    else if(s!=F_END) fail("enumerate",s);
}
static void mount_raw(const wchar_t *device,int write) {
    unsigned attempt; int status;
    /* Native verification remounts the filesystem; background users can briefly
       prevent an exclusive open. Retry acquisition only, before any staging.
       win_open closes its handle on failure. Never retry a failed commit. */
    for(attempt=0;;attempt++) {
        status=win_open(&volume,device,write);
        if(!status || !write || attempt==20 || status!=F_IO ||
           (volume.error!=ERROR_ACCESS_DENIED && volume.error!=ERROR_SHARING_VIOLATION && volume.error!=ERROR_LOCK_VIOLATION)) break;
        printf("  writable acquisition retry %u: Win32 %u; no sectors staged\n",attempt+1,volume.error);
        Sleep(250);
    }
    check("open raw volume",status);
    check("initialize buffer",sb_init(&buffer,&volume.ops));
    check("mount FAT32",fat_mount(&identity,&buffer.ops,NULL));
}
static void close_raw(void) { check("discard buffer",sb_discard(&buffer)); check("close raw volume",win_close(&volume)); }
static void native_verify(const wchar_t *path,const unsigned char *expected,DWORD length,const FatEntry *entry) {
    HANDLE h; DWORD count; unsigned char *got; WIN32_FILE_ATTRIBUTE_DATA info;
    if(!GetFileAttributesExW(path,GetFileExInfoStandard,&info) || info.nFileSizeHigh || info.nFileSizeLow!=length)
        fail("native size after raw commit",F_CORRUPT);
    if((info.dwFileAttributes&0x3F)!=(DWORD)(entry->raw[11]&0x3F)) fail("native attributes after raw commit",F_CORRUPT);
    compare_time(&info.ftCreationTime,word_at(entry->raw+16),word_at(entry->raw+14),entry->raw[13]);
    compare_time(&info.ftLastWriteTime,word_at(entry->raw+24),word_at(entry->raw+22),-1);
    compare_time(&info.ftLastAccessTime,word_at(entry->raw+18),0xFFFFFFFFu,-1);
    got=malloc(length?length:1); if(!got) fail("native verify memory",F_MEMORY);
    h=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    if(h==INVALID_HANDLE_VALUE) fail("native reopen after raw commit",F_IO);
    if(!ReadFile(h,got,length,&count,NULL) || count!=length || memcmp(expected,got,count)) fail("native roundtrip",F_CORRUPT);
    CloseHandle(h); free(got);
    printf("  native verified: %lu bytes, size, attributes and timestamps\n",length);
}
static void write_test(const wchar_t *device,const wchar_t *root) {
    wchar_t dir_name[128],path[260],old_path[260]; FatCreate req; FatEntry dir,file;
    unsigned char bytes[262144],patch[41];
    FILETIME now; unsigned i; DWORD length=131101,cluster_bytes;
    GetSystemTimeAsFileTime(&now);
    swprintf_s(dir_name,128,L"FAT32-test-%08X%08X-%lu",now.dwHighDateTime,now.dwLowDateTime,GetCurrentProcessId());
    mount_raw(device,1);
    cluster_bytes=identity.cluster_bytes;
    printf("WRITE TEST: cluster=%lu bytes, directory=%s\n",cluster_bytes,utf8(dir_name));
    req=(FatCreate){(const uint16_t *)dir_name,1,0};
    check("create test directory",fat_create(&identity,identity.root_cluster,&req,&dir));
    req=(FatCreate){(const uint16_t *)L"Sector buffer roundtrip.bin",0,0};
    check("create test file",fat_create(&identity,dir.cluster,&req,&file));
    for(i=0;i<length;i++) bytes[i]=(unsigned char)(i*73+(i>>8));
    { FatTransfer t={bytes,0,length,0}; check("stage file",fat_write(&identity,&file,&t)); }
    check("commit new test directory",sb_commit(&buffer));
    check("second commit on same locked handle",sb_commit(&buffer)); close_raw();
    swprintf_s(path,260,L"%s\\%s\\Sector buffer roundtrip.bin",root,dir_name);
    native_verify(path,bytes,length,&file);
    puts("PASS: raw create / write / two flushes / close / native read");

    mount_raw(device,1);
    check("find test dir for edit",fat_lookup(&identity,identity.root_cluster,(const uint16_t *)dir_name,&dir));
    check("find test file for edit",fat_lookup(&identity,dir.cluster,(const uint16_t *)L"Sector buffer roundtrip.bin",&file));
    for(i=0;i<sizeof(patch);i++) patch[i]=(unsigned char)(0xD3^i);
    { FatTransfer t={patch,cluster_bytes-7,sizeof(patch),0}; check("patch across cluster boundary",fat_write(&identity,&file,&t)); }
    memcpy(bytes+cluster_bytes-7,patch,sizeof(patch));
    memset(bytes+length,0,cluster_bytes+17); length+=cluster_bytes+17;
    check("extend and zero fill",fat_resize(&identity,&file,length));
    check("rename long Unicode name",fat_rename(&identity,&file,(const uint16_t *)L"Renamed Ω buffer.bin"));
    { FatStamp stamp={0x8D48,0x5D2F,0x5D2F,0x8D48,0x5D2F,123,0x22}; check("set packed metadata",fat_set_info(&identity,&file,&stamp)); }
    check("commit edits",sb_commit(&buffer)); close_raw();
    wcscpy_s(old_path,260,path); swprintf_s(path,260,L"%s\\%s\\Renamed Ω buffer.bin",root,dir_name);
    if(GetFileAttributesW(old_path)!=INVALID_FILE_ATTRIBUTES || GetLastError()!=ERROR_FILE_NOT_FOUND) fail("old name survived rename",F_CORRUPT);
    native_verify(path,bytes,length,&file);
    puts("PASS: cluster-boundary patch / zero-filled growth / Unicode rename / metadata");

    mount_raw(device,1);
    check("find dir for shrink",fat_lookup(&identity,identity.root_cluster,(const uint16_t *)dir_name,&dir));
    check("find file for shrink",fat_lookup(&identity,dir.cluster,(const uint16_t *)L"Renamed Ω buffer.bin",&file));
    length=cluster_bytes-11;
    check("truncate across cluster chain",fat_resize(&identity,&file,length));
    check("commit truncation",sb_commit(&buffer)); close_raw();
    native_verify(path,bytes,length,&file); puts("PASS: raw truncate / remount / native read");
    mount_raw(device,1);
    check("find test dir",fat_lookup(&identity,identity.root_cluster,(const uint16_t *)dir_name,&dir));
    check("find test file",fat_lookup(&identity,dir.cluster,(const uint16_t *)L"Renamed Ω buffer.bin",&file));
    check("delete test file",fat_remove(&identity,&file));
    check("refresh test dir",fat_lookup(&identity,identity.root_cluster,(const uint16_t *)dir_name,&dir));
    check("delete test dir",fat_remove(&identity,&dir));
    check("commit removal",sb_commit(&buffer)); close_raw();
    swprintf_s(path,260,L"%s\\%s",root,dir_name);
    if(GetFileAttributesW(path)!=INVALID_FILE_ATTRIBUTES || GetLastError()!=ERROR_FILE_NOT_FOUND) fail("removal not visible",F_CORRUPT);
    printf("PASS: raw delete / commit / remount; isolated test directory removed\n");
}
int wmain(int argc,wchar_t **argv) {
    wchar_t device[]=L"\\\\.\\X:",root[]=L"X:",mount[]=L"X:\\",label[64],fs[32];
    DWORD serial=0;
    setvbuf(stdout,NULL,_IONBF,0);
    if(argc<2 || argc>3 || wcslen(argv[1])!=2 || argv[1][1]!=L':' ||
       (argc==3 && wcscmp(argv[2],L"--write-test") && wcscmp(argv[2],L"--flush-test"))) {
        puts("usbcheck X: [--write-test | --flush-test]"); return 2;
    }
    device[4]=root[0]=mount[0]=argv[1][0];
    if(!GetVolumeInformationW(mount,label,64,&serial,NULL,NULL,fs,32)) fail("volume identity",F_IO);
    if(wcscmp(label,L"TESTING") || wcscmp(fs,L"FAT32")) fail("expected TESTING FAT32",F_FORMAT);
    printf("TESTING %s serial %08lX\n",utf8(root),serial);
    mount_raw(device,0);
    printf("raw mount: sectors=%u, bytes/sector=%u, bytes/cluster=%u\n",identity.total_sectors,identity.sector_bytes,identity.cluster_bytes);
    verify_dir(identity.root_cluster,root,0); close_raw();
    printf("READ CHECK: %u files, %u directories, %llu bytes; %u native access skips, %u corrupt directories\n",files,dirs,(unsigned long long)verified_bytes,skipped,corrupt);
    if(corrupt || skipped) { puts("Write test withheld: volume verification is incomplete or corrupt."); return 1; }
    if(argc==3 && !wcscmp(argv[2],L"--flush-test")) {
        mount_raw(device,1);
        check("first flush without sector writes",win_flush(&volume));
        check("second flush on same handle",win_flush(&volume));
        close_raw(); puts("PASS: repeated flush on one locked volume handle");
    } else if(argc==3) write_test(device,root);
    return 0;
}
