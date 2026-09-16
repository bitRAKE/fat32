"""Check a generated corpus with the actual assembly library and optional fsck.

No image is modified. The manifest also supplies expected results to future
basic/checked/shared-handle tests, independently of the current legacy API.
"""
import argparse
import ctypes
import hashlib
import json
from pathlib import Path
import re
import subprocess
import struct
from images import make_sparse

ROOT = Path(__file__).resolve().parents[1]
# fsck.fat 4.2's non-repair diagnostics. Some diagnosed faults return zero;
# checking only its exit status would miss backup and LFN damage.
DIAGNOSTICS = {
    "dirty": r"dirty bit",
    "backup-conflict": r"differences between boot sector and its backup",
    "mirror-conflict": r"FATs differ",
    "late-cycle": r"Circular cluster chain",
    "short-chain": r"File size is .*cluster chain length",
    "cross-link": r"share clusters",
    "bad-lfn": r"Checksum in long filename part wrong",
    "duplicate-sfn": r"Duplicate directory entry",
    "orphan-chain": r"Reclaimed 2 unused clusters",
}


def sparse_clone(source_path, target_path):
    """Copy actual allocated ranges, including data outside manifest extents.

    This makes volume-mount-point repositories accessible to WSL without
    allocating gigabytes of zero-filled holes on the system volume.
    """
    import msvcrt
    control = ctypes.WinDLL("kernel32", use_last_error=True).DeviceIoControl
    control.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p,
                       ctypes.c_uint32, ctypes.c_void_p, ctypes.c_uint32,
                       ctypes.POINTER(ctypes.c_uint32), ctypes.c_void_p]
    control.restype = ctypes.c_int
    length = source_path.stat().st_size
    with source_path.open("rb") as source, target_path.open("xb") as target:
        make_sparse(target)
        target.truncate(length)
        position = 0
        while position < length:
            query = (ctypes.c_int64 * 2)(position, length - position)
            ranges = (ctypes.c_int64 * 512)()
            returned = ctypes.c_uint32()
            ok = control(msvcrt.get_osfhandle(source.fileno()), 0x940CF,
                         query, 16, ranges, ctypes.sizeof(ranges), ctypes.byref(returned), None)
            if not ok and ctypes.get_last_error() != 234:  # ERROR_MORE_DATA
                raise ctypes.WinError(ctypes.get_last_error())
            assert returned.value % 16 == 0
            for i in range(returned.value // 16):
                offset, count = ranges[2 * i], ranges[2 * i + 1]
                assert offset >= position and count > 0
                count = min(count, length - offset)
                source.seek(offset)
                target.seek(offset)
                left = count
                while left:
                    data = source.read(min(left, 1024 * 1024))
                    assert data
                    target.write(data)
                    left -= len(data)
                position = offset + count
            if ok:
                break
            assert returned.value, "range query made no progress"


def namespace_collisions(image, item):
    """Independent raw decoder for the generated WORK directory's one cluster.

    Only the two well-formed name fixtures use this oracle. It is not a repair
    tool or a second general-purpose filesystem reader.
    """
    with image.open('rb') as source:
        source.seek(item['data_start'] * item['sector_bytes'] + item['cluster_bytes'])
        data = source.read(item['cluster_bytes'])
    pending, seen, collisions = [], {}, []
    fold = str.maketrans('ABCDEFGHIJKLMNOPQRSTUVWXYZ', 'abcdefghijklmnopqrstuvwxyz')
    for index in range(len(data) // 32):
        raw = data[index * 32:(index + 1) * 32]
        if not raw[0]:
            break
        assert raw[0] != 0xe5
        if raw[11] == 15:
            pending.append(raw)
            continue
        assert not raw[11] & 8
        alias = raw[:8].decode('ascii').rstrip()
        extension = raw[8:11].decode('ascii').rstrip()
        if extension:
            alias += '.' + extension
        names = {alias}
        if pending:
            checksum = 0
            for value in raw[:11]:
                checksum = (((checksum & 1) << 7) + (checksum >> 1) + value) & 255
            assert all(slot[13] == checksum for slot in pending)
            assert [slot[0] for slot in pending] == [len(pending) | 64, *range(len(pending)-1,0,-1)]
            units = []
            for slot in reversed(pending):
                units += [struct.unpack_from('<H',slot,offset)[0]
                          for offset in (1,3,5,7,9,14,16,18,20,22,24,28,30)]
            if 0 in units:
                units = units[:units.index(0)]
            names.add(struct.pack('<'+'H'*len(units),*units).decode('utf-16le'))
        for key in sorted({name.translate(fold) for name in names}):
            if key in seen:
                collisions.append(dict(name=key,earlier=seen[key],later=index))
            else:
                seen[key] = index
        pending.clear()
    expected_slot = 8 if item['fault'] == 'duplicate-sfn' else 9
    assert collisions == [dict(name='alpha.bin',earlier=2,later=expected_slot)], (image,collisions)
    return collisions


def allocation_fixture(image, item):
    """Decode reachable allocation and the two added allocation fixtures raw."""
    from images import pattern
    with image.open('rb') as source:
        boot = source.read(item['sector_bytes'])
        bps = struct.unpack_from('<H',boot,11)[0]
        spc = boot[13]
        reserved = struct.unpack_from('<H',boot,14)[0]
        fat_sectors = struct.unpack_from('<I',boot,36)[0]
        root = struct.unpack_from('<I',boot,44)[0]
        data = reserved + boot[16] * fat_sectors
        assert data == item['data_start'] and bps * spc == item['cluster_bytes']
        source.seek(reserved*bps)
        fat = source.read(fat_sectors*bps)
        assert source.read(fat_sectors*bps) == fat
        values = [x & 0xfffffff for (x,) in struct.iter_unpack('<I',fat)]
        owned, queue = set(), [(root,True)]
        while queue:
            first, directory = queue.pop()
            chain, current = [], first
            while current:
                assert 2 <= current < 65532 and current not in owned
                owned.add(current); chain.append(current)
                following = values[current]
                if following >= 0xffffff8:
                    break
                assert 2 <= following < 65532
                current = following
            if not directory:
                continue
            ended = False
            for cluster in chain:
                source.seek((data+(cluster-2)*spc)*bps)
                slots = source.read(spc*bps)
                for offset in range(0,len(slots),32):
                    raw = slots[offset:offset+32]
                    if not raw[0]:
                        ended = True; break
                    if raw[0] in (0xe5,ord('.')) or raw[11] & 8:
                        continue
                    child = ((struct.unpack_from('<H',raw,20)[0]<<16) | struct.unpack_from('<H',raw,26)[0]) & 0xfffffff
                    if child:
                        queue.append((child,bool(raw[11]&16)))
                if ended:
                    break
        free = {c for c in range(2,65532) if not values[c]}
        bad = {c for c in range(2,65532) if values[c] == 0xffffff7}
        unowned = set(range(2,65532)) - owned - free - bad
        assert owned == set(range(2,13))
        result = dict(owned_clusters=len(owned),free_clusters=len(free),
                      marked_bad_clusters=sorted(bad),unowned_clusters=sorted(unowned))
        if item['fault'] == 'orphan-chain':
            assert unowned == {13,14} and not bad and values[13] == 14 and values[14] >= 0xffffff8
            source.seek((data+11*spc)*bps)
            content = source.read(2*spc*bps)
            assert content == pattern(len(content),121)
            result['unowned_payload_sha256'] = hashlib.sha256(content).hexdigest()
            result['logical_filename_and_length_known'] = False
        else:
            assert not unowned and bad == {13}
        return result


def check_fat_views(image, item, observation, boot_sector=0):
    """Decode each copy independently; a healthy first FAT cannot vouch for another."""
    with image.open('rb') as source:
        source.seek(boot_sector*item['sector_bytes'])
        boot = source.read(item['sector_bytes'])
        bps = struct.unpack_from('<H',boot,11)[0]
        reserved = struct.unpack_from('<H',boot,14)[0]
        fat_sectors = struct.unpack_from('<I',boot,36)[0]
        data_start = reserved+boot[16]*fat_sectors
        clusters = (struct.unpack_from('<I',boot,32)[0]-data_start)//boot[13]
        def directory(cluster):
            source.seek((data_start+(cluster-2)*boot[13])*bps)
            raw=source.read(item['cluster_bytes']); entries={}
            for offset in range(0,len(raw),32):
                slot=raw[offset:offset+32]
                if slot[0]==0: break
                if slot[0]==0xe5 or slot[11]==15: continue
                first=(struct.unpack_from('<H',slot,20)[0]<<16)|struct.unpack_from('<H',slot,26)[0]
                entries.setdefault(slot[:11].decode('ascii'),(first,struct.unpack_from('<I',slot,28)[0]))
            return entries
        root=directory(struct.unpack_from('<I',boot,44)[0])
        work=directory(root['WORK       '][0])
        assert len(observation['copies']) == boot[16] == 2
        for copy, result in enumerate(observation['copies']):
            assert result['fat_copy'] == copy and len(result['files']) == len(item['files'])
            source.seek((reserved+copy*fat_sectors)*bps)
            table = source.read(fat_sectors*bps)
            for index, (expected, actual) in enumerate(zip(item['files'],result['files'])):
                first,size = (work if '/' in expected['path'] else root)[expected['alias']]
                assert actual['first']==first and actual['size']==size==expected['size'], (image,copy,index,actual)
                visited=set(); cluster=first; issue=0
                while cluster:
                    if cluster<2 or cluster>=clusters+2:
                        issue=1; break  # FC_LINK
                    if cluster in visited:
                        issue=2; break  # FC_CYCLE
                    visited.add(cluster)
                    following=struct.unpack_from('<I',table,cluster*4)[0]&0x0fffffff
                    if following>=0x0ffffff8: break
                    if following<2 or following>=clusters+2:
                        issue=1; break
                    cluster=following
                needed=(expected['size']+item['cluster_bytes']-1)//item['cluster_bytes']
                if not issue and len(visited)<needed: issue=3  # FC_SHORT
                assert actual['status']==(4 if issue else 0) and actual['issue']==issue, (image,copy,index,actual,issue)
                if not issue:
                    assert actual['count']==len(visited) and actual['flags']==(1 if len(visited)>needed else 0)
                # Independent set-based candidate prefix, stopping before an
                # unreadable/invalid allocation or repeated cluster. Raw bytes
                # come from the image, not the library's returned extent list.
                prefix=[]; seen=set(); current=first; salvage_issue=0
                while len(prefix)*item['cluster_bytes'] < size:
                    if not current:
                        salvage_issue=3; break
                    if not 2<=current<clusters+2:
                        salvage_issue=1; break
                    if current in seen:
                        salvage_issue=2; break
                    following=struct.unpack_from('<I',table,current*4)[0]&0xfffffff
                    if following<0xffffff8 and not 2<=following<clusters+2:
                        salvage_issue=1; break
                    seen.add(current); prefix.append(current)
                    if len(prefix)*item['cluster_bytes']>=size: break
                    if following>=0xffffff8:
                        salvage_issue=3; break
                    current=following
                content=bytearray(); extents=[]
                for cluster in prefix:
                    source.seek((data_start+(cluster-2)*boot[13])*bps)
                    content.extend(source.read(item['cluster_bytes']))
                    if extents and sum(extents[-1])==cluster: extents[-1][1]+=1
                    else: extents.append([cluster,1])
                content=content[:size]; checksum=14695981039346656037
                for byte in content: checksum=((checksum^byte)*1099511628211)&0xffffffffffffffff
                assert actual['salvage_status']==(4 if salvage_issue else 0) and actual['salvage_issue']==salvage_issue,(image,copy,index,actual)
                assert actual['available']==actual['done']==len(content) and actual['extents']==extents,(image,copy,index,actual)
                assert actual['fnv64']==f'{checksum:016x}',(image,copy,index,actual)
    return dict(independentCopies=2,filesChecked=2*len(item['files']),salvageFilesChecked=2*len(item['files']))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--reader", type=Path, default=ROOT / 'build/win32/imagecheck.exe',
                        help="Harness image reader implementing the observation profiles")
    parser.add_argument("--fsck", type=Path, help="Windows path to Linux fsck.fat, run through WSL Ubuntu")
    parser.add_argument("--oracle-directory", type=Path, help="Fresh WSL-accessible directory for exact sparse clones")
    args = parser.parse_args()
    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    directory = args.manifest.resolve().parent
    if args.oracle_directory:
        assert args.fsck, "oracle-directory requires fsck"
        args.oracle_directory.mkdir(parents=True, exist_ok=False)
    results = []
    for item in manifest["images"]:
        image = directory / item["image"]
        assert image.parent == directory and image.stat().st_size == item["bytes"]
        with image.open("rb") as source:
            for extent in item["extents"]:
                source.seek(extent["offset"])
                assert hashlib.sha256(source.read(extent["length"])).hexdigest() == extent["sha256"], image
        process = subprocess.run([str(args.reader), str(image),
                                  str(item["sector_bytes"]), str(item["cluster_bytes"]), item["fault"]],
                                 check=True, capture_output=True, text=True, timeout=30)
        result = dict(image=item["image"], observation=json.loads(process.stdout))
        process = subprocess.run([str(args.reader), str(image),
                                  str(item["sector_bytes"]), str(item["cluster_bytes"]), item["fault"], "shared"],
                                 check=True, capture_output=True, text=True, timeout=30)
        result["shared_observation"] = json.loads(process.stdout)
        process = subprocess.run([str(args.reader), str(image),
                                  str(item["sector_bytes"]), str(item["cluster_bytes"]), item["fault"], "checked"],
                                 check=True, capture_output=True, text=True, timeout=30)
        result["checked_observation"] = json.loads(process.stdout)
        process = subprocess.run([str(args.reader), str(image),
                                  str(item["sector_bytes"]), str(item["cluster_bytes"]), item["fault"], "views"],
                                 check=True, capture_output=True, text=True, timeout=30)
        result['fat_views'] = json.loads(process.stdout)
        result['fat_view_oracle'] = check_fat_views(image,item,result['fat_views'])
        if item['fault'] in ('duplicate-sfn','duplicate-name'):
            result['namespace_collisions'] = namespace_collisions(image,item)
        if item['fault'] in ('orphan-chain','bad-cluster'):
            result['allocation_oracle'] = allocation_fixture(image,item)
            checked = result['checked_observation']
            oracle = result['allocation_oracle']
            assert checked['owned_clusters'] == oracle['owned_clusters']
            assert checked['free_clusters'] == oracle['free_clusters']
            assert checked['orphan_clusters'] == len(oracle['unowned_clusters'])
            assert checked['bad_clusters'] == len(oracle['marked_bad_clusters'])
        for profile in ("stream", "range"):
            process = subprocess.run([str(args.reader), str(image), str(item["sector_bytes"]),
                                      str(item["cluster_bytes"]), item["fault"], profile],
                                     check=True, capture_output=True, text=True, timeout=30)
            result[profile + "_observation"] = json.loads(process.stdout)
        if args.fsck:
            oracle_image = image
            if args.oracle_directory:
                oracle_image = args.oracle_directory / image.name
                sparse_clone(image, oracle_image)
            def linux(path):
                return subprocess.check_output(["wsl.exe", "-d", "Ubuntu", "--cd", "/", "--exec", "wslpath", "-a", "-u", str(path.resolve())], text=True).strip()
            check = subprocess.run(["wsl.exe", "-d", "Ubuntu", "--cd", "/", "--exec", linux(args.fsck), "-n", "-v", linux(oracle_image)],
                                   capture_output=True, text=True, timeout=60)
            (directory / (image.stem + "-fsck.txt")).write_text(check.stdout + check.stderr, encoding="utf-8")
            result["fsck_exit"] = check.returncode
            if item["fault"] in ("clean", "unknown-fsinfo", "bad-cluster"):
                assert check.returncode == 0, (image, check.stdout, check.stderr)
            elif item["fault"] == "duplicate-name":
                # fsck.fat 4.2 does not compare this LFN with another SFN.
                # Preserve the oracle's actual limitation, not a healthy label.
                assert check.returncode == 0 and not re.search(r"Duplicate directory entry",check.stdout,re.I), (image,check.stdout)
                assert result["checked_observation"]["work_names_issue"] == 16  # FC_DUPLICATE
                result["fsck_known_limitation"] = "LFN/short-alias lookup collision not diagnosed"
            else:
                assert check.returncode in (0, 1), (image, check.stdout, check.stderr)
                assert re.search(DIAGNOSTICS[item["fault"]], check.stdout, re.I), (image, check.stdout)
                result["fsck_expected_diagnostic"] = True
        results.append(result)
        print("PASS", item["image"])
    (directory / "observations.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
    print(f"PASS: {len(results)} images; library observations and expected content")


if __name__ == "__main__":
    main()
