"""Check packed home-space metadata and reject records outside its ABI bounds."""
from pathlib import Path
import argparse
import os
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def locals_by_proc(text):
    starts = list(re.finditer(r"DisplayName: ([^\r\n]+)", text))
    result = {}
    for index, match in enumerate(starts):
        end = starts[index + 1].start() if index + 1 < len(starts) else len(text)
        fields = re.findall(
            r"RegRelativeSym \{\s*Kind: S_REGREL32 .*?Offset: (0x[0-9A-Fa-f]+)"
            r"\s*Type: (.*?)\s*Register: RSP .*?VarName: ([^\r\n]+)",
            text[match.end():end], re.S)
        result[match[1]] = {name: (int(offset, 16), kind) for offset, kind, name in fields}
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fasm-root", type=Path, required=True)
    parser.add_argument("--llvm-bin", type=Path)
    args = parser.parse_args()
    readobj = str(args.llvm_bin / "llvm-readobj.exe") if args.llvm_bin else "llvm-readobj"

    def symbols(path):
        return locals_by_proc(subprocess.check_output(
            [readobj, "--codeview", str(path)], text=True, timeout=30))

    fields = symbols(ROOT / "build/win32/abi.obj")["abi_home_fields"]
    base = fields["a"][0]
    for index, name in enumerate("abcdefgh"):
        assert fields[name] == (base + index * 4, "unsigned (0x75)"), (name, fields[name])
    assert fields["fifth"] == (base + 32, "unsigned __int64 (0x77)")
    library = symbols(ROOT / "fat32.obj")
    assert library["f_validate"]["offset"][1] == "unsigned (0x75)"
    assert library["f_validate"]["location"][1] == "unsigned __int64 (0x77)"

    # Each probe assembles a used procedure: unused PROC bodies may be omitted.
    # Record-size growth and an invalid offset must fail before producing an object.
    destination = ROOT / "build/home-check"
    destination.mkdir(parents=True, exist_ok=True)
    environment = os.environ.copy()
    environment["INCLUDE"] = str(args.fasm_root / "include") + ";" + environment.get("INCLUDE", "")
    for name, record, offset in (("negative", "FatTransfer", -1),
                                 ("overrun", "FatTransfer", 9),
                                 ("oversized", "FatHandle", 0)):
        source = destination / (name + ".asm")
        source.write_text("include '../../common/policy.g'\n"
                          "include '../../fat32.inc'\n"
                          "public home_rejected\nproc home_rejected\n"
                          f"    home_struct request, {record}, {offset}\n"
                          "body:\n    ret\nendp\n", encoding="utf-8")
        run = subprocess.run([str(args.fasm_root / "fasmg.exe"), "-e", "1",
                              str(source), str(source.with_suffix(".obj"))],
                             env=environment, capture_output=True, text=True, timeout=30)
        assert run.returncode and "assertion failed" in run.stdout + run.stderr, (name, run)
    print("PASS: eight DWORD debug types/offsets; fifth argument; three rejected home layouts")


if __name__ == "__main__":
    main()
