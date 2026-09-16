"""Generate all 31 representable Microsoft-tool cluster/sector combinations.

Run on a fresh local build directory. Optional fsck.fat is invoked read-only
through WSL; metadata is hashed before and after every independent check.
"""
from pathlib import Path
import argparse
import hashlib
import importlib.util
import json
import subprocess

spec = importlib.util.spec_from_file_location('formatted', Path(__file__).with_name('format-images.py'))
formatted = importlib.util.module_from_spec(spec)
spec.loader.exec_module(formatted)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--fsck', help='Linux path to fsck.fat; use a build directory visible to WSL')
    args = parser.parse_args()
    destination = args.directory.resolve()
    assert 'build' in destination.parts and not destination.exists()
    destination.mkdir(parents=True)
    results = []
    for bps in (512,1024,2048,4096):
        cb = bps
        while cb <= min(262144,bps*128):
            unlabeled = cb == bps
            path = destination/f'format-{bps}-{cb}.img'
            row = formatted.materialize(path,bps,cb,verified=cb==min(262144,bps*128),unlabeled=unlabeled)
            if args.fsck:
                def metadata_hash():
                    with path.open('rb') as image:
                        return hashlib.sha256(image.read((row['dataStart']+cb//bps)*bps)).hexdigest()
                before = metadata_hash()
                linux = subprocess.run(['wsl.exe','-d','Ubuntu','--exec','wslpath','-a','-u',str(path)],
                                       check=True,capture_output=True,text=True).stdout.strip()
                check = subprocess.run(['wsl.exe','-d','Ubuntu','--exec',args.fsck,'-n','-v',linux],
                                       capture_output=True,text=True)
                output = check.stdout+check.stderr
                path.with_suffix('.fsck.txt').write_text(output)
                assert check.returncode == 0, (path,check.returncode,output)
                assert before == metadata_hash(), 'non-repair oracle changed metadata'
                row.update(fsckExitCode=check.returncode, metadataSHA256=before)
            results.append(row)
            cb *= 2
    (destination/'manifest.json').write_text(json.dumps(results,indent=2)+'\n')
    print(f'PASS: {len(results)} library-formatted geometries, independent raw decoder'+(', non-repair fsck' if args.fsck else ''))


if __name__ == '__main__':
    main()
