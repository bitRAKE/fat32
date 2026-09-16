"""Prove library feature removal in COFF objects and linked consumer images.

Run from the NMAKE feature-test target (x64 MSVC environment). No disk devices
are opened. Generated probes, maps, PDBs and reports remain under build/features.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess


ROOT = Path(__file__).resolve().parents[2]


def llvm_directory() -> Path | None:
    """Use caller configuration or the tool on PATH; no machine-specific root."""
    configured = os.environ.get('LLVM_BIN')
    if configured:
        return Path(configured)
    executable = shutil.which('llvm-readobj')
    return Path(executable).parent if executable else None


PROFILES = {
    "mount": ["fat_mount"],
    "fat-view": ["fat_view_open", "fat_view_close", "fat_check_chain"],
    "boot-view": ["fat_view_boot", "fat_view_close"],
    "policy": ["fat_policy_init", "fat_policy_note", "fat_policy_call", "fat_policy_close"],
    "policy-read": ["fat_mount", "fat_volume_init", "fat_root", "fat_open", "fat_read_at", "fat_close", "fat_policy_init", "fat_policy_note", "fat_policy_call", "fat_policy_close"],
    "policy-adaptive": ["fat_mount", "fat_volume_init", "fat_root", "fat_open", "fat_read_at", "fat_close", "fat_check_file", "fat_policy_init", "fat_policy_note", "fat_policy_call", "fat_policy_close"],
    "salvage-plan": ["fat_salvage_plan"],
    "salvage-read": ["fat_salvage_plan", "fat_salvage_read"],
    "format-plan": ["fat_format_plan"],
    "format": ["fat_format"],
    "format-verified": ["fat_format_verified"],
    "put": ["fat_mount", "fat_put"],
    "put-checked": ["fat_mount", "fat_put_checked"],
    "free-space": ["fat_mount", "fat_count_free"],
    "read": ["fat_mount", "fat_lookup", "fat_read"],
    "append": ["fat_mount", "fat_lookup", "fat_create", "fat_write", "fat_read"],
    "full": ["fat_mount", "fat_invalidate", "fat_get", "fat_chain",
             "fat_dir_open", "fat_dir_next", "fat_lookup", "fat_read",
             "fat_write", "fat_resize", "fat_set_info", "fat_create",
             "fat_remove", "fat_rename"],
    "shared-read": ["fat_mount", "fat_volume_init", "fat_root", "fat_open", "fat_read_at", "fat_close"],
    "shared-write": ["fat_mount", "fat_volume_init", "fat_root", "fat_open", "fat_new",
                     "fat_read_at", "fat_write_at", "fat_close"],
    "shared-full": ["fat_mount", "fat_volume_init", "fat_root", "fat_open", "fat_new", "fat_close",
                    "fat_handle_info", "fat_handle_get_stamp", "fat_seek", "fat_read_at", "fat_write_at", "fat_read_next", "fat_write_next",
                    "fat_handle_resize", "fat_handle_set_info", "fat_handle_rename", "fat_unlink",
                    "fat_iter_open", "fat_iter_next", "fat_iter_close", "fat_volume_close"],
    "locked-read": ["fat_mount", "fat_volume_init", "fat_root", "fat_open", "fat_read_at", "fat_close", "fat_call_locked"],
    "checked-read": ["fat_mount", "fat_volume_init", "fat_root", "fat_open", "fat_read_checked", "fat_close"],
    "adaptive-read": ["fat_mount", "fat_volume_init", "fat_root", "fat_open", "fat_read_adaptive", "fat_close"],
    "diagnostics": ["fat_mount", "fat_check_chain", "fat_check_reserved", "fat_check_backup", "fat_check_mirrors"],
    "directory-check": ["fat_mount", "fat_volume_init", "fat_root", "fat_check_directory", "fat_close"],
    "name-check": ["fat_mount", "fat_volume_init", "fat_root", "fat_check_names", "fat_close"],
    "ownership-check": ["fat_mount", "fat_check_ownership"],
    "ordered-write": ["fat_mount", "fat_volume_init", "fat_root", "fat_open", "fat_new", "fat_read_at", "fat_write_at",
                      "fat_close", "fat_order_init", "fat_order_discard", "fat_order_commit"],
    "verified-write": ["fat_mount", "fat_volume_init", "fat_root", "fat_open", "fat_new", "fat_read_at", "fat_write_at",
                       "fat_close", "fat_order_init", "fat_order_discard", "fat_order_commit_verified"],
    "stream-read": ["fat_mount", "fat_volume_init", "fat_root", "fat_open", "fat_stream_open", "fat_stream_read", "fat_stream_close", "fat_close"],
    "stream-map": ["fat_mount", "fat_volume_init", "fat_root", "fat_open", "fat_stream_open", "fat_stream_sector", "fat_stream_close", "fat_close"],
    "range-read": ["fat_mount", "fat_volume_init", "fat_root", "fat_open", "fat_stream_open", "fat_stream_read_range", "fat_stream_close", "fat_close"],
    "ordered-range": ["fat_mount", "fat_volume_init", "fat_root", "fat_open", "fat_new", "fat_read_at", "fat_write_at",
                      "fat_close", "fat_order_init", "fat_order_discard", "fat_order_commit", "fat_stream_open",
                      "fat_stream_read_range", "fat_stream_close", "fat_order_read_range"],
}
WRITE_FUNCTIONS = {
    "fat_format", "fat_format_verified", "f_format_run", "f_format_emit", "f_format_flush",
    "fat_write", "fat_resize", "fat_set_info", "fat_create", "fat_remove",
    "fat_rename", "f_write_bytes", "f_write_sector", "fat_put", "fat_put_checked", "f_begin", "f_order",
    "f_finish", "f_info_unknown", "f_allocate", "f_free", "f_store",
    "f_resize", "f_create", "f_alias", "f_slots", "f_erase",
    "f_write_record", "f_resize_record", "f_set_record", "f_rename_record", "f_remove_record",
    "fat_write_at", "fat_new", "fat_handle_resize", "fat_handle_set_info", "fat_handle_rename", "fat_unlink",
}
# Legacy API regression ceilings, including the small executable probe and
# linker alignment. New API profiles get separate, explicitly reviewed budgets.
CODE_LIMITS = {"salvage-plan": 1536, "salvage-read": 2048, "fat-view": 1088, "format-plan": 512, "format": 1792, "format-verified": 1920, "put": 1152, "put-checked": 1536, "mount": 768, "free-space": 1280, "read": 4352, "append": 8704, "full": 10496,
               "policy": 3072, "policy-read": 7168, "policy-adaptive": 7936, "boot-view": 1536, "shared-read": 4608, "shared-write": 9728, "shared-full": 12032, "locked-read": 4864,
               "checked-read": 5632, "adaptive-read": 5632, "diagnostics": 2560, "directory-check": 3584,
               "name-check": 5376,
               "ownership-check": 3200,
               "ordered-write": 12032, "verified-write": 12288,
               "stream-read": 4864, "stream-map": 4608, "range-read": 5376, "ordered-range": 13824}


def run(command: list[str], output: Path | None = None) -> str:
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True,
                            errors="replace", timeout=120)
    text = result.stdout + result.stderr
    if output:
        output.write_text(text, encoding="utf-8")
    if result.returncode:
        raise RuntimeError(f"command failed ({result.returncode}): {command}\n{text}")
    return text


def object_sections(path: Path) -> list[dict]:
    """Read the emitted classic/bigobj section definitions and relocations."""
    data = path.read_bytes()
    u16 = lambda offset: struct.unpack_from("<H", data, offset)[0]
    u32 = lambda offset: struct.unpack_from("<I", data, offset)[0]
    big = data[:4] == b"\0\0\xff\xff"
    if big:
        assert u16(6) == 0x8664
        count, symptr, nsyms, start, symsize = u32(44), u32(48), u32(52), 56, 20
    else:
        assert u16(0) == 0x8664
        count, symptr, nsyms, start, symsize = u16(2), u32(8), u32(12), 20 + u16(16), 18
    strings = symptr + nsyms * symsize

    def name(raw: bytes) -> str:
        if raw[:1] == b"/":
            offset = int(raw[1:].split(b"\0")[0])
        elif raw[:4] == b"\0" * 4:
            offset = struct.unpack_from("<I", raw, 4)[0]
        else:
            return raw.split(b"\0")[0].decode("ascii")
        return data[strings + offset:].split(b"\0")[0].decode("ascii")

    sections = []
    for i in range(count):
        at = start + i * 40
        sections.append(dict(number=i + 1, name=name(data[at:at + 8]),
                             size=u32(at + 16), raw=u32(at + 20),
                             reloc=u32(at + 24), nreloc=u16(at + 32),
                             flags=u32(at + 36), selection=0, association=0,
                             symbols=[], targets=[]))
    symbols = {}
    i = 0
    while i < nsyms:
        at = symptr + i * symsize
        symbol = name(data[at:at + 8])
        section = struct.unpack_from("<i" if big else "<h", data, at + 12)[0]
        storage, aux = data[at + symsize - 2:at + symsize]
        symbols[i] = (symbol, section)
        if section > 0:
            item = sections[section - 1]
            item["symbols"].append(symbol)
            if storage == 3 and aux and symbol == item["name"]:
                extra = at + symsize
                item["selection"] = data[extra + 14]
                item["association"] = u16(extra + 12) | (u16(extra + 16) << 16)
        i += 1 + aux
    for item in sections:
        for n in range(item["nreloc"]):
            at = item["reloc"] + 10 * n
            item["targets"].append(symbols[u32(at + 4)])
    return sections


def inspect_object(path: Path) -> dict:
    sections = object_sections(path)
    code = [s for s in sections if s["flags"] & 0x20 and s["size"]]
    assert code and all(s["flags"] & 0x1000 for s in code), "unpackaged code"
    exports = set(re.findall(r'^public (fat_\w+)$', (ROOT / 'fat32.asm').read_text(), re.M))
    assert all(s["selection"] == (2 if s["name"].removeprefix('.text$') in exports else 1)
               for s in code), "unexpected public/private code selection"
    for item in code:
        assert item["name"].startswith(".text$")
        for kind in (".pdata", ".xdata", ".debug$S"):
            associated = [s for s in sections if s["name"] == kind
                          and s["association"] == item["number"]]
            assert len(associated) == 1, (item["name"], kind, associated)
            assert associated[0]["selection"] == 5
    for item in sections:
        if item["name"].startswith(".rdata$"):
            assert item["flags"] & 0x1000 and item["selection"] == 1
    return dict(code_bytes=sum(s["size"] for s in code),
                functions=[s["name"].split("$", 1)[1] for s in code],
                sections=sections)


def pe_sections(path: Path) -> dict:
    data = path.read_bytes()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    assert data[pe:pe + 4] == b"PE\0\0"
    count, optsize = struct.unpack_from("<H", data, pe + 6)[0], struct.unpack_from("<H", data, pe + 20)[0]
    optional = pe + 24
    assert struct.unpack_from("<H", data, optional)[0] == 0x20B
    assert struct.unpack_from("<II", data, optional + 112 + 8) == (0, 0), "unexpected imports"
    sections = {}
    for i in range(count):
        at = optional + optsize + i * 40
        name = data[at:at + 8].split(b"\0")[0].decode("ascii")
        size, rva, rawsize, raw = struct.unpack_from("<IIII", data, at + 8)
        sections[name] = dict(bytes=size, rva=rva, raw=raw, raw_bytes=rawsize)
    exception_rva, exception_size = struct.unpack_from("<II", data, optional + 112 + 3 * 8)
    ranges = []
    if exception_size:
        section = next(s for s in sections.values() if s["rva"] <= exception_rva < s["rva"] + s["bytes"])
        offset = section["raw"] + exception_rva - section["rva"]
        assert exception_size % 12 == 0
        for n in range(exception_size // 12):
            begin, end, unwind = struct.unpack_from("<III", data, offset + n * 12)
            assert sections[".text"]["rva"] <= begin < end <= sections[".text"]["rva"] + sections[".text"]["bytes"]
            assert any(s["rva"] <= unwind < s["rva"] + s["bytes"] for s in sections.values())
            ranges.append((begin, end))
        assert ranges == sorted(ranges) and len({r[0] for r in ranges}) == len(ranges)
    return dict(sections=sections, runtime_functions=len(ranges), file_bytes=len(data))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fasm-root", type=Path, required=True)
    parser.add_argument("--llvm-bin", type=Path, default=llvm_directory())
    parser.add_argument("--baseline-object", type=Path,
                        help="Optional preserved pre-COMDAT object for size comparison")
    args = parser.parse_args()
    if args.llvm_bin is None:
        parser.error('Supply --llvm-bin, set LLVM_BIN, or put LLVM tools on PATH')
    out = ROOT / "build/features"
    out.mkdir(parents=True, exist_ok=True)
    obj = inspect_object(ROOT / "fat32.obj")
    (out / "object.json").write_text(json.dumps(obj, indent=2), encoding="utf-8")
    linkers = {"msvc": shutil.which("link.exe"), "lld": str(args.llvm_bin / "lld-link.exe")}
    assert all(linkers.values()), "MSVC developer environment required"
    os.environ["INCLUDE"] = str(args.fasm_root / "include") + ";" + os.environ.get("INCLUDE", "")
    results = []
    baseline = []
    for profile, roots in PROFILES.items():
        source = out / f"{profile}.asm"
        source.write_text("include 'common/policy.g'\n" +
                          "\n".join("extrn " + name for name in roots) +
                          "\nsection '.rdata$roots' data readable comdat align 8\n" +
                          "feature_roots dq " + ",".join(roots) +
                          "\nsection '.text$probe' code readable executable comdat align 16\n" +
                          "public feature_probe\nproc feature_probe\n" +
                          " lea rax,[feature_roots]\n xor eax,eax\n ret\nendp\n", encoding="utf-8")
        probe = source.with_suffix(".obj")
        run([str(args.fasm_root / "fasmg.exe"), str(source), str(probe)])
        for compiler, linker in linkers.items():
            for storage in ("obj", "lib"):
                for debug in (False, True):
                    tag = f"{profile}-{compiler}-{storage}-{'debug' if debug else 'release'}"
                    image, mapfile = out / (tag + ".exe"), out / (tag + ".map")
                    command = [str(linker), "/nologo", "/machine:x64", "/subsystem:console",
                               "/entry:feature_probe", "/nodefaultlib", "/incremental:no",
                               "/opt:ref", "/opt:noicf", f"/out:{image}", f"/map:{mapfile}"]
                    if debug:
                        command += ["/debug:full", f"/pdb:{out / (tag + '.pdb')}"]
                    command += [str(probe), str(ROOT / ("fat32." + storage))]
                    run(command, out / (tag + ".txt"))
                    mapping = mapfile.read_text(encoding="utf-8", errors="replace")
                    # MSVC lists discarded contributions with zero length in
                    # the section table. Count only symbols with linked VAs.
                    symbol_names = re.findall(r"^\s+[0-9A-Fa-f]+:[0-9A-Fa-f]+\s+(\S+)\s+[0-9A-Fa-f]{16}\b", mapping, re.M)
                    kept = {name.split("$", 1)[-1] for name in symbol_names}
                    assert set(roots) <= kept, (tag, "missing root", set(roots) - kept)
                    if profile in ("mount", "free-space", "read", "shared-read", "locked-read", "checked-read", "adaptive-read", "diagnostics", "directory-check", "name-check", "ownership-check", "stream-read", "stream-map", "range-read"):
                        assert not (kept & WRITE_FUNCTIONS), (tag, "write path retained", kept & WRITE_FUNCTIONS)
                    if profile == "mount":
                        assert not ({"fat_read", "fat_dir_next", "fat_chain", "fat_get"} & kept)
                    if profile != "free-space":
                        assert "fat_count_free" not in kept, (tag, "free scan retained")
                    if profile != "shared-full":
                        assert "fat_handle_get_stamp" not in kept, (tag, "timestamp decoder retained")
                    if profile != "put-checked":
                        assert "fat_put_checked" not in kept, (tag, "checked update retained")
                    if profile == "put":
                        assert "fat_get" not in kept, (tag, "audit-only FAT read retained")
                    if profile == "put-checked":
                        assert "fat_put" not in kept, (tag, "checked update depends on replaced default")
                    format_functions = {name for name in obj["functions"] if name.startswith(("fat_format", "f_format_"))}
                    if profile not in ("format-plan", "format", "format-verified"):
                        assert not kept & format_functions, (tag, "formatter retained")
                    if profile != "format-verified":
                        assert "f_format_verify" not in kept, (tag, "format readback retained")
                    if profile == "format-plan":
                        assert not kept & WRITE_FUNCTIONS
                    diagnostic_functions = {name for name in obj["functions"]
                                            if name.startswith(("fat_check_", "f_check_")) or name in ("fat_read_checked", "fat_read_adaptive")}
                    if profile.startswith("policy"):
                        allowed={"fat_check_fresh","f_check_clear"}
                        if profile=="policy-adaptive":allowed|={"fat_check_file","fat_check_chain","f_check_init","f_check_clear"}
                        assert not kept & (diagnostic_functions-allowed)
                        diagnostic_functions-=allowed
                        assert not kept & WRITE_FUNCTIONS
                        if profile=="policy":assert "fat_mount" not in kept and "fat_read_at" not in kept
                    else:
                        assert not {name for name in kept if name.startswith(("fat_policy_","f_policy_"))}, (tag, "policy retained")
                    if profile.startswith("salvage-"):
                        assert not kept & (diagnostic_functions - {"f_check_init", "f_check_clear"})
                        diagnostic_functions -= {"f_check_init", "f_check_clear"}
                    if profile not in ("fat-view", "checked-read", "adaptive-read", "diagnostics", "directory-check", "name-check", "ownership-check"):
                        assert not (kept & diagnostic_functions), (tag, "optional diagnostics retained", kept & diagnostic_functions)
                    if profile not in ("fat-view", "boot-view"):
                        assert not ({"fat_view_boot", "f_boot_read", "fat_view_open", "fat_view_close"} & kept), (tag, "FAT view retained")
                    elif profile == "fat-view":
                        assert not ({"fat_view_boot", "f_boot_read"} & kept)
                        assert not kept & WRITE_FUNCTIONS and "fat_mount" not in kept
                    else:
                        assert not kept & WRITE_FUNCTIONS and "fat_view_open" not in kept
                    if not profile.startswith("salvage-"):
                        assert not ({"fat_salvage_plan", "fat_salvage_read"} & kept), (tag, "salvage retained")
                    else:
                        assert not kept & WRITE_FUNCTIONS and "fat_mount" not in kept
                        assert not ({"fat_check_chain", "fat_chain", "fat_check_file", "fat_read_at", "fat_check_ownership"} & kept)
                        if profile == "salvage-plan":
                            assert "fat_salvage_read" not in kept
                    if profile != "name-check":
                        assert not ({"fat_check_names", "f_check_name_pair"} & kept), (tag, "namespace scan retained")
                    if profile != "ownership-check":
                        assert not ({name for name in kept if name.startswith('f_check_owner') or name=='fat_check_ownership'}), (tag, 'ownership scan retained')
                    else:
                        assert not ({'fat_dir_next','fat_chain','fat_check_chain','fat_check_file','fat_check_reserved','fat_check_backup','fat_check_mirrors','fat_read_checked','fat_read_adaptive'} & kept)
                    if profile not in ("directory-check", "name-check"):
                        assert "fat_check_directory" not in kept, (tag, "directory scan retained")
                    else:
                        assert not ({"fat_check_chain", "fat_check_file", "fat_check_reserved", "fat_check_backup", "fat_check_mirrors", "fat_read_checked", "fat_read_adaptive"} & kept)
                    if profile in ("checked-read", "adaptive-read"):
                        assert not ({"fat_check_reserved", "fat_check_backup", "fat_check_mirrors"} & kept)
                    if profile != "adaptive-read":
                        assert "fat_read_adaptive" not in kept, (tag, "adaptive policy retained")
                    else:
                        assert "fat_read_checked" not in kept, (tag, "checked preflight retained")
                    commit_functions = {name for name in obj["functions"]
                                        if name.startswith(("fat_order_", "f_order_"))}
                    if profile not in ("ordered-write", "verified-write", "ordered-range"):
                        assert not (kept & commit_functions), (tag, "commit module retained", kept & commit_functions)
                    if profile != "verified-write":
                        assert "f_order_verify" not in kept, (tag, "readback policy retained")
                    stream_functions = {name for name in obj["functions"] if name.startswith(("fat_stream_", "f_stream_"))}
                    if profile not in ("stream-read", "stream-map", "range-read", "ordered-range"):
                        assert not (kept & stream_functions), (tag, "stream module retained", kept & stream_functions)
                    if profile != "stream-map":
                        assert "fat_stream_sector" not in kept, (tag, "sector mapping retained")
                    else:
                        assert "fat_stream_read" not in kept and "fat_stream_read_range" not in kept
                    if profile not in ("range-read", "ordered-range"):
                        assert "fat_stream_read_range" not in kept, (tag, "range reader retained")
                    if profile != "ordered-range":
                        assert "fat_order_read_range" not in kept, (tag, "ordered range adapter retained")
                    if profile in ("shared-read", "locked-read", "checked-read", "adaptive-read", "stream-read", "range-read"):
                        assert "fat_chain" not in kept, (tag, "unbounded preflight retained")
                    if profile == "append":
                        assert not ({"fat_remove", "fat_rename", "fat_resize", "fat_set_info"} & kept)
                    if profile.startswith("shared-") or profile in ("locked-read", "checked-read", "adaptive-read", "ordered-write", "verified-write", "stream-read", "range-read", "ordered-range"):
                        assert not ({"fat_read", "fat_write", "fat_resize", "fat_set_info", "fat_create",
                                     "fat_remove", "fat_rename", "f_validate", "f_file", "f_snapshot_done"} & kept)
                    if profile != "locked-read":
                        assert "fat_call_locked" not in kept
                    facts = pe_sections(image)
                    assert facts["sections"][".text"]["bytes"] <= CODE_LIMITS[profile], (tag, "code budget", facts)
                    # Every library PROC and the probe has one retained unwind record.
                    functions = kept & set(obj["functions"])
                    assert facts["runtime_functions"] == len(functions) + 1, (tag, facts["runtime_functions"], sorted(functions))
                    run([str(image)])
                    results.append(dict(profile=profile, linker=compiler, storage=storage,
                                        debug=debug, functions=sorted(functions), **facts))
        if args.baseline_object and profile in ("mount", "read", "append", "full"):
            image = out / (profile + "-baseline.exe")
            run([str(linkers["msvc"]), "/nologo", "/machine:x64", "/subsystem:console",
                 "/entry:feature_probe", "/nodefaultlib", "/incremental:no", "/opt:ref", "/opt:noicf",
                 f"/out:{image}", str(probe), str(args.baseline_object.resolve())])
            run([str(image)])
            baseline.append(dict(profile=profile, **pe_sections(image)))
    (out / "results.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
    if baseline:
        (out / "baseline.json").write_text(json.dumps(baseline, indent=2), encoding="utf-8")
    for profile in PROFILES:
        sample = next(r for r in results if r["profile"] == profile and r["linker"] == "lld" and not r["debug"])
        print(f"{profile}: {len(sample['functions'])} functions, {sample['sections']['.text']['bytes']} linked code bytes")
        if baseline and profile in ("mount", "read", "append", "full"):
            previous = next(r for r in baseline if r["profile"] == profile)
            print(f"  pre-COMDAT: {previous['sections']['.text']['bytes']} linked code bytes")
    print(f"PASS: {len(results)} links; imports, feature removal, COMDAT associations and unwind ranges")


if __name__ == "__main__":
    main()
