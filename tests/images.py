"""Generate deterministic, sparse FAT32 images and independently specified files.

This writer does not call the FAT32 library. Output is restricted to a fresh
directory under this repository's ignored build/ tree; no device paths are used.
The images are volume images (LBA zero is the BPB), not bootable EFI disks.
"""
from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import struct


ROOT = Path(__file__).resolve().parents[1]
CLUSTERS = 65530
SERIAL = 0x09162026
GEOMETRIES = ((512, 1), (512, 8), (512, 128), (4096, 16))
FAULTS = ("clean", "unknown-fsinfo", "dirty", "backup-conflict",
          "mirror-conflict", "late-cycle", "short-chain", "cross-link", "bad-lfn",
          "duplicate-sfn", "duplicate-name", "orphan-chain", "bad-cluster")


def word(data: bytearray, offset: int, value: int) -> None:
    struct.pack_into("<H", data, offset, value)


def dword(data: bytearray, offset: int, value: int) -> None:
    struct.pack_into("<I", data, offset, value)


def pattern(size: int, seed: int) -> bytes:
    return bytes((i * 29 + (i >> 8) + seed) & 255 for i in range(size))


def entry(alias: bytes, attributes: int, cluster: int, size: int) -> bytes:
    assert len(alias) == 11
    result = bytearray(32)
    result[:11], result[11] = alias, attributes
    word(result, 20, cluster >> 16)
    word(result, 26, cluster & 0xFFFF)
    dword(result, 28, size)
    return bytes(result)


