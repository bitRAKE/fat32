"""Read-only independent Win32/FAT oracle for run-64k.ps1's retained fixtures.

Arguments: native-fixtures.json SERIAL8 output-directory. Windows only.
Acquire a volume lock (requires a read/write handle, but issues no writes),
capture every sector consumed, then release before any native file access.
The ZIP is a sparse sector capture, not a complete image or repair backup.
"""
import ctypes as C
from ctypes import wintypes as W
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import zipfile

manifest = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8-sig"))
serial = int(sys.argv[2], 16)
output = Path(sys.argv[3]).resolve()
repo = Path(__file__).resolve().parents[1]
if output.is_relative_to(repo):
    ignored = subprocess.run(["git", "-C", str(repo), "check-ignore", "-q", "--", str(output / "sectors.zip")])
    if ignored.returncode:
        raise ValueError("Use ignored build/ storage or an output directory outside the repository")
output.mkdir(parents=True, exist_ok=False)
destination = manifest["Destination"]
drive = destination[:2]
assert len(drive) == 2 and drive[0].isupper() and drive[1] == ":"
k = C.WinDLL("kernel32", use_last_error=True)
k.CreateFileW.argtypes = [W.LPCWSTR, W.DWORD, W.DWORD, C.c_void_p, W.DWORD, W.DWORD, W.HANDLE]
k.CreateFileW.restype = W.HANDLE
k.DeviceIoControl.argtypes = [W.HANDLE, W.DWORD, C.c_void_p, W.DWORD, C.c_void_p, W.DWORD, C.POINTER(W.DWORD), C.c_void_p]
k.SetFilePointerEx.argtypes = [W.HANDLE, C.c_longlong, C.c_void_p, W.DWORD]
k.ReadFile.argtypes = [W.HANDLE, C.c_void_p, W.DWORD, C.POINTER(W.DWORD), C.c_void_p]
k.VirtualAlloc.argtypes = [C.c_void_p, C.c_size_t, W.DWORD, W.DWORD]
k.VirtualAlloc.restype = C.c_void_p
k.VirtualFree.argtypes = [C.c_void_p, C.c_size_t, W.DWORD]
k.CloseHandle.argtypes = [W.HANDLE]
k.GetVolumeInformationW.argtypes = [W.LPCWSTR, W.LPWSTR, W.DWORD, C.POINTER(W.DWORD), C.c_void_p, C.c_void_p, W.LPWSTR, W.DWORD]

def check(value):
    if not value:
        raise C.WinError(C.get_last_error())
    return value

label, filesystem, native_serial = C.create_unicode_buffer(64), C.create_unicode_buffer(32), W.DWORD()
check(k.GetVolumeInformationW(drive + "\\", label, 64, C.byref(native_serial), None, None, filesystem, 32))
assert label.value == "TESTING" and filesystem.value == "FAT32" and native_serial.value == serial
handle = k.CreateFileW("\\\\.\\" + drive, 0xC0000000, 3, None, 3, 0x20000000, None)
if handle == W.HANDLE(-1).value:
    raise C.WinError(C.get_last_error())
memory = check(k.VirtualAlloc(None, 65536, 0x3000, 4))
count = W.DWORD()
captured = {}
bps = 512  # This regression explicitly guards 512-byte-sector, 64-KiB media.
u16 = lambda b, o: struct.unpack_from("<H", b, o)[0]
u32 = lambda b, o: struct.unpack_from("<I", b, o)[0]

def sector(lba):
    if lba not in captured:
        check(k.SetFilePointerEx(handle, lba * bps, None, 0))
        check(k.ReadFile(handle, memory, bps, C.byref(count), None))
        assert count.value == bps
        captured[lba] = C.string_at(memory, bps)
    return captured[lba]

