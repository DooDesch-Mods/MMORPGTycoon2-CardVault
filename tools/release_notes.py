"""Render one product/version changelog section for release destinations."""
import re


def section(text, product, version):
    prefix = 'Vortex extension ' if product == 'vortex' else ''
    pattern = re.compile(r'^## ' + re.escape(prefix + version) + r'(?: development preview)?\s*$', re.M)
    matches = list(pattern.finditer(text))
    if len(matches) != 1:
        raise ValueError(f'{product} {version}: expected exactly one changelog section, found {len(matches)}')
    remaining = text[matches[0].end():]
    end = re.search(r'^## ', remaining, re.M)
    notes = remaining[:end.start()] if end else remaining
    notes = notes.strip()
    if not notes or not re.search(r'^- \S', notes, re.M):
        raise ValueError(f'{product} {version}: changelog has no release notes')
    return notes


def plain(markdown):
    text = re.sub(r'\[([^\]]+)\]\(([^)]+)\)', r'\1 (\2)', markdown)
    text = re.sub(r'\*\*(.+?)\*\*', r'\1', text)
    text = re.sub(r'`([^`]+)`', r'\1', text)
    return re.sub(r'^#{1,6}\s+', '', text, flags=re.M).strip()


def bbcode(markdown):
    text = re.sub(r'\[([^\]]+)\]\((https?://[^)]+)\)', r'[url=\2]\1[/url]', markdown)
    text = re.sub(r'\*\*(.+?)\*\*', r'[b]\1[/b]', text)
    text = re.sub(r'`([^`]+)`', r'\1', text)
    text = re.sub(r'^#{1,6}\s+(.+)$', r'[b]\1[/b]', text, flags=re.M)
    result, level, item = [], 0, None
    def flush():
        nonlocal item
        if item is not None:
            result.append('[*]' + item)
            item = None
    for line, source in zip(text.splitlines(), markdown.splitlines()):
        if re.match(r'^#{1,6}\s+', source):
            flush()
            while level: result.append('[/list]'); level -= 1
            result.append(line)
            continue
        bullet = re.match(r'^(\s*)[-*]\s+(.+)', line)
        if bullet:
            flush()
            desired = 2 if len(bullet[1]) >= 2 else 1
            while level > desired: result.append('[/list]'); level -= 1
            while level < desired: result.append('[list]'); level += 1
            item = bullet[2]
        elif line.strip() and item is not None:
            item += ' ' + line.strip()
        else:
            flush()
            while level: result.append('[/list]'); level -= 1
            result.append(line)
    flush()
    while level: result.append('[/list]'); level -= 1
    return '\n'.join(result).strip()
