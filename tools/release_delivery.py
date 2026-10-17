"""Complete Nexus changelogs and acknowledged Discord release announcements."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request

from nexus_release import API, REPOSITORY, gh, targets


def request_json(url, method='GET', body=None, headers=None, service='Nexus'):
    data = None if body is None else json.dumps(body).encode('utf-8')
    request = urllib.request.Request(url, data=data, method=method,
        headers={'Accept': 'application/json', 'Content-Type': 'application/json',
                 'User-Agent': 'MT2-ModFramework-CI', **(headers or {})})
    try:
        with urllib.request.urlopen(request, timeout=45) as response:
            raw = response.read()
            return json.loads(raw) if raw else None
    except urllib.error.HTTPError as error:
        raise RuntimeError(f'{service} {method} failed: HTTP {error.code}; inspect destination before retrying a write') from None
    except (urllib.error.URLError, TimeoutError, json.JSONDecodeError) as error:
        # Webhook URLs contain credentials. Never include the URL or response body in diagnostics.
        raise RuntimeError(f'{service} {method} acknowledgement failed ({type(error).__name__}); inspect destination before retrying a write') from None


def normalized_notes(value):
    lines = [line for item in value for line in item.splitlines()] if isinstance(value, list) else value.splitlines()
    return ' '.join(re.sub(r'^\s*[-*]\s+', '', line).strip() for line in lines if line.strip())


def changelog_status(existing, version, expected):
    rows = existing.get(version, [])
    if not rows:
        return 'missing'
    if normalized_notes(rows) == normalized_notes(expected):
        return 'confirmed'
    raise ValueError(f'Nexus changelog {version} already contains different text; additive API must not append a duplicate')


def sync_changelog(plan, directory, request=request_json):
    key = os.environ.get('NEXUSMODS_API_KEY', '').strip()
    if not key:
        raise ValueError('NEXUSMODS_API_KEY is missing')
    target = targets()[plan['product']]
    url = f"https://api.nexusmods.com/v1/games/{target['domain']}/mods/{target['page_id']}/changelogs.json"
    headers = {'apikey': key}
    status = changelog_status(request(url, headers=headers), plan['version'], plan['notes_plain'])
    if status == 'missing':
        request(API + f"/mods/{target['mod_uid']}/changelogs", 'POST',
                {'version': plan['version'], 'changelog': plan['notes_plain']}, headers)
        status = changelog_status(request(url, headers=headers), plan['version'], plan['notes_plain'])
        if status != 'confirmed':
            raise RuntimeError('Nexus accepted the changelog write, but readback is missing; do not replay until destination is inspected')
    receipt = {'status': status, 'version': plan['version'], 'product': plan['product'], 'nexus_url': plan['nexus_url']}
    (directory / 'changelog.json').write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
    print(f"Nexus changelog {plan['product']} {plan['version']}: {status}")


def discord_payload(plan):
    target = targets()[plan['product']]
    notes = re.sub(r'^#{1,6}\s+(.+)$', r'**\1**', plan['notes'], flags=re.M)
    if len(notes) > 1400:
        notes = notes[:1396].rsplit(' ', 1)[0] + '\n...'
    return {'allowed_mentions': {'parse': []}, 'embeds': [{
        'title': f"{target['name']} {plan['version']}", 'url': plan['release_url'],
        'description': notes, 'color': 8256434,
        'thumbnail': {'url': f'https://raw.githubusercontent.com/{REPOSITORY}/main/assets/icon.png'},
        'fields': [{'name': 'Get it', 'value': f"[Nexus]({plan['nexus_url']}) - [GitHub]({plan['release_url']})"}],
        'footer': {'text': 'MMORPG Tycoon 2 - MMORPGTycoon2-CardVault'}
    }]}


def webhook_url(value):
    url = urllib.parse.urlsplit(value)
    if url.scheme != 'https' or url.netloc != 'discord.com' or not re.fullmatch(r'/api(?:/v\d+)?/webhooks/\d+/[A-Za-z0-9._-]+', url.path) or url.fragment:
        raise ValueError('DISCORD_RELEASE_WEBHOOK must be an official Discord webhook URL')
    return urllib.parse.urlunsplit((url.scheme, url.netloc, url.path, '', ''))


def canonical_embeds(embeds):
    result = []
    for embed in embeds:
        row = {key: embed[key] for key in ('title', 'url', 'description', 'color') if key in embed}
        if 'fields' in embed:
            row['fields'] = [{**{key: field[key] for key in ('name', 'value')}, 'inline': field.get('inline', False)} for field in embed['fields']]
        for key, value in (('image', 'url'), ('thumbnail', 'url'), ('footer', 'text')):
            if key in embed:
                row[key] = {value: embed[key][value]}
        result.append(row)
    return result


def remote_receipt(plan, filename, directory):
    release = json.loads(gh('release', 'view', plan['tag'], '--json', 'assets'))
    if not any(row['name'] == filename for row in release['assets']):
        return None
    receipt_dir = directory / 'previous'
    receipt_dir.mkdir(parents=True, exist_ok=True)
    gh('release', 'download', plan['tag'], '--dir', str(receipt_dir), '--pattern', filename)
    return json.loads((receipt_dir / filename).read_text(encoding='utf-8'))


def announce(plan, directory, request=request_json, message_id=None, update=False):
    secret = os.environ.get('DISCORD_RELEASE_WEBHOOK', '').strip()
    if not secret:
        raise ValueError('DISCORD_RELEASE_WEBHOOK is missing; release announcement was not sent')
    webhook = webhook_url(secret)
    filename = f"release-delivery-{plan['product']}-{plan['version']}.json"
    previous = remote_receipt(plan, filename, directory)
    identity = {name: plan[name] for name in ('product', 'version', 'tag', 'sha256')}
    expected = canonical_embeds(discord_payload(plan)['embeds'])
    path = directory / filename
    if previous:
        if any(previous.get(name) != value for name, value in identity.items()):
            raise ValueError('Discord delivery receipt differs from release identity')
        message_id = message_id or previous.get('message_id')
        if previous.get('status') != 'confirmed' and not message_id:
            destination = request(webhook, service='Discord')
            print(f"Discord release destination: guild={destination.get('guild_id')}, channel={destination['channel_id']}")
            raise RuntimeError('Discord announcement has an unconfirmed send intent; inspect the channel before reconciling the receipt')
        message = request(webhook + '/messages/' + message_id, service='Discord')
        if message['id'] != message_id:
            raise ValueError('Previously acknowledged Discord message differs')
        if canonical_embeds(message['embeds']) != expected and update:
            if len(message['embeds']) != 1 or any(message['embeds'][0].get(key) != expected[0][key] for key in ('title', 'url')):
                raise ValueError('Discord message does not identify this exact product release; edit rejected')
            (directory / 'discord-before.json').write_text(json.dumps(message, indent=2) + '\n', encoding='utf-8')
            previous.update(status='updating', message_id=message_id)
            path.write_text(json.dumps(previous, indent=2) + '\n', encoding='utf-8')
            gh('release', 'upload', plan['tag'], str(path), '--clobber')
            message = request(webhook + '/messages/' + message_id, 'PATCH', discord_payload(plan), service='Discord')
            (directory / 'discord-message.json').write_text(json.dumps(message, indent=2) + '\n', encoding='utf-8')
            message = request(webhook + '/messages/' + message_id, service='Discord')
        if canonical_embeds(message['embeds']) != expected:
            raise ValueError('Previously acknowledged Discord message differs')
        if previous.get('status') != 'confirmed' or previous.get('embeds') != expected:
            previous.update(status='confirmed', message_id=message_id, channel_id=message['channel_id'], embeds=expected)
            path.write_text(json.dumps(previous, indent=2) + '\n', encoding='utf-8')
            gh('release', 'upload', plan['tag'], str(path), '--clobber')
        print(f"Discord {plan['product']} {plan['version']}: already confirmed ({message['id']})")
        return
    if message_id:
        raise ValueError('Discord message reconciliation requires an existing send intent')
    receipt = {**identity, 'status': 'pending', 'run_id': os.environ.get('GITHUB_RUN_ID')}
    path.write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
    # Persist intent before sending. An ambiguous POST blocks replay across workflow reruns.
    gh('release', 'upload', plan['tag'], str(path))
    message = request(webhook + '?wait=true', 'POST', discord_payload(plan), service='Discord')
    (directory / 'discord-message.json').write_text(json.dumps(message, indent=2) + '\n', encoding='utf-8')
    if not message or not re.fullmatch(r'\d+', message.get('id', '')):
        raise RuntimeError('Discord did not acknowledge a message ID; send intent remains pending')
    receipt.update(status='acknowledged', message_id=message['id'], channel_id=message['channel_id'])
    path.write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
    gh('release', 'upload', plan['tag'], str(path), '--clobber')
    if canonical_embeds(message['embeds']) != expected:
        raise ValueError('Discord acknowledged a message with different release content; inspect send intent before retrying')
    receipt.update(status='confirmed', message_id=message['id'], channel_id=message['channel_id'], embeds=expected)
    path.write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
    gh('release', 'upload', plan['tag'], str(path), '--clobber')
    print(f"Discord {plan['product']} {plan['version']}: confirmed ({message['id']})")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=Path, required=True)
    parser.add_argument('--announce', action='store_true')
    parser.add_argument('--message-id', help='Read and reconcile this existing Discord message; never send a replacement')
    parser.add_argument('--update-announcement', action='store_true', help='Update the identified existing release post and read it back')
    args = parser.parse_args()
    if args.update_announcement and not (args.announce and args.message_id):
        raise ValueError('Updating a Discord post requires --announce and its existing --message-id')
    plan = json.loads((args.directory / 'plan.json').read_text(encoding='utf-8'))
    if plan['status'] not in ('ready', 'already-present'):
        raise ValueError('Release has no deployable product')
    sync_changelog(plan, args.directory)
    if args.announce:
        if args.message_id and not re.fullmatch(r'\d+', args.message_id):
            raise ValueError('Invalid Discord message ID')
        announce(plan, args.directory, message_id=args.message_id, update=args.update_announcement)


if __name__ == '__main__':
    try:
        main()
    except (ValueError, KeyError, OSError, RuntimeError, subprocess.SubprocessError) as error:
        print('error: ' + str(error), file=sys.stderr)
        sys.exit(1)
