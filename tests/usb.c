/* Integration harness: compare raw FAT32 reads with normal Win32 file reads.
   Physical writes occur only with --write-test and only in a new GUID directory. */
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
            char text[4096]; unsigned n;
            DWORD error=GetLastError();
            WideCharToMultiByte(CP_UTF8,0,path,-1,text,sizeof(text),NULL,NULL);
            printf("  native lookup failed (Win32 %lu): %s\n",error,text);
            printf("    cluster=%u, slot=%llu, raw=",e.cluster,(unsigned long long)e.index);
            for(n=0;n<32;n++) printf("%02X",e.raw[n]);
            puts(""); ++skipped; continue;
        }
        if((info.dwFileAttributes&0x3F)!=(DWORD)(e.raw[11]&0x3F)) fail("attributes disagree",F_CORRUPT);
        compare_time(&info.ftCreationTime,word_at(e.raw+16),word_at(e.raw+14),e.raw[13]);
        compare_time(&info.ftLastWriteTime,word_at(e.raw+24),word_at(e.raw+22),-1);
        compare_time(&info.ftLastAccessTime,word_at(e.raw+18),0xFFFFFFFFu,-1);
        if(e.raw[11]&0x10) { printf("  directory cluster=%u: %ls\n",e.cluster,path); ++dirs; verify_dir(e.cluster,path,depth+1); }
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
            printf("  matched %10u bytes: %ls\n",e.size,path);
        }
    }
    if(s==F_CORRUPT) { printf("  corrupt directory: %ls\n",parent); ++corrupt; }
    else if(s!=F_END) fail("enumerate",s);
}
static void mount_raw(const wchar_t *device,int write) {
    check("open raw volume",win_open(&volume,device,write));
    check("initialize buffer",sb_init(&buffer,&volume.ops));
    check("mount FAT32",fat_mount(&identity,&buffer.ops,NULL));
}
static void close_raw(void) { check("discard buffer",sb_discard(&buffer)); check("close raw volume",win_close(&volume)); }
static void write_test(const wchar_t *device,const wchar_t *root) {
    wchar_t dir_name[128],path[260]; FatCreate req; FatEntry dir,file; unsigned char bytes[131101],got[131101];
    FILETIME now; unsigned i; HANDLE h; DWORD count;
    GetSystemTimeAsFileTime(&now);
    swprintf_s(dir_name,128,L"FAT32-test-%08X%08X-%lu",now.dwHighDateTime,now.dwLowDateTime,GetCurrentProcessId());
    mount_raw(device,1);
    req=(FatCreate){(const uint16_t *)dir_name,1,0};
    check("create test directory",fat_create(&identity,identity.root_cluster,&req,&dir));
    req=(FatCreate){(const uint16_t *)L"Sector buffer roundtrip.bin",0,0};
    check("create test file",fat_create(&identity,dir.cluster,&req,&file));
    for(i=0;i<sizeof(bytes);i++) bytes[i]=(unsigned char)(i*73+(i>>8));
    { FatTransfer t={bytes,0,sizeof(bytes),0}; check("stage file",fat_write(&identity,&file,&t)); }
    check("commit new test directory",sb_commit(&buffer)); close_raw();
    swprintf_s(path,260,L"%s\\%s\\Sector buffer roundtrip.bin",root,dir_name);
    h=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    if(h==INVALID_HANDLE_VALUE) fail("native reopen after raw commit",F_IO);
    if(!ReadFile(h,got,sizeof(got),&count,NULL) || count!=sizeof(got) || memcmp(bytes,got,count)) fail("native roundtrip",F_CORRUPT);
    CloseHandle(h);
    mount_raw(device,1);
    check("find test dir",fat_lookup(&identity,identity.root_cluster,(const uint16_t *)dir_name,&dir));
    check("find test file",fat_lookup(&identity,dir.cluster,(const uint16_t *)L"Sector buffer roundtrip.bin",&file));
    check("delete test file",fat_remove(&identity,&file));
    check("refresh test dir",fat_lookup(&identity,identity.root_cluster,(const uint16_t *)dir_name,&dir));
    check("delete test dir",fat_remove(&identity,&dir));
    check("commit removal",sb_commit(&buffer)); close_raw();
    swprintf_s(path,260,L"%s\\%s",root,dir_name);
    if(GetFileAttributesW(path)!=INVALID_FILE_ATTRIBUTES || GetLastError()!=ERROR_FILE_NOT_FOUND) fail("removal not visible",F_CORRUPT);
    printf("PASS: raw write / native read / raw delete of isolated test directory\n");
}
int wmain(int argc,wchar_t **argv) {
    wchar_t device[]=L"\\\\.\\X:",root[]=L"X:",mount[]=L"X:\\",label[64],fs[32];
    DWORD serial=0;
    if(argc<2 || argc>3 || wcslen(argv[1])!=2 || argv[1][1]!=L':' ||
       (argc==3 && wcscmp(argv[2],L"--write-test"))) { puts("usbcheck X: [--write-test]"); return 2; }
    device[4]=root[0]=mount[0]=argv[1][0];
    if(!GetVolumeInformationW(mount,label,64,&serial,NULL,NULL,fs,32)) fail("volume identity",F_IO);
    if(wcscmp(label,L"TESTING") || wcscmp(fs,L"FAT32")) fail("expected TESTING FAT32",F_FORMAT);
    printf("TESTING %ls serial %08lX\n",root,serial);
    mount_raw(device,0);
    printf("raw mount: sectors=%u, bytes/sector=%u, bytes/cluster=%u\n",identity.total_sectors,identity.sector_bytes,identity.cluster_bytes);
    verify_dir(identity.root_cluster,root,0); close_raw();
    printf("READ CHECK: %u files, %u directories, %llu bytes; %u native access skips, %u corrupt directories\n",files,dirs,(unsigned long long)verified_bytes,skipped,corrupt);
    if(corrupt || skipped) { puts("Write test withheld: volume verification is incomplete or corrupt."); return 1; }
    if(argc==3) write_test(device,root);
    return 0;
}
