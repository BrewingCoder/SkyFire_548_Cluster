#!/usr/bin/env python3
# This file is part of Project SkyFire https://www.projectskyfire.org.
# See LICENSE.md file for Copyright information
"""Preserve published SQL when merging upstream; stage collisions for promotion."""
import argparse
import hashlib
from pathlib import Path
import re
import subprocess

DOMAINS = ('auth', 'characters', 'world', 'hub')
RELEASE = re.compile(r'^(\d{4}[_-]\d{2}[_-]\d{2})_(auth|characters|world|hub)_(\d{2})(?:\D.*)?\.sql$')


def git(root, *args):
    return subprocess.check_output(['git', '-C', str(root), *args])


def content_key(data):
    # Git checkout line endings are not a SQL change. Preserve every other byte.
    return data.replace(b'\r\n', b'\n')


def reconcile(root, base_ref, apply=False):
    root = Path(root).resolve()
    base = git(root, 'rev-parse', '--verify', base_ref + '^{commit}').decode().strip()
    actions = []
    writes, deletes = {}, set()
    for domain in DOMAINS:
        directory = f'sql/updates/{domain}'
        paths = git(root, 'ls-tree', '-r', '--name-only', '-z', base, '--', directory).decode().split('\0')
        original = {p: git(root, 'show', f'{base}:{p}') for p in paths if p.endswith('.sql')}
        current = {p.relative_to(root).as_posix(): p.read_bytes()
                   for p in sorted((root / directory).glob('*.sql'))}
        slots, contents = {}, {}
        for path, data in original.items():
            match = RELEASE.fullmatch(Path(path).name)
            if match:
                slots.setdefault((match[1].replace('-', '_'), match[3]), path)
            contents.setdefault(content_key(data), path)
            if path not in current:
                raise ValueError(f'Published SQL was deleted: {path}; restore it before reconciliation')

        for pending in sorted((root / f'sql/pending_updates/{domain}').glob('*.sql')):
            contents.setdefault(content_key(pending.read_bytes()), pending.relative_to(root).as_posix())

        for path, data in current.items():
            if path in original and content_key(data) == content_key(original[path]):
                continue
            match = RELEASE.fullmatch(Path(path).name)
            slot = (match[1].replace('-', '_'), match[3]) if match else None
            duplicate = contents.get(content_key(data))
            collision = path in original or (slot is not None and slot in slots)
            if not duplicate and not collision:
                contents[content_key(data)] = path
                if slot:
                    slots[slot] = path
                continue
            # Do not silently break an existing promotion identity.
            if (root / (path + '.pending-name')).exists():
                raise ValueError(f'Collision has promotion metadata; review manually: {path}')
            if duplicate:
                actions.append(f'SKIP identical SQL: {path} (already in {duplicate})')
            else:
                digest = hashlib.sha256(content_key(data)).hexdigest()[:16]
                name = f'merge_{Path(path).stem}_{digest}.sql'
                target = f'sql/pending_updates/{domain}/{name}'
                if (root / target).exists() and content_key((root / target).read_bytes()) != content_key(data):
                    raise ValueError(f'Pending destination has different SQL: {target}')
                writes[target] = data
                contents[content_key(data)] = target
                actions.append(f'PENDING collision: {path} -> {target}')
            if path in original:
                writes[path] = original[path]
                actions.append(f'PRESERVE published SQL: {path}')
            else:
                deletes.add(path)
        # All validation finishes before any files are changed.
    if apply:
        for path, data in writes.items():
            target = root / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        for path in deletes:
            (root / path).unlink()
    return actions


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', default='.')
    parser.add_argument('--base-ref', required=True, help='Clustering commit BEFORE the upstream merge')
    parser.add_argument('--apply', action='store_true')
    args = parser.parse_args()
    try:
        actions = reconcile(args.root, args.base_ref, args.apply)
    except (ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'SQL reconciliation failed: {error}\n')
    for action in actions:
        print(action)
    if not actions:
        print('No incoming SQL collisions or identical copies found.')
    elif not args.apply:
        parser.exit(1, 'Changes required; rerun with --apply to stage collisions and skip identical copies.\n')


if __name__ == '__main__':
    main()
