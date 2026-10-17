"""Validate release artifacts and Nexus destinations; uploading stays in the official action."""
from pathlib import Path, PurePosixPath
import argparse
import fnmatch
import hashlib
import json
import os
import re
import subprocess
import sys
import urllib.error
import urllib.request
import zipfile
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from release_notes import section, plain, bbcode
REPOSITORY = 'DooDesch-Mods/MMORPGTycoon2-CardVault'
API = 'https://api.nexusmods.com/v3'


def targets():
    result = json.loads((ROOT / 'integrations/nexus/targets.json').read_text(encoding='utf-8'))
    for key, value in result.items():
        if int(value['mod_uid']) != (value['game_id'] << 32) + value['page_id']:
            raise ValueError(f'{key}: Unique Mod ID does not match game/page IDs')
        if not re.fullmatch(r'[1-9][0-9]*', value['file_id']):
            raise ValueError(f'{key}: invalid Nexus file ID')
    return result


def version_key(value):
    match = re.fullmatch(r'(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-([0-9A-Za-z.-]+))?(?:\+[0-9A-Za-z.-]+)?', value)
    if not match:
        raise ValueError('Expected SemVer version: ' + value)
    suffix = match[4]
    parts = tuple((0, int(part)) if part.isdigit() else (1, part) for part in suffix.split('.')) if suffix else ()
    return tuple(int(match[i]) for i in (1, 2, 3)) + (suffix is None, parts)


def archive_version(path, product):
    with zipfile.ZipFile(path) as archive:
        names = archive.namelist()
        if len(names) != len(set(names)):
            raise ValueError('Duplicate ZIP entries: ' + path.name)
        for name in names:
            normalized = PurePosixPath(name.replace('\\', '/'))
            if normalized.is_absolute() or '..' in normalized.parts or ':' in name:
                raise ValueError('Unsafe ZIP path: ' + name)
        if archive.testzip():
            raise ValueError('ZIP CRC failed: ' + path.name)
        from package import FILES
        expected = {'doodesch.cardvault/'+name for name in FILES}
        if set(names) != expected:
            raise ValueError('Card Vault player inventory differs')
        metadata = json.loads(archive.read('doodesch.cardvault/mod.json'))
        if metadata['id'] != 'doodesch.cardvault' or metadata['entry'] != 'MT2CardVault.dll':
            raise ValueError('Wrong Card Vault manifest')
        version_key(metadata['version'])
        return metadata['version']


def checksum_for(text, filename):
    matches = []
    for line in text.splitlines():
        match = re.fullmatch(r'([a-fA-F0-9]{64})\s+\*?(.+)', line)
        if not match:
            raise ValueError('Malformed SHA256SUMS.txt line')
        if match[2] == filename:
            matches.append(match[1].lower())
    if len(matches) != 1:
        raise ValueError('Expected exactly one checksum for ' + filename)
    return matches[0]


def gh(*args):
    result = subprocess.run(['gh', *args, '--repo', REPOSITORY], capture_output=True, text=True, timeout=600)
    if result.returncode:
        cause = result.stderr.strip()
        for name in ('GH_TOKEN', 'GITHUB_TOKEN', 'NEXUSMODS_API_KEY', 'DISCORD_RELEASE_WEBHOOK'):
            if value := os.environ.get(name):cause = cause.replace(value, '[redacted]')
        raise RuntimeError(f'GitHub release {args[1]} failed (exit {result.returncode}): {cause[:2000]}')
    return result.stdout


def fetch_release(tag, product, directory, optional=False):
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]{0,127}', tag):
        raise ValueError('Invalid release tag')
    metadata = json.loads(gh('release', 'view', tag, '--json', 'tagName,isDraft,assets,url'))
    if metadata['isDraft'] or metadata['tagName'] != tag:
        raise ValueError('Expected a published GitHub release')
    selected = targets()[product]
    matches = [row for row in metadata['assets'] if fnmatch.fnmatchcase(row['name'], selected['asset_pattern'])]
    if not matches and optional:
        return None
    if len(matches) != 1:
        raise ValueError(f'{product}: expected one release archive, found {len(matches)}')
    asset = matches[0]
    if not any(row['name'] == 'SHA256SUMS.txt' for row in metadata['assets']):
        raise ValueError('Release is missing SHA256SUMS.txt')
    directory.mkdir(parents=True, exist_ok=True)
    gh('release', 'download', tag, '--dir', str(directory), '--pattern', asset['name'], '--pattern', 'SHA256SUMS.txt')
    path = directory / asset['name']
    expected = checksum_for((directory / 'SHA256SUMS.txt').read_text(encoding='utf-8'), path.name)
    actual = hashlib.sha256(path.read_bytes()).hexdigest()
    if actual != expected or path.stat().st_size != asset['size']:
        raise ValueError('GitHub release asset integrity differs: ' + path.name)
    version = archive_version(path, product)
    expected_name = f'MT2CardVault-{version}-windows-x64.zip'
    if path.name != expected_name:
        raise ValueError('Archive filename differs from embedded product version')
    return {'product': product, 'tag': tag, 'release_url': metadata['url'], 'version': version,
            'filename': str(path), 'sha256': actual, 'bytes': path.stat().st_size}


def api_get(endpoint):
    key = os.environ.get('NEXUSMODS_API_KEY', '').strip()
    if not key:
        raise ValueError('NEXUSMODS_API_KEY is missing; configure the repository or organization secret')
    request = urllib.request.Request(API + endpoint, headers={'apikey': key, 'Accept': 'application/json', 'User-Agent': 'MT2-ModFramework-CI'})
    try:
        with urllib.request.urlopen(request, timeout=45) as response:
            return json.load(response)['data']
    except urllib.error.HTTPError as error:
        # API bodies can contain account details; the endpoint and status identify this failure.
        raise RuntimeError(f'Nexus GET {endpoint} failed: HTTP {error.code}') from None
    except urllib.error.URLError as error:
        raise RuntimeError(f'Nexus GET {endpoint} failed: {error.reason}') from None


