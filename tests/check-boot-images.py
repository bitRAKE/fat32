"""Independent candidate-BPB fixtures; disk files only, no repair or devices."""
from pathlib import Path
import argparse,hashlib,importlib.util,json,struct,subprocess
from images import ROOT,GEOMETRIES,build

spec=importlib.util.spec_from_file_location('check_images',ROOT/'tests/check-images.py')
oracle=importlib.util.module_from_spec(spec);spec.loader.exec_module(oracle)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();destination=args.output.resolve()
    if (ROOT/'build').resolve() not in destination.parents:
        parser.error('output must be a fresh subdirectory under build/')
    destination.mkdir(parents=True,exist_ok=False)
    results=[];manifest=[]
    for bps,spc in GEOMETRIES:
        for fault in ('primary-signature','primary-io','both-signatures','backup-conflict'):
            directory=destination/f'{bps}-{bps*spc}-{fault}';directory.mkdir()
            item=build(directory,bps,spc,'clean');image=directory/item['image']
            with image.open('r+b') as output:
                if fault in ('primary-signature','both-signatures'):
                    output.seek(510);output.write(b'\x00')
                if fault=='both-signatures':
                    output.seek(6*bps+510);output.write(b'\x00')
                if fault=='backup-conflict':
                    output.seek(6*bps+67);serial=struct.unpack('<I',output.read(4))[0]
                    output.seek(6*bps+67);output.write(struct.pack('<I',serial^1))
            with image.open('rb') as source:
                primary=source.read(bps);source.seek(6*bps);backup=source.read(bps)
                extents=[]
                for extent in item['extents']:
                    source.seek(extent['offset']);content=source.read(extent['length'])
                    extents.append(dict(offset=extent['offset'],length=extent['length'],sha256=hashlib.sha256(content).hexdigest()))
            observed=json.loads(subprocess.check_output([str(ROOT / 'build/win32/imagecheck.exe'),str(image),str(bps),str(bps*spc),'clean',
                                                        'boot-io' if fault=='primary-io' else 'boot'],text=True,timeout=30))
            expected_primary=2 if fault=='primary-io' else 0 if fault=='backup-conflict' else 3
            assert observed['primary_status']==expected_primary,(image,observed)
            assert observed['candidate_status']==(3 if fault=='both-signatures' else 0),(image,observed)
            result=dict(image=str(image.relative_to(destination)),fault=fault,observation=observed)
            if fault!='both-signatures':
                assert backup[510:512]==b'\x55\xaa'
                assert observed['cluster_bytes']==struct.unpack_from('<H',backup,11)[0]*backup[13]==bps*spc
                assert observed['serial']==struct.unpack_from('<I',backup,67)[0]
                assert observed['backup_status']==(2 if fault=='primary-io' else 4)
                assert observed['backup_issue']==(10 if fault=='primary-io' else 8 if fault=='backup-conflict' else 7)
                assert observed['backup_sector']==(6 if fault=='backup-conflict' else 0)
                result['oracle']=oracle.check_fat_views(image,item,observed['observations'],boot_sector=6)
            with image.open('rb') as source:
                assert source.read(bps)==primary;source.seek(6*bps);assert source.read(bps)==backup
                for extent in extents:
                    source.seek(extent['offset'])
                    assert hashlib.sha256(source.read(extent['length'])).hexdigest()==extent['sha256']
            result['materializedBytesUnchanged']=True
            manifest.append(dict(image=result['image'],fault=fault,sectorBytes=bps,clusterBytes=bps*spc,
                                 bytes=image.stat().st_size,primarySHA256=hashlib.sha256(primary).hexdigest(),
                                 backupSHA256=hashlib.sha256(backup).hexdigest(),extents=extents))
            results.append(result);print('PASS',result['image'])
    (destination/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    (destination/'observations.json').write_text(json.dumps(results,indent=2)+'\n')
    checked=sum(row.get('oracle',{}).get('filesChecked',0) for row in results)
    assert len(results)==16 and checked==120
    print(f'PASS: {len(results)} BPB fixtures; {checked} independently decoded file/copy and salvage results; no repair')

if __name__=='__main__':main()
