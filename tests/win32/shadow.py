"""Exercise public replacement selection, including calls from inside the library."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shutil

from features import ROOT, inspect_object, object_sections, pe_sections, run, llvm_directory


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fasm-root', type=Path, required=True)
    parser.add_argument('--llvm-bin', type=Path, default=llvm_directory())
    args = parser.parse_args()
    if args.llvm_bin is None:
        parser.error('Supply --llvm-bin, set LLVM_BIN, or put LLVM tools on PATH')
    out = ROOT / 'build/shadow'
    out.mkdir(parents=True, exist_ok=True)
    os.environ['INCLUDE'] = str(args.fasm_root / 'include') + ';' + os.environ.get('INCLUDE', '')
    linkers = {'msvc': shutil.which('link.exe'), 'lld': str(args.llvm_bin / 'lld-link.exe')}
    assert all(linkers.values()), 'MSVC developer environment required'
    prefix = "include '../../common/policy.g'\ninclude '../../fat32.inc'\n"
    replacement = out / 'replacement.asm'
    replacement.write_text(prefix + """
public fat_get
section '.text$fat_get' code readable executable comdat any align 16
proc fat_get uses rbx rsi
    mov ebx, 1234
    mov esi, ebx
    mov eax, esi
    ret
endp
""")
    probe = out / 'probe.asm'
    probe.write_text(prefix + """
extrn fat_get
extrn fat_chain
public shadow_probe
section '.data' data readable writeable align 8
identity FatIdentity
output dq 0
section '.text$shadow_probe' code readable executable comdat align 16
proc shadow_probe
    ; An intentionally invalid mounted identity gives F_CORRUPT in the default
    ; fat_get without provider I/O. The chain bounds still admit cluster 2.
    mov dword [identity.cluster_count], 1
    fastcall fat_get, addr identity, 2, addr output
    cmp eax, EXPECTED
    jne .failed
    ; Force extraction of the library even when fat_get is supplied elsewhere,
    ; and prove its internal reference reaches the same selected definition.
    fastcall fat_chain, addr identity, 2, addr output
    cmp eax, EXPECTED
    jne .failed
    xor eax, eax
    ret
.failed:
    mov eax, 1
    ret
endp
""")
    fasm = str(args.fasm_root / 'fasmg.exe')
    run([fasm, '-e', '5', str(replacement), str(replacement.with_suffix('.obj'))])
    inspect_object(replacement.with_suffix('.obj'))
    run(['lib.exe', '/nologo', f'/out:{out / "replacement.lib"}', str(replacement.with_suffix('.obj'))])
    probes = {}
    for owner, expected in (('replacement', '1234'), ('default', 'F_CORRUPT')):
        obj = out / f'probe-{owner}.obj'
        run([fasm, '-e', '5', '-i', f'EXPECTED := {expected}', str(probe), str(obj)])
        probes[owner] = obj

    results = []
    for linker_name, linker in linkers.items():
        for debug in (False, True):
            for replacement_storage, default_storage in (('obj', 'obj'), ('lib', 'lib'),
                                                          ('obj', 'lib'), ('lib', 'obj')):
                storage = replacement_storage + '-' + default_storage
                for first in ('replacement', 'default'):
                    # In mixed lists explicit objects are selected before
                    # lazy archive extraction, independent of textual order.
                    owner = first if replacement_storage == default_storage else (
                        'replacement' if replacement_storage == 'obj' else 'default')
                    tag = f'{linker_name}-{storage}-{first}-' + ('debug' if debug else 'release')
                    inputs = [out / f'replacement.{replacement_storage}', ROOT / f'fat32.{default_storage}']
                    if first == 'default':
                        inputs.reverse()
                    image, mapping = out / (tag + '.exe'), out / (tag + '.map')
                    command = [str(linker), '/nologo', '/machine:x64', '/subsystem:console',
                               '/entry:shadow_probe', '/nodefaultlib', '/incremental:no',
                               '/opt:ref', '/opt:noicf', f'/out:{image}', f'/map:{mapping}']
                    if debug:
                        command += ['/debug:full', f'/pdb:{out / (tag + ".pdb")}']
                    run(command + [str(probes[owner]), *map(str, inputs)], out / (tag + '.txt'))
                    run([str(image)])
                    facts = pe_sections(image)
                    # Exactly one selected fat_get, one chain/probe, and the
                    # default-only f_read_sector when the default wins.
                    assert facts['runtime_functions'] == (3 if owner == 'replacement' else 4), (tag, facts)
                    rows = re.findall(r'^\s+[\da-fA-F]+:[\da-fA-F]+\s+fat_get\s+[^\n]+',
                                      mapping.read_text(errors='replace'), re.M)
                    assert len(rows) == 1 and ('replacement' if owner == 'replacement' else 'fat32') in rows[0], (tag, rows)
                    results.append(dict(linker=linker_name, storage=storage, debug=debug, first=first,
                                        selected=owner, internalCall=True, **facts))
    # Matching names alone must not silently enable replacement of internals.
    default_sections = object_sections(ROOT / 'fat32.obj')
    assert next(s for s in default_sections if s['name'] == '.text$f_read_sector')['selection'] == 1
    (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    print(f'PASS: {len(results)} shadow links; both input orders, internal calls, selected ownership and unwind')


if __name__ == '__main__':
    main()