try:
    check(k.DeviceIoControl(handle, 0x90018, None, 0, None, 0, C.byref(count), None))
    boot = sector(0)
    assert u16(boot, 11) == 512 and boot[13] == 128 and u32(boot, 67) == serial
    # Windows can retain NO NAME in the BPB while the root volume label is set.
    assert boot[510:512] == b"\x55\xaa"
    spc, reserved, copies, fat_size = boot[13], u16(boot, 14), boot[16], u32(boot, 36)
    data_start = reserved + copies * fat_size
    clusters = (u32(boot, 32) - data_start) // spc
    assert copies == 2 and u16(boot, 40) & 0x80 == 0

    def link(cluster):
        values = []
        for copy in range(copies):
            values.append(u32(sector(reserved + copy * fat_size + cluster * 4 // bps), cluster * 4 % bps) & 0xFFFFFFF)
        assert values[0] == values[1], (cluster, values)
        return values[0]

    def chain(first):
        result, seen = [], set()
        while first:
            assert 2 <= first < clusters + 2 and first not in seen
            result.append(first)
            seen.add(first)
            first = link(first)
            if first >= 0xFFFFFF8:
                break
        return result

    def cluster_bytes(cluster):
        lba = data_start + (cluster - 2) * spc
        return b"".join(sector(lba + i) for i in range(spc))

    def directory(first):
        pending = []
        for cluster in chain(first):
            data = cluster_bytes(cluster)
            for at in range(0, len(data), 32):
                entry = data[at:at + 32]
                if entry[0] == 0:
                    return
                if entry[0] == 0xE5:
                    pending = []
                    continue
                if entry[11] == 0x0F:
                    pending.append(entry)
                    continue
                checksum = 0
                for byte in entry[:11]:
                    checksum = (((checksum & 1) << 7) + (checksum >> 1) + byte) & 255
                if pending:
                    assert pending[0][0] == (0x40 | len(pending))
                    assert [e[0] & 31 for e in pending] == list(range(len(pending), 0, -1))
                    assert all(e[13] == checksum for e in pending)
                    utf16 = b"".join(e[1:11] + e[14:26] + e[28:32] for e in reversed(pending))
                    name = utf16.decode("utf-16-le").split("\0")[0]
                else:
                    name = entry[:8].decode("cp437").rstrip()
                    ext = entry[8:11].decode("cp437").rstrip()
                    if ext:
                        name += "." + ext
                pending = []
                if entry[11] & 8:
                    continue
                first_cluster = ((u16(entry, 20) << 16) | u16(entry, 26)) & 0xFFFFFFF
                yield name, first_cluster, u32(entry, 28), entry[11]

    root = list(directory(u32(boot, 44)))
    parent = next(e for e in root if e[0] == Path(destination).name)
    entries = {e[0]: e for e in directory(parent[1]) if e[0] not in (".", "..")}
    assert len(entries) == len(manifest["Files"])
    records = []
    owners = set()
    for expected in manifest["Files"]:
        name, first, size, attributes = entries[expected["Name"]]
        sequence = chain(first)
        assert size == expected["Bytes"] and not attributes & 0x10
        assert len(sequence) == (size + 65535) // 65536
        assert not owners.intersection(sequence)
        owners.update(sequence)
        data = b"".join(cluster_bytes(c) for c in sequence)[:size]
        digest = hashlib.sha256(data).hexdigest().upper()
        assert digest == expected["SHA256"], name
        extents = sum(i == 0 or c != sequence[i - 1] + 1 for i, c in enumerate(sequence))
        records.append(dict(name=name, size=size, chain=sequence, extents=extents, raw_sha256=digest))
    fragmented = next(r for r in records if r["name"] == "Fragmented Ω.bin")
    assert fragmented["extents"] > 1, fragmented
    # Include every reachable directory/file for replay by the C harness.
    # Its FAT32 reads then use exactly these immutable sector bytes, without
    # any Win32 raw adapter or concurrent on-device changes.
    def capture_tree(first, depth=0):
        assert depth < 32
        for name, cluster, size, attributes in directory(first):
            if name in (".", ".."):
                continue
            if attributes & 0x10:
                capture_tree(cluster, depth + 1)
            else:
                for c in chain(cluster):
                    cluster_bytes(c)
    capture_tree(u32(boot, 44))
finally:
    # Closing releases the lock; no dismount is needed because nothing changed.
    check(k.CloseHandle(handle))
    check(k.VirtualFree(memory, 0, 0x8000))
    with zipfile.ZipFile(output / "sectors.zip", "w", zipfile.ZIP_DEFLATED) as archive:
        for lba, data in sorted(captured.items()):
            archive.writestr(f"{lba:08X}.bin", data)

result = dict(volume_serial=f"{serial:08X}", bps=bps, spc=spc, data_start=data_start,
              clusters=clusters, captured_sectors=len(captured), files=records)
(output / "result.json").write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
print(f"PASS: locked independent Win32/FAT oracle, {len(records)} file hashes; {len(captured)} sectors captured")
print("Fragmented file:", fragmented)
