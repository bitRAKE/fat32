/* Host verification of the example, with a synthetic five-argument firmware
   callback. No firmware entry, device access, or Win32 sector adapter involved. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "../../tests/api.h"

typedef uint64_t (*ReadBlocks)(void *,uint32_t,uint64_t,uint64_t,void *);
typedef struct GuideEfiRead {
    void *protocol;
    ReadBlocks read_blocks;
    uint64_t base_lba,sectors;
    void *bounce;
    uint64_t last_status;
    uint32_t media_id,block_bytes,online,reserved;
} GuideEfiRead;
_Static_assert(sizeof(GuideEfiRead)==64,"example context");
_Static_assert(offsetof(GuideEfiRead,online)==56,"example online flag");
extern int guide_efi_read(GuideEfiRead *,uint64_t,void *);
extern int guide_mount_readonly(FatIdentity *,GuideEfiRead *,SectorOps *);
extern int guide_read_root_range(FatIdentity *,const uint16_t *,FatTransfer *);

#define CHECK(c) do { if(!(c)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#c); exit(1); } } while(0)
enum { BYTES=512, FAT_SECTORS=512, DATA=32+2*FAT_SECTORS, SECTORS=DATA+65525 };
typedef struct Firmware { uint64_t base,status; uint32_t media_id,calls; void *bounce; } Firmware;
static unsigned char boot[BYTES],fat[BYTES],root[BYTES],payload[2*BYTES];
static void wr16(unsigned char *p,uint16_t v) { p[0]=(unsigned char)v; p[1]=(unsigned char)(v>>8); }
static void wr32(unsigned char *p,uint32_t v) { wr16(p,(uint16_t)v); wr16(p+2,(uint16_t)(v>>16)); }
static uint64_t mock_read(void *this_pointer,uint32_t media_id,uint64_t lba,uint64_t bytes,void *buffer) {
    Firmware *f=this_pointer;
    CHECK(bytes==BYTES && buffer==f->bounce && ((uintptr_t)buffer&511)==0);
    CHECK(lba>=f->base && lba-f->base<SECTORS); ++f->calls;
    memset(buffer,0xEE,BYTES); /* A failed firmware read may overwrite bounce. */
    if(media_id!=f->media_id) return 0x800000000000000Dull;
    if(f->status) return f->status;
    lba-=f->base; memset(buffer,0,BYTES);
    if(lba==0) memcpy(buffer,boot,BYTES);
    else if(lba==32 || lba==32+FAT_SECTORS) memcpy(buffer,fat,BYTES);
    else if(lba==DATA) memcpy(buffer,root,BYTES);
    else if(lba==DATA+1 || lba==DATA+2) memcpy(buffer,payload+(size_t)(lba-DATA-1)*BYTES,BYTES);
    return 0;
}
int main(void) {
    static __declspec(align(512)) unsigned char bounce[BYTES];
    static FatIdentity identity;
    SectorOps ops; FatTransfer transfer; FatEntry entry;
    Firmware firmware={0x100000005ull,0,37,0,bounce};
    GuideEfiRead context={&firmware,mock_read,firmware.base,SECTORS,bounce,0,37,BYTES,1,0};
    unsigned char output[802]; unsigned i; uint32_t calls;
    const uint16_t name[]={'b','o','o','t','.','b','i','n',0},missing[]={'n','o',0};

    boot[0]=0xEB; boot[1]=0x58; boot[2]=0x90;
    wr16(boot+11,BYTES); boot[13]=1; wr16(boot+14,32); boot[16]=2; boot[21]=0xF8;
    wr32(boot+32,SECTORS); wr32(boot+36,FAT_SECTORS); wr32(boot+44,2); wr16(boot+510,0xAA55);
    wr32(fat,0x0FFFFFF8); wr32(fat+4,0x0FFFFFFF); wr32(fat+8,0x0FFFFFFF);
    wr32(fat+12,4); wr32(fat+16,0x0FFFFFFF);
    memcpy(root,"BOOT    BIN",11); root[11]=0x20; wr16(root+26,3); wr32(root+28,777);
    for(i=0;i<sizeof(payload);i++) payload[i]=(unsigned char)(i*13+7);

    CHECK(guide_mount_readonly(&identity,&context,&ops)==F_OK);
    CHECK(ops.context==&context && ops.sectors==SECTORS && ops.sector_bytes==BYTES);
    CHECK(!ops.write && !ops.begin && !ops.end && !ops.flush && !ops.reserved);
    memset(output,0xA5,sizeof(output)); transfer=(FatTransfer){output+1,0,800,99};
    CHECK(guide_read_root_range(&identity,name,&transfer)==F_OK && transfer.done==777);
    CHECK(!memcmp(output+1,payload,777) && output[0]==0xA5 && output[778]==0xA5);
    transfer.offset=500; transfer.length=64;
    CHECK(guide_read_root_range(&identity,name,&transfer)==F_OK && transfer.done==64);
    CHECK(!memcmp(output+1,payload+500,64));
    transfer.offset=777;
    CHECK(guide_read_root_range(&identity,name,&transfer)==F_OK && transfer.done==0);
    transfer.done=99;
    CHECK(guide_read_root_range(&identity,missing,&transfer)==F_NOTFOUND && transfer.done==0);
    CHECK(fat_lookup(&identity,2,name,&entry)==F_OK);
    CHECK(fat_resize(&identity,&entry,1)==F_READONLY);

    calls=firmware.calls;
    CHECK(guide_efi_read(&context,SECTORS,output+1)==F_RANGE && firmware.calls==calls);
    context.base_lba=UINT64_MAX;
    CHECK(guide_efi_read(&context,1,output+1)==F_RANGE && firmware.calls==calls);
    context.base_lba=firmware.base;
    /* High bits alone must be examined; testing only EAX would miss this status. */
    firmware.status=0x8000000000000000ull; memset(output,0xA5,sizeof(output));
    CHECK(guide_efi_read(&context,0,output+1)==F_IO && !context.online);
    CHECK(context.last_status==firmware.status);
    for(i=0;i<sizeof(output);i++) CHECK(output[i]==0xA5);
    calls=firmware.calls;
    CHECK(guide_efi_read(&context,0,output+1)==F_IO && firmware.calls==calls);
    /* Direct callback probe only: deliberately reset after failure, no FAT calls. */
    firmware.status=0; ++firmware.media_id; context.online=1;
    CHECK(guide_efi_read(&context,0,output+1)==F_IO && !context.online);
    CHECK(context.last_status==0x800000000000000Dull);
    puts("PASS: UEFI example host mock: fifth argument, 64-bit LBA/status, bounce, mount/read/EOF, bounds, offline/media change");
    return 0;
}
