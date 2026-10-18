#!/usr/bin/env python3
#
#  testdata.py:  fetches and checks the disk, CD-ROM, tape, and ROM images
#  that the machines under testdata/ need.  The images are not kept in git;
#  each machine's manifest.json says which files it needs, their sha256,
#  and where they can be downloaded from.  See testdata/README.md.
#
#  Usage:
#
#	test/testdata.py list
#	test/testdata.py fetch  mips/decstation/3max-ultrix-4.5 [--force]
#	test/testdata.py verify mips/decstation/3max-ultrix-4.5
#	test/testdata.py pin    mips/decstation/3max-ultrix-4.5 [--force]
#
#  A machine is named by its directory, relative to testdata/ (or to the
#  current directory).  fetch gets the files that are missing; a file whose
#  sha256 does not match is replaced only with --force.  pin records the
#  sha256 and size of the files that are there, e.g. after adding a new
#  file, or after installing onto a disk image.
#
#  If GXEMUL_TESTDATA_CACHE is set, it is a directory where files are kept
#  by their sha256 (as CACHE/sha256/<hex>).  fetch looks there before
#  downloading, and fetch and pin put the files they have into it, so a
#  cache shared between checkouts (or hosts) needs each file only once.
#
#  Only the Python standard library is needed.  zstd-compressed sources
#  need the zstd command, and gh: sources the GitHub CLI (gh).
#

import bz2
import gzip
import hashlib
import json
import lzma
import os
import shutil
import subprocess
import sys
import tempfile
import urllib.request

TESTDATA = os.path.join(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))), 'testdata')
CACHE = os.environ.get('GXEMUL_TESTDATA_CACHE', '')


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def cache_path(sha):
    return os.path.join(CACHE, 'sha256', sha) if CACHE and sha else ''


def find_machine(name):
    for d in (os.path.join(TESTDATA, name), name):
        if os.path.isfile(os.path.join(d, 'manifest.json')):
            return d
    sys.exit('%s: no manifest.json there (or under %s)' % (name, TESTDATA))


def load(mdir):
    with open(os.path.join(mdir, 'manifest.json')) as f:
        return json.load(f)


def save(mdir, manifest):
    path = os.path.join(mdir, 'manifest.json')
    with open(path + '.new', 'w') as f:
        json.dump(manifest, f, indent=2)
        f.write('\n')
    os.replace(path + '.new', path)


def status(mdir, entry):
    """Returns 'ok', 'missing', 'unpinned' (present, no sha256 yet), or
    'mismatch'."""
    path = os.path.join(mdir, entry['path'])
    if not os.path.isfile(path):
        return 'missing'
    if not entry.get('sha256'):
        return 'unpinned'
    return 'ok' if sha256_of(path) == entry['sha256'] else 'mismatch'


def download(source, dest):
    """Gets one source into dest, unpacked.  Returns an error string, or
    None when it worked."""
    url = source.get('url', '')
    unpack = source.get('unpack', '')
    tmpdir = tempfile.mkdtemp(dir=os.path.dirname(dest))
    raw = os.path.join(tmpdir, 'download')
    try:
        if url.startswith('gh:'):
            #  gh:owner/repo@tag/asset, a release asset (private repos too)
            repo, _, rest = url[3:].partition('@')
            tag, _, asset = rest.partition('/')
            r = subprocess.run(['gh', 'release', 'download', tag, '-R', repo,
                '-p', asset, '-O', raw], capture_output=True, text=True)
            if r.returncode != 0:
                return 'gh: ' + r.stderr.strip()
        else:
            with urllib.request.urlopen(url) as resp, open(raw, 'wb') as f:
                shutil.copyfileobj(resp, f, 1 << 20)

        if unpack in ('', 'none'):
            os.replace(raw, dest)
        elif unpack in ('gz', 'bz2', 'xz'):
            opener = {'gz': gzip.open, 'bz2': bz2.open, 'xz': lzma.open}
            with opener[unpack](raw, 'rb') as fin, open(dest, 'wb') as fout:
                shutil.copyfileobj(fin, fout, 1 << 20)
        elif unpack == 'zst':
            r = subprocess.run(['zstd', '-d', '-q', '-f', raw, '-o', dest],
                capture_output=True, text=True)
            if r.returncode != 0:
                return 'zstd: ' + r.stderr.strip()
        else:
            return 'unknown unpack "%s"' % unpack
        return None
    except Exception as e:
        return str(e)
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)


