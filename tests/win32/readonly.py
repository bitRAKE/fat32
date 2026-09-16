"""Prove force-excluded mutation APIs with a deliberately writable provider."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil

from features import ROOT, WRITE_FUNCTIONS, inspect_object, pe_sections, run, llvm_directory

BLOCKED = set('''fat_format fat_format_verified fat_put fat_put_checked fat_new fat_write_at fat_write_next fat_handle_resize
fat_handle_set_info fat_handle_rename fat_unlink fat_write fat_resize fat_set_info
fat_create fat_remove fat_rename fat_order_init fat_order_commit fat_order_commit_verified'''.split())
# New public APIs must be classified explicitly before this profile can claim
# that all mutation is excluded. Do not infer "reader" from an unfamiliar name.
ALLOWED = set('''fat_policy_init fat_policy_note fat_policy_call fat_policy_close fat_view_boot fat_salvage_plan fat_salvage_read fat_view_open fat_view_close fat_format_plan fat_mount fat_invalidate fat_get fat_count_free fat_chain
fat_volume_init fat_volume_close fat_root fat_open fat_close fat_handle_info
fat_handle_get_stamp fat_seek fat_read_at fat_read_next fat_iter_open fat_iter_next
fat_iter_close fat_call_locked fat_stream_open fat_stream_close fat_stream_read
fat_stream_read_range fat_stream_sector fat_check_chain fat_check_file
fat_check_fresh fat_check_reserved fat_check_backup fat_check_mirrors fat_read_checked
fat_read_adaptive fat_order_discard fat_order_read_range fat_dir_open fat_dir_next
fat_lookup fat_read fat_check_directory fat_check_names fat_check_ownership'''.split())
FORBIDDEN = (WRITE_FUNCTIONS - BLOCKED) | set('''f_format_boot f_format_zero f_format_label f_format_verify f_mutable f_version_room
f_file_changed f_namespace_changed f_order_write f_order_begin f_order_end
f_order_barrier f_order_load f_order_flush f_order_send f_order_verify
f_order_reserved f_order_run'''.split())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fasm-root', type=Path, required=True)
    parser.add_argument('--llvm-bin', type=Path, default=llvm_directory())
    args = parser.parse_args()
    if args.llvm_bin is None:
        parser.error('Supply --llvm-bin, set LLVM_BIN, or put LLVM tools on PATH')
    out = ROOT / 'build/readonly'
    out.mkdir(parents=True, exist_ok=True)
    os.environ['INCLUDE'] = str(args.fasm_root/'include') + ';' + os.environ.get('INCLUDE', '')
    exports = set(re.findall(r'^public (fat_\w+)$',(ROOT/'fat32.asm').read_text(),re.M))
    assert exports == BLOCKED | ALLOWED and not BLOCKED & ALLOWED, 'Classify changed public API'
    stubs = ROOT/'example/readonly/readonly.obj'
    assert set(re.findall(r'^public (fat_\w+)$',stubs.with_suffix('.asm').read_text(),re.M)) == BLOCKED
    inspect_object(stubs)
    source = out/'roots.asm'
    source.write_text("include '../../common/policy.g'\n" +
                      '\n'.join('extrn '+name for name in sorted(exports)) +
                      "\npublic readonly_roots\nsection '.rdata$roots' data readable comdat align 8\n" +
                      'readonly_roots dq '+','.join(sorted(exports))+'\n')
    run([str(args.fasm_root/'fasmg.exe'),str(source),str(source.with_suffix('.obj'))])
    probes = {}
    for mode in ('readonly','control'):
        probe = out/('probe-'+mode+'.obj')
        command = [str(args.llvm_bin/'clang.exe'),'--target=x86_64-pc-windows-msvc',
                   '-std=c17','-Wall','-Wextra','-Werror','-O2','-ffreestanding',
                   '-fno-builtin','-fno-stack-protector','-ffunction-sections','-fdata-sections',
                   '-g','-gcodeview','-c',str(ROOT/'example/readonly/test.c'),'-o',str(probe)]
        if mode == 'control': command += ['-DREADONLY_CONTROL=1']
        run(command)
        probes[mode] = probe
    linkers = {'msvc':shutil.which('link.exe'),'lld':str(args.llvm_bin/'lld-link.exe')}
    assert all(linkers.values()), 'MSVC developer environment required'
    results = []
    for linker_name, linker in linkers.items():
        for storage in ('obj','lib'):
            for debug in (False,True):
                for mode in ('readonly','control'):
                    tag=f'{linker_name}-{storage}-{mode}-'+('debug' if debug else 'release')
                    image,mapping=out/(tag+'.exe'),out/(tag+'.map')
                    command=[str(linker),'/nologo','/machine:x64','/subsystem:console',
                             '/entry:readonly_probe','/nodefaultlib','/incremental:no',
                             '/opt:ref','/opt:noicf',f'/out:{image}',f'/map:{mapping}']
                    if debug: command += ['/debug:full',f'/pdb:{out/(tag+".pdb")}']
                    command += [str(probes[mode]),str(source.with_suffix('.obj'))]
                    if mode == 'readonly': command += [str(stubs)]
                    command += [str(ROOT/f'fat32.{storage}')]
                    run(command,out/(tag+'.txt'))
                    run([str(image)])
                    facts=pe_sections(image)
                    rows=re.findall(r'^\s+[\da-fA-F]+:[\da-fA-F]+\s+(\S+)\s+[\da-fA-F]{16}\b([^\n]*)',
                                    mapping.read_text(errors='replace'),re.M)
                    kept={name.split('$',1)[-1] for name,_ in rows}
                    assert exports <= kept, (tag,'all public entries must remain callable')
                    if mode == 'readonly':
                        assert not kept & FORBIDDEN,(tag,'mutation implementation retained',kept & FORBIDDEN)
                        for name in BLOCKED:
                            definitions=[owner for symbol,owner in rows if symbol==name]
                            assert len(definitions)==1 and 'readonly.obj' in definitions[0],(tag,name,definitions)
                    else:
                        assert {'f_write_sector','f_order_send','f_order_write'} <= kept,(tag,'missing write control')
                    results.append(dict(linker=linker_name,storage=storage,debug=debug,mode=mode,
                                        allExportsRooted=len(exports),blockedEntries=len(BLOCKED),**facts))
    (out/'results.json').write_text(json.dumps(results,indent=2)+'\n')
    print(f'PASS: {len(results)} links; {len(BLOCKED)} denied APIs, all {len(exports)} exports rooted, no mutation helpers, writable controls')


if __name__=='__main__':
    main()
