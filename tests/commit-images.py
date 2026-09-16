"""Materialize production ordered-commit prefixes as sparse FAT32 volumes.

Only a fresh directory beneath build/ is accepted. No raw devices are opened.
The fault model is a completed write prefix, or half of one selected metadata
sector, with acknowledged flush boundaries. It does not model every ordering a
real device could expose after losing unflushed writes.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib
import json
from pathlib import Path
import struct
import subprocess
from images import make_sparse, pattern

ROOT = Path(__file__).resolve().parents[1]
OPERATIONS = ("append", "shrink", "rename", "delete", "directory", "metadata")
DIRTY, CLEAN = 0xFFFFFFFE, 0xFFFFFFFF


def decode(trace: Path):
    with trace.open("rb") as source:
        assert source.read(8) == b"FATORD01"
        bps, spc, sectors = struct.unpack("<IIQ", source.read(16))
        state, events = {}, []
        initial = True
        while header := source.read(16):
            kind, phase, lba = struct.unpack("<IIQ", header)
            payload = source.read(bps) if kind in (1, 2) else None
            assert payload is None or len(payload) == bps
            if kind == 1:
                assert initial and lba < sectors and lba not in state
                state[lba] = payload
            elif kind == 0:
                assert initial
                initial = False
            else:
                assert not initial and kind in (2, 3, 4)
                assert kind != 2 or lba < sectors
                events.append((kind, phase, lba, payload))
        assert events[-1][0] == 4
    return bps, spc, sectors, state, events


def inspect(state, bps, spc, sectors):
    """Independent raw decoder: names, chains, ownership and known file bytes."""
    zero = bytes(bps)
    boot = state[0]
    fat_start = struct.unpack_from("<H", boot, 14)[0]
    fat_size = struct.unpack_from("<I", boot, 36)[0]
    data_start = fat_start + 2 * fat_size
    clusters = (sectors - data_start) // spc
    errors, owners, crosslinks, files = [], {}, set(), []

    def fat(cluster, copy=0):
        offset = cluster * 4
        return struct.unpack_from("<I", state.get(fat_start + copy * fat_size + offset // bps, zero), offset % bps)[0] & 0xFFFFFFF

    def chain(first, owner):
        found, seen = [], set()
        while first:
            if not 2 <= first < clusters + 2 or first in seen:
                errors.append(f"{owner}: invalid/cyclic chain at {first}")
                break
            seen.add(first)
            if first in owners and owners[first] != owner:
                crosslinks.add(first)
            owners[first] = owner
            found.append(first)
            following = fat(first)
            if following >= 0xFFFFFF8:
                break
            if following < 2 or following == 0xFFFFFF7:
                errors.append(f"{owner}: unallocated/bad link at {first}")
                break
            first = following
        return found

    def content(nodes):
        return b"".join(state.get(data_start + (node - 2) * spc + offset, zero)
                        for node in nodes for offset in range(spc))

    def directory(first, parent, depth=0):
        if depth > 8:
            errors.append("directory recursion budget")
            return
        raw = content(chain(first, parent))
        for offset in range(0, len(raw), 32):
            entry = raw[offset:offset + 32]
            if not entry[0]:
                break
            if entry[0] == 0xE5 or entry[11] == 15 or entry[11] & 8 or entry[0] == 46:
                continue
            name = entry[:11].decode("ascii", errors="replace").rstrip()
            path = parent + "/" + name
            start = (struct.unpack_from("<H", entry, 20)[0] << 16) | struct.unpack_from("<H", entry, 26)[0]
            size = struct.unpack_from("<I", entry, 28)[0]
            if entry[11] & 16:
                files.append(dict(path=path, directory=True, size=size, first=start))
                directory(start, path, depth + 1)
            else:
                nodes = chain(start, path)
                data = content(nodes)[:size]
                if len(data) < size:
                    errors.append(f"{path}: short chain")
                files.append(dict(path=path, directory=False, size=size, first=start,
                                  clusters=len(nodes), sha256=hashlib.sha256(data).hexdigest(), attributes=entry[11]))

    directory(2, "")
    allocated = {node for node in range(2, clusters + 2) if fat(node)}
    mirrors_equal = all(fat(node) == fat(node, 1) for node in range(clusters + 2))
    return dict(clean=[bool(fat(1, copy) & 0x8000000) for copy in range(2)],
                mirrors_equal=mirrors_equal, errors=errors, crosslinked_clusters=sorted(crosslinks),
                orphan_clusters=sorted(allocated - owners.keys()), files=files)


def save(directory, name, state, bps, sectors, metadata):
    image = directory / (name + ".img")
    digest = hashlib.sha256()
    with image.open("xb") as output:
        make_sparse(output)
        output.truncate(sectors * bps)
        for lba, data in sorted(state.items()):
            if any(data):
                output.seek(lba * bps)
                output.write(data)
                digest.update(struct.pack("<Q", lba) + data)
    return dict(image=image.name, bytes=sectors * bps, sector_bytes=bps,
                nonzero_sector_sha256=digest.hexdigest(), **metadata)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--fsck", type=Path)
    parser.add_argument("--oracle-directory", type=Path)
    args = parser.parse_args()
    destination = args.directory.resolve()
    assert destination.is_relative_to((ROOT / "build").resolve()) and destination != (ROOT / "build").resolve()
    destination.mkdir(parents=True, exist_ok=False)
    if args.oracle_directory:
        assert args.fsck
        args.oracle_directory.mkdir(parents=True, exist_ok=False)
    records = []
    for geometry in range(2):
        for number, operation in enumerate(OPERATIONS):
            name = f"{operation}-{'4096-65536' if geometry else '512-512'}"
            trace = destination / (name + ".trace")
            subprocess.run([str(ROOT / 'build/win32/tests.exe'), "--commit-trace", str(trace), str(number), str(geometry)], check=True, timeout=30)
            bps, spc, sectors, state, events = decode(trace)

            def snapshot(suffix, kind, phase, index, final=False, torn_lba=None):
                observed = inspect(state, bps, spc, sectors)
                if kind != "torn":
                    assert not observed["errors"] and observed["mirrors_equal"], (name, suffix, observed)
                    assert observed["clean"] == ([True, True] if final or kind == "baseline" else [False, False])
                if final or kind == "baseline":
                    assert not observed["crosslinked_clusters"] and not observed["orphan_clusters"], (name, observed)
                if final:
                    regular = [f for f in observed["files"] if not f["directory"]]
                    assert len(regular) == (0 if operation == "delete" else 1)
                    if regular:
                        size = 17 if operation in ("shrink", "directory") else bps * spc * 2 + 17
                        expected = bytes(17) if operation == "directory" else pattern(size, 71)
                        assert regular[0]["size"] == size and regular[0]["sha256"] == hashlib.sha256(expected).hexdigest()
                    assert sum(f["directory"] for f in observed["files"]) == (operation == "directory")
                records.append(save(destination, name + "-" + suffix, state, bps, sectors,
                                    dict(operation=operation, boundary=kind, phase=phase, event=index,
                                         final=final, torn_lba=torn_lba, observation=observed)))

            snapshot("baseline", "baseline", None, -1)
            torn = set()
            boot = state[0]
            fat_start = struct.unpack_from("<H", boot, 14)[0]
            data_start = fat_start + 2 * struct.unpack_from("<I", boot, 36)[0]
            for index, (kind, phase, lba, payload) in enumerate(events):
                if kind == 2:
                    # Half-sector failures in the first FAT and root directory
                    # write, distinct from fully completed phase snapshots.
                    if phase < 0xFFFFFFFD and lba in (fat_start, data_start) and lba not in torn:
                        previous = state.get(lba, bytes(bps))
                        state[lba] = payload[:bps // 2] + previous[bps // 2:]
                        snapshot(f"torn-{index:03}", "torn", phase, index, torn_lba=lba)
                        state[lba] = previous
                        torn.add(lba)
                    state[lba] = payload
                elif kind == 3:
                    snapshot(f"flush-{index:03}", "flush", phase, index, final=phase == CLEAN)
            print("PASS", name, flush=True)
    manifest = dict(format=1, model="production callback write prefixes and half-sector metadata writes", images=records)
    (destination / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    if args.fsck:
        def linux(path):
            return subprocess.check_output(["wsl.exe", "-d", "Ubuntu", "--cd", "/", "--exec", "wslpath", "-a", "-u", str(path.resolve())], text=True).strip()
        fsck = linux(args.fsck)
        clone = importlib.import_module("check-images").sparse_clone
        for record in records:
            image = destination / record["image"]
            oracle_image = image
            if args.oracle_directory:
                oracle_image = args.oracle_directory / image.name
                clone(image, oracle_image)
            check = subprocess.run(["wsl.exe", "-d", "Ubuntu", "--cd", "/", "--exec", fsck, "-n", "-v", linux(oracle_image)],
                                   capture_output=True, text=True, timeout=60)
            output = check.stdout + check.stderr
            (destination / (image.stem + "-fsck.txt")).write_text(output, encoding="utf-8")
            record["fsck_exit"] = check.returncode
            assert check.returncode in (0, 1), (image, output)
            if record["final"] or record["boundary"] == "baseline":
                assert check.returncode == 0 and "Leaving filesystem unchanged" not in output, (image, output)
            elif record["boundary"] == "flush":
                assert "dirty bit" in output.lower(), (image, output)
                if record["observation"]["crosslinked_clusters"]:
                    assert "share clusters" in output, (image, output)
            print("ORACLE", image.name, flush=True)
        (destination / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    print(f"PASS: {len(records)} commit-state images; {len(OPERATIONS) * 2} production traces")


if __name__ == "__main__":
    main()
