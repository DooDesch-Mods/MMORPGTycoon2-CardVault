"""Publish only a tag that matches the main-branch manifest and verified package."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import subprocess
from release_notes import section

ROOT = Path(__file__).resolve().parents[1]


def run(*args):
    return subprocess.check_output(args, cwd=ROOT, text=True)


def preflight(tag):
    if not re.fullmatch(r'v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)', tag):
        raise ValueError('Release tag must be vX.Y.Z')
    version = json.loads((ROOT/'mod.json').read_text())['version']
    if tag != 'v'+version:
        raise ValueError('Release tag differs from mod manifest')
    subprocess.run(['git', '-C', str(ROOT), 'fetch', '--no-tags', 'origin', 'main'], check=True)
    subprocess.run(['git', '-C', str(ROOT), 'merge-base', '--is-ancestor', 'HEAD', 'origin/main'], check=True)
    return section((ROOT/'CHANGELOG.md').read_text(encoding='utf-8'), 'cardvault', version)


def publish(tag):
    notes = preflight(tag)
    receipt = json.loads((ROOT/'dist/package.json').read_text())
    archive = ROOT/'dist'/receipt['archive']
    if hashlib.sha256(archive.read_bytes()).hexdigest() != receipt['sha256']:
        raise ValueError('Release archive changed after validation')
    notes_path = ROOT/'dist/release-notes.md'
    notes_path.write_text(notes+'\n\nRequires [MT2 Mod Framework 0.1.4 or newer](https://www.nexusmods.com/mmorpgtycoon2/mods/25). Install the ZIP through Vortex or follow the included guide.\n', encoding='utf-8')
    # Refuse a second creation. A failed/unknown create is reconciled by inspecting this tag's release.
    subprocess.run(['gh', 'release', 'create', tag, str(archive), str(ROOT/'dist/SHA256SUMS.txt'),
                    '--verify-tag', '--title', 'MT2 Card Vault '+receipt['version'], '--notes-file', str(notes_path),
                    '--repo', 'DooDesch-Mods/MMORPGTycoon2-CardVault'], cwd=ROOT, check=True)
    remote = json.loads(run('gh', 'release', 'view', tag, '--repo', 'DooDesch-Mods/MMORPGTycoon2-CardVault', '--json', 'assets,url'))
    asset = next(row for row in remote['assets'] if row['name'] == archive.name)
    if asset['size'] != archive.stat().st_size or asset.get('digest') != 'sha256:'+receipt['sha256']:
        raise ValueError('GitHub asset acknowledgement differs; inspect before retrying')
    print('GitHub release confirmed: '+remote['url'])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=['preflight', 'publish'])
    parser.add_argument('--tag', required=True)
    args = parser.parse_args()
    if args.command == 'preflight':
        print(preflight(args.tag))
    else:
        publish(args.tag)