def long_entries(name: str, alias: bytes) -> list[bytes]:
    checksum = 0
    for value in alias:
        checksum = (((checksum & 1) << 7) + (checksum >> 1) + value) & 255
    units = list(struct.unpack("<" + "H" * (len(name.encode("utf-16le")) // 2), name.encode("utf-16le")))
    count = (len(units) + 12) // 13
    units += [0] + [0xFFFF] * 13
    records = []
    for ordinal in range(count, 0, -1):
        record = bytearray(32)
        record[0], record[11], record[13] = ordinal | (0x40 if ordinal == count else 0), 15, checksum
        for offset, value in zip((1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30), units[(ordinal - 1) * 13:ordinal * 13]):
            word(record, offset, value)
        records.append(bytes(record))
    return records


def make_sparse(file) -> None:
    if os.name == "nt":
        import msvcrt
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        control = kernel.DeviceIoControl
        control.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p,
                            ctypes.c_uint32, ctypes.c_void_p, ctypes.c_uint32,
                            ctypes.POINTER(ctypes.c_uint32), ctypes.c_void_p]
        control.restype = ctypes.c_int
        returned = ctypes.c_uint32()
        if not control(msvcrt.get_osfhandle(file.fileno()), 0x900C4, None, 0,
                       None, 0, ctypes.byref(returned), None):
            raise ctypes.WinError(ctypes.get_last_error())


def build(destination: Path, bps: int, spc: int, fault: str) -> dict:
    cluster_bytes = bps * spc
    fat_sectors = ((CLUSTERS + 2) * 4 + bps - 1) // bps
    data_start = 32 + 2 * fat_sectors
    sectors = data_start + CLUSTERS * spc
    boot = bytearray(bps)
    boot[:11] = b"\xEB\x58\x90FATTEST "
    word(boot, 11, bps)
    boot[13] = spc
    word(boot, 14, 32)
    boot[16], boot[21] = 2, 0xF8
    word(boot, 24, 63)
    word(boot, 26, 255)
    dword(boot, 32, sectors)
    dword(boot, 36, fat_sectors)
    dword(boot, 44, 2)
    word(boot, 48, 1)
    word(boot, 50, 6)
    boot[64], boot[66] = 0x80, 0x29
    dword(boot, 67, SERIAL)
    boot[71:82], boot[82:90] = b"MIGRATION  ", b"FAT32   "
    word(boot, 510, 0xAA55)
    backup = bytearray(boot)
    if fault == "backup-conflict":
        backup[13] = spc * 2
    fat = bytearray(fat_sectors * bps)
    dword(fat, 0, 0x0FFFFFF8)
    dword(fat, 4, 0x07FFFFFF if fault == "dirty" else 0x0FFFFFFF)
    for cluster in (2, 3):
        dword(fat, cluster * 4, 0x0FFFFFFF)
    files = [
        dict(path="KEEP.TXT", alias=b"KEEP    TXT", chain=[4], size=97, seed=11),
        dict(path="WORK/ALPHA.BIN", alias=b"ALPHA   BIN", chain=[5, 9, 6], size=2 * cluster_bytes + 17, seed=37),
        dict(path="WORK/BETA.BIN", alias=b"BETA    BIN", chain=[7, 11], size=cluster_bytes + 1, seed=71),
        dict(path="WORK/EMPTY.BIN", alias=b"EMPTY   BIN", chain=[], size=0, seed=0),
        dict(path="WORK/Boundary Ω.bin", alias=b"BOUND~1 BIN", chain=[8, 12, 10], size=2 * cluster_bytes + min(513, cluster_bytes), seed=93),
    ]
    for spec in files:
        for i, cluster in enumerate(spec["chain"]):
            dword(fat, cluster * 4, spec["chain"][i + 1] if i + 1 < len(spec["chain"]) else 0x0FFFFFFF)
    if fault == "late-cycle":
        dword(fat, 6 * 4, 9)
    if fault == "short-chain":
        dword(fat, 5 * 4, 0x0FFFFFFF)
    if fault == "orphan-chain":
        dword(fat, 13 * 4, 14)
        dword(fat, 14 * 4, 0x0FFFFFFF)
    elif fault == "bad-cluster":
        dword(fat, 13 * 4, 0x0FFFFFF7)
    other_fat = bytearray(fat)
    if fault == "mirror-conflict":
        dword(other_fat, 6 * 4, 0)
    info = bytearray(bps)
    dword(info, 0, 0x41615252)
    dword(info, 484, 0x61417272)
    dword(info, 488, 0xFFFFFFFF if fault == "unknown-fsinfo" else CLUSTERS - 11)
    dword(info, 492, 0xFFFFFFFF if fault == "unknown-fsinfo" else 13)
    if fault in ("orphan-chain", "bad-cluster"):
        extra = 2 if fault == "orphan-chain" else 1
        dword(info, 488, CLUSTERS - 11 - extra)
        dword(info, 492, 13 + extra)
    dword(info, 508, 0xAA550000)
    root = bytearray(cluster_bytes)
    root[:32] = entry(b"MIGRATION  ", 8, 0, 0)
    root[32:64] = entry(b"WORK       ", 16, 3, 0)
    root[64:96] = entry(files[0]["alias"], 32, 4, files[0]["size"])
    directory = bytearray(cluster_bytes)
    records = [entry(b".          ", 16, 3, 0), entry(b"..         ", 16, 0, 0)]
    for spec in files[1:]:
        if "Ω" in spec["path"]:
            records += long_entries(spec["path"].split("/")[-1], spec["alias"])
            if fault == "bad-lfn":
                broken = bytearray(records[-1])
                broken[13] ^= 1
                records[-1] = bytes(broken)
        first = spec["chain"][0] if spec["chain"] else 0
        if fault == "cross-link" and spec["path"] == "WORK/BETA.BIN":
            first = 9
        records.append(entry(spec["alias"], 32, first, spec["size"]))
    # Keep ordinary lookup's original first match intact, but create another
    # empty object with the same lookup key. This isolates namespace ambiguity
    # from structural encoding, allocation, content, and cross-link damage.
    if fault == "duplicate-sfn":
        records.append(entry(b"ALPHA   BIN", 32, 0, 0))
    elif fault == "duplicate-name":
        records += long_entries("alpha.bin", b"EXTRA~1 BIN")
        records.append(entry(b"EXTRA~1 BIN", 32, 0, 0))
    directory[:len(records) * 32] = b"".join(records)
    assert len(directory) == cluster_bytes
    writes = [(0, boot), (bps, info), (6 * bps, backup), (7 * bps, info),
              (32 * bps, fat), ((32 + fat_sectors) * bps, other_fat),
              (data_start * bps, root), ((data_start + spc) * bps, directory)]
    expected = []
    for spec in files:
        content = pattern(spec["size"], spec["seed"])
        for index, cluster in enumerate(spec["chain"]):
            chunk = content[index * cluster_bytes:(index + 1) * cluster_bytes]
            writes.append(((data_start + (cluster - 2) * spc) * bps, chunk))
        expected.append({k: v for k, v in spec.items() if k != "alias"} |
                        dict(alias=spec["alias"].decode("ascii"), sha256=hashlib.sha256(content).hexdigest()))
    if fault == "orphan-chain":
        # Raw allocated bytes without a namespace entry or recoverable logical
        # length. Future salvage can prove these bytes, not invent a filename.
        writes.append(((data_start + 11 * spc) * bps, pattern(2 * cluster_bytes, 121)))
    path = destination / f"fat32-{bps}-{cluster_bytes}-{fault}.img"
    with path.open("xb") as file:
        make_sparse(file)
        file.truncate(sectors * bps)
        for offset, content in writes:
            file.seek(offset)
            file.write(content)
    # Hash the materialized extents, not gigabytes of deterministic sparse zeros.
    extent_hash = hashlib.sha256()
    for offset, content in sorted(writes):
        extent_hash.update(struct.pack("<QQ", offset, len(content)))
        extent_hash.update(content)
    return dict(image=path.name, bytes=sectors * bps, sector_bytes=bps,
                cluster_bytes=cluster_bytes, data_start=data_start,
                fault=fault, files=expected, materialized_bytes=sum(len(c) for _, c in writes),
                extent_sha256=extent_hash.hexdigest(),
                extents=[dict(offset=o, length=len(c), sha256=hashlib.sha256(c).hexdigest()) for o, c in sorted(writes)])


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    destination = args.output.resolve()
    build_root = (ROOT / "build").resolve()
    if destination == build_root or build_root not in destination.parents:
        parser.error("output must be a fresh subdirectory under the repository build/ directory")
    destination.mkdir(parents=True, exist_ok=False)
    images = [build(destination, bps, spc, "clean") for bps, spc in GEOMETRIES]
    images += [build(destination, 512, 8, fault) for fault in FAULTS if fault != "clean"]
    manifest = dict(schema=1, serial=f"{SERIAL:08X}", volume_relative=True,
                    description="Generated test media; damaged variants must never be used as a boot volume.",
                    images=images)
    (destination / "manifest.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False), encoding="utf-8")
    print(f"Created {len(images)} sparse images in {destination}")
    print(f"Materialized bytes: {sum(i['materialized_bytes'] for i in images)}")


if __name__ == "__main__":
    main()