def check_destination(target, version, get=api_get):
    mod = get(f"/games/{target['domain']}/mods/{target['page_id']}")
    if str(mod['id']) != target['mod_uid'] or str(mod['game_id']) != str(target['game_id']) or str(mod['game_scoped_id']) != str(target['page_id']):
        raise ValueError('Nexus page resolved to a different mod/game')
    files = get(f"/mods/{target['mod_uid']}/files")['mod_files']
    if not any(str(row['id']) == target['file_id'] for row in files):
        raise ValueError('Nexus file ID does not belong to the configured mod')
    versions = get(f"/mod-files/{target['file_id']}/versions")['versions']
    if not versions:
        raise ValueError('Nexus file has no existing version; check its file ID')
    for row in versions:
        if str(row['file']['id']) != target['file_id']:
            raise ValueError('Nexus returned a version from another file')
    if any(row['version'] == version for row in versions):
        return 'already-present'
    candidate = version_key(version)
    for row in versions:
        if version_key(row['version']) >= candidate:
            raise ValueError('Release version is older than a version already on Nexus: ' + row['version'])
    return 'ready'


def outputs(values):
    if destination := os.environ.get('GITHUB_OUTPUT'):
        with open(destination, 'a', encoding='utf-8') as stream:
            for name, value in values.items():
                delimiter = 'output_' + uuid.uuid4().hex
                stream.write(f'{name}<<{delimiter}\n{value}\n{delimiter}\n')


def prepare(args):
    target = targets()[args.product]
    for variable, expected in [('NEXUS_FILE_ID', target['file_id']), ('NEXUS_MOD_ID', target['mod_uid'])]:
        if os.environ.get(variable) != expected:
            raise ValueError(variable + ' is missing or differs from integrations/nexus/targets.json')
    directory = Path(args.directory)
    plan = fetch_release(args.tag, args.product, directory, args.optional)
    if plan is None:
        plan = {'product': args.product, 'tag': args.tag, 'status': 'not-in-release'}
    else:
        plan.update({'status': check_destination(target, plan['version']), 'file_id': target['file_id'],
                     'mod_uid': target['mod_uid'], 'nexus_url': f"https://www.nexusmods.com/{target['domain']}/mods/{target['page_id']}"})
        notes = section((ROOT / 'CHANGELOG.md').read_text(encoding='utf-8'), args.product, plan['version'])
        plan.update(notes=notes, notes_plain=plain(notes), notes_bbcode=bbcode(notes))
    directory.mkdir(parents=True, exist_ok=True)
    (directory / 'plan.json').write_text(json.dumps(plan, indent=2) + '\n', encoding='utf-8')
    result = {'status': plan['status']}
    if 'version' in plan:
        result.update({name: plan[name] for name in ('filename', 'version', 'file_id', 'mod_uid')})
        result.update({'display_name': target['name'], 'description': plan['notes_bbcode']})
    outputs(result)
    print(json.dumps(plan))
    if summary := os.environ.get('GITHUB_STEP_SUMMARY'):
        with open(summary, 'a', encoding='utf-8') as stream:
            stream.write(f"### {target['name']}\n\nStatus: **{plan['status']}**\n\n")
            if 'version' in plan:
                stream.write(f"Version: {plan['version']}\n\nFile ID: {target['file_id']}\n\nSHA-256: `{plan['sha256']}`\n\n")
                if plan['status'] == 'already-present':
                    stream.write('This version exists on Nexus. No upload is performed. Remote file bytes were not compared.\n\n')


def verify(args):
    directory = Path(args.directory);plan = json.loads((directory / 'plan.json').read_text())
    row = api_get('/mod-file-versions/' + args.version_id)
    if str(row['id']) != args.version_id or str(row['file']['id']) != plan['file_id'] or row['version'] != plan['version'] or row['category'] != 'main':
        raise ValueError('Nexus upload readback differs from the release plan')
    receipt = {**plan, 'status': 'confirmed', 'nexus_version_id': args.version_id}
    (directory / 'uploaded.json').write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(receipt))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    plan = commands.add_parser('matrix');plan.add_argument('--target', choices=['all', 'cardvault'], default='all')
    command = commands.add_parser('prepare')
    command.add_argument('--product', choices=['cardvault'], required=True)
    command.add_argument('--tag', required=True);command.add_argument('--directory', required=True)
    command.add_argument('--optional', action='store_true')
    command = commands.add_parser('verify')
    command.add_argument('--directory', required=True);command.add_argument('--version-id', required=True)
    args = parser.parse_args()
    if args.command == 'matrix':
        selected = [{'product': key, 'file_variable': value['file_variable'], 'mod_variable': value['mod_variable']} for key, value in targets().items() if args.target in ('all', key)]
        value = json.dumps({'include': selected}, separators=(',', ':'));outputs({'matrix': value});print(value)
    elif args.command == 'prepare':
        prepare(args)
    else:
        verify(args)


if __name__ == '__main__':
    try:
        main()
    except (ValueError, KeyError, OSError, RuntimeError, zipfile.BadZipFile, subprocess.SubprocessError) as error:
        # subprocess output is not dumped: release/API errors must not expose authenticated requests.
        print('error: ' + str(error), file=sys.stderr);sys.exit(1)
