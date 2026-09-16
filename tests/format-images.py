"""Materialize actual library formatter output as sparse regular-file volumes.

The C fixture calls assembly formatting and, optionally, shared file operations.
This script encodes no FAT structures. The separate decoder below checks them.
No device paths, partition edits, repair operations or existing-file replacement.
"""
from pathlib import Path
import argparse
import hashlib
import json
import struct
import subprocess
from images import ROOT, make_sparse


def materialize(destination, bps, cluster, *, verified=False, unlabeled=False, efi=None, cache=None):
    destination = Path(destination).resolve()
    assert destination.suffix == '.img' and not destination.exists(), 'fresh .img required'
    assert 'build' in destination.parts and destination.drive, 'local build directory required'
    assert destination.parent.is_dir() and (efi is None) == (cache is None)
    trace = destination.with_suffix('.sectors')
    assert not trace.exists()
    subprocess.run([str(ROOT / 'build/win32/tests.exe'), '--format-trace', str(trace), str(bps),
                    str(cluster), str(int(verified)), str(efi) if efi else ('unlabeled' if unlabeled else '-'),
                    str(cache) if cache else '-'], check=True)
    raw = trace.read_bytes()
    assert raw[:8] == b'FATFMT01'
    actual_bps, spc, sectors = struct.unpack_from('<IIQ', raw, 8)
    assert actual_bps == bps and spc*bps == cluster and (len(raw)-24) % (8+bps) == 0
    seen = set()
    with destination.open('xb') as image:
        make_sparse(image)
        image.truncate(sectors*bps)
        for offset in range(24, len(raw), 8+bps):
            lba, = struct.unpack_from('<Q', raw, offset)
            assert lba < sectors and lba not in seen
            seen.add(lba)
            image.seek(lba*bps)
            image.write(raw[offset+8:offset+8+bps])
    result = dict(path=str(destination), sectorBytes=bps, clusterBytes=cluster,
                  sectors=sectors, retainedSectors=len(seen), verified=verified,
                  traceSHA256=hashlib.sha256(raw).hexdigest(), populated=bool(efi))
    result.update(decode(destination, bps, cluster, empty=not efi, unlabeled=unlabeled))
    destination.with_suffix('.json').write_text(json.dumps(result, indent=2)+'\n')
    return result


def decode(path, bps, cluster, *, empty, unlabeled):
    """Raw BPB/FAT/root oracle; no library mount/read/check APIs."""
    u16 = lambda raw, at: struct.unpack_from('<H', raw, at)[0]
    u32 = lambda raw, at: struct.unpack_from('<I', raw, at)[0]
    with Path(path).open('rb') as image:
        def sector(lba):
            image.seek(lba*bps)
            raw = image.read(bps)
            assert len(raw) == bps
            return raw
        boot = sector(0)
        assert boot == sector(6) and u16(boot, 510) == 0xaa55
        assert u16(boot, 11) == bps and boot[13]*bps == cluster
        assert u16(boot, 14) == 32 and boot[16] == 2 and u32(boot, 28) == 2048
        assert not u16(boot, 17) and not u16(boot, 19) and not u16(boot, 22)
        assert not u16(boot, 40) and not u16(boot, 42) and u32(boot, 44) == 2
        assert u16(boot, 48) == 1 and u16(boot, 50) == 6
        assert u32(boot, 67) == 0x1234abcd and boot[82:90] == b'FAT32   '
        assert boot[71:82] == (b'NO NAME    ' if unlabeled else b'NEW VOLUME ')
        fats, sectors, spc = u32(boot, 36), u32(boot, 32), boot[13]
        start = 32+2*fats
        clusters = (sectors-start)//spc
        assert Path(path).stat().st_size == sectors*bps and 65541 <= clusters <= 0xfffffee
        assert fats*bps//4 >= clusters+2
        assert (fats-1)*bps//4 < (sectors-32-2*(fats-1))//spc+2
        info = sector(1)
        assert info == sector(7) and u32(info, 0) == 0x41615252 and u32(info, 484) == 0x61417272
        assert u32(info, 508) == 0xaa550000 and sector(2) == sector(8)
        image.seek(32*bps)
        fat = image.read(fats*bps)
        assert fat == image.read(fats*bps)
        assert tuple(u32(fat, at) for at in (0,4,8)) == (0xffffff8,0xfffffff,0xfffffff)
        root = b''.join(sector(start+i) for i in range(spc))
        if empty:
            assert not any(fat[12:])
            assert u32(info,488) == clusters-1 and u32(info,492) == 3
            if unlabeled:
                assert not any(root)
            else:
                assert root[:11] == boot[71:82] and root[11] == 8 and not any(root[12:])
            assert sector(start+spc) == bytes([0x7c])*bps, 'quick format erased unrelated data'
        return dict(fatSectors=fats, dataStart=start, clusters=clusters,
                    fatSHA256=hashlib.sha256(fat).hexdigest(), emptyAllocationChecked=empty)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    parser.add_argument('--sector-bytes', type=int, default=512)
    parser.add_argument('--cluster-bytes', type=int, default=4096)
    parser.add_argument('--verified', action='store_true')
    parser.add_argument('--unlabeled', action='store_true')
    parser.add_argument('--efi', type=Path)
    parser.add_argument('--cache', type=Path)
    args = parser.parse_args()
    assert not (args.unlabeled and args.efi)
    print(json.dumps(materialize(args.image, args.sector_bytes, args.cluster_bytes,
                               verified=args.verified, unlabeled=args.unlabeled,
                               efi=args.efi, cache=args.cache), indent=2))


if __name__ == '__main__':
    main()