def to_cache(path, sha):
    c = cache_path(sha)
    if c and not os.path.exists(c):
        os.makedirs(os.path.dirname(c), exist_ok=True)
        shutil.copyfile(path, c + '.new')
        os.replace(c + '.new', c)


def fetch_one(mdir, entry, force):
    path = os.path.join(mdir, entry['path'])
    sha = entry.get('sha256', '')
    st = status(mdir, entry)
    if st in ('ok', 'unpinned'):
        return st
    if st == 'mismatch' and not force:
        #  It may be a disk with changes worth keeping (e.g. not yet pinned
        #  after an install), so it is replaced only when asked to.
        return 'mismatch (use --force to replace it)'
    os.makedirs(os.path.dirname(path), exist_ok=True)

    c = cache_path(sha)
    if c and os.path.isfile(c):
        shutil.copyfile(c, path)
        return 'from cache'

    errors = []
    for source in entry.get('sources', []):
        tmp = path + '.part'
        err = download(source, tmp)
        if err is None and sha and sha256_of(tmp) != sha:
            err = 'sha256 does not match'
        if err is None:
            os.replace(tmp, path)
            if sha:
                to_cache(path, sha)
            return 'downloaded' + ('' if sha else ', not pinned')
        if os.path.exists(tmp):
            os.remove(tmp)
        errors.append('%s: %s' % (source.get('url'), err))

    if st == 'missing' and 'create_mb' in entry:
        #  Nothing to download: an empty disk image, to install onto.
        with open(path, 'wb') as f:
            f.truncate(entry['create_mb'] * 1024 * 1024)
        return 'created empty (%d MB)' % entry['create_mb']

    for e in errors:
        print('    ' + e)
    return st + (', no sources' if not entry.get('sources') else '')


def cmd_list(_args):
    for root, dirs, files in sorted(os.walk(TESTDATA)):
        dirs.sort()
        if 'manifest.json' in files:
            m = load(root)
            print('%-40s %s' % (os.path.relpath(root, TESTDATA),
                m.get('description', '')))


def cmd_fetch(args):
    force = '--force' in args
    bad = 0
    for name in [a for a in args if a != '--force']:
        mdir = find_machine(name)
        print(name + ':')
        for entry in load(mdir)['files']:
            result = fetch_one(mdir, entry, force)
            print('  %-30s %s' % (entry['path'], result))
            if result.startswith(('missing', 'mismatch')):
                bad += 1
    return 1 if bad else 0


def cmd_verify(args):
    bad = 0
    for name in args:
        mdir = find_machine(name)
        print(name + ':')
        for entry in load(mdir)['files']:
            st = status(mdir, entry)
            print('  %-30s %s' % (entry['path'], st))
            if st != 'ok':
                bad += 1
    return 1 if bad else 0


def cmd_pin(args):
    force = '--force' in args
    for name in [a for a in args if a != '--force']:
        mdir = find_machine(name)
        manifest = load(mdir)
        print(name + ':')
        for entry in manifest['files']:
            path = os.path.join(mdir, entry['path'])
            if not os.path.isfile(path):
                print('  %-30s missing' % entry['path'])
                continue
            sha = sha256_of(path)
            if entry.get('sha256') and entry['sha256'] != sha and not force:
                print('  %-30s mismatch (use --force to re-pin)' %
                    entry['path'])
                continue
            changed = entry.get('sha256') != sha
            entry['sha256'] = sha
            entry['size'] = os.path.getsize(path)
            to_cache(path, sha)
            print('  %-30s %s' % (entry['path'],
                'pinned' if changed else 'ok'))
        save(mdir, manifest)
    return 0


def main():
    commands = {'list': cmd_list, 'fetch': cmd_fetch, 'verify': cmd_verify,
        'pin': cmd_pin}
    if len(sys.argv) < 2 or sys.argv[1] not in commands or \
            (sys.argv[1] != 'list' and len(sys.argv) < 3):
        sys.exit('usage: testdata.py list | fetch MACHINE... [--force] | '
            'verify MACHINE... | pin MACHINE... [--force]')
    return commands[sys.argv[1]](sys.argv[2:]) or 0


if __name__ == '__main__':
    sys.exit(main())
