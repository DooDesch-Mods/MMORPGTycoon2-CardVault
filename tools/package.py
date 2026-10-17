"""Build an allowlisted player ZIP and retain its exact inventory."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]
FILES = ['MT2CardVault.dll', 'mod.json', 'README.md', 'CHANGELOG.md', 'LICENSE',
         'THIRD_PARTY_NOTICES.md', 'third_party/nlohmann-LICENSE.MIT']


def package(build, output, checker=None):
    manifest = json.loads((ROOT/'mod.json').read_text(encoding='utf-8'))
    version = manifest['version']
    if not re.fullmatch(r'(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)', version):
        raise ValueError('Expected a release SemVer in mod.json')
    source = (ROOT/'src/plugin.cpp').read_text(encoding='utf-8')
    cmake = (ROOT/'CMakeLists.txt').read_text(encoding='utf-8')
    if f'info->version="{version}"' not in source or f'VERSION {version} LANGUAGES' not in cmake:
        raise ValueError('Native query, CMake and manifest versions differ')
    from release_notes import section
    section((ROOT/'CHANGELOG.md').read_text(encoding='utf-8'), 'cardvault', version)
    payload = {}
    for name in FILES:
        path = build/'package/doodesch.cardvault/Release'/name if name.endswith('.dll') else ROOT/name
        if not path.is_file():
            raise ValueError('Missing release input: '+name)
        data = path.read_bytes()
        if name.endswith('.dll') and data[:2] != b'MZ':
            raise ValueError('Expected a Windows DLL')
        if name == 'README.md':
            # The archive carries a complete player guide; build documentation stays in the repository.
            text = data.decode('utf-8').split('## Build\n',1)[0]
            text = re.sub(r'!\[[^\]]*\]\([^)]*\)\n', '', text)
            text = text.replace('(CHANGELOG.md)', '(CHANGELOG.md)')
            data = text.encode('utf-8')
        payload['doodesch.cardvault/'+name] = data
    output.mkdir(parents=True, exist_ok=True)
    archive = output/f'MT2CardVault-{version}-windows-x64.zip'
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as stream:
        for name, data in sorted(payload.items()):
            info = zipfile.ZipInfo(name, (2026,1,1,0,0,0))
            stream.writestr(info, data, compress_type=zipfile.ZIP_DEFLATED)
    with zipfile.ZipFile(archive) as stream:
        if stream.testzip() or set(stream.namelist()) != set(payload):
            raise ValueError('ZIP inventory or CRC differs')
    if checker:
        # Validate an extracted folder because the native checker accepts directories.
        staging = output/'validation'
        with zipfile.ZipFile(archive) as stream:
            stream.extractall(staging)
        subprocess.run([str(checker.resolve()), 'check', str((staging/'doodesch.cardvault').resolve())], check=True)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    (output/'SHA256SUMS.txt').write_text(digest+'  '+archive.name+'\n', encoding='utf-8')
    receipt = {'version':version, 'archive':archive.name, 'bytes':archive.stat().st_size, 'sha256':digest,
               'files':{name:hashlib.sha256(data).hexdigest() for name,data in payload.items()}}
    (output/'package.json').write_text(json.dumps(receipt,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({key:receipt[key] for key in ('version','archive','bytes','sha256')}))
    return receipt


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=ROOT/'build')
    parser.add_argument('--output', type=Path, default=ROOT/'dist')
    parser.add_argument('--checker', type=Path)
    args = parser.parse_args()
    package(args.build, args.output, args.checker)
