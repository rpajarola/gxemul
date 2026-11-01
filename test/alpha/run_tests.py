#!/usr/bin/env python3
#
#  Runs the Alpha instruction tests in gxemul and compares the output with
#  the reference output.
#
#  Usage: run_tests.py [-v] builddir [gxemul-binary [expecteddir]]
#
#  builddir is where build_tests.sh put the test programs. The default
#  gxemul binary is the one at the top of the source tree, and the default
#  reference output is in the "expected" directory next to this script.

import collections
import os
import subprocess
import sys

here = os.path.dirname(os.path.abspath(__file__))
args = sys.argv[1:]
verbose = False
if args and args[0] == '-v':
    verbose = True
    args = args[1:]
if not args:
    sys.exit(__doc__ if __doc__ else 'usage: run_tests.py builddir')
build = args[0]
gxemul = args[1] if len(args) > 1 else os.path.join(here, '..', '..',
                                                    'gxemul')
expdir = args[2] if len(args) > 2 else os.path.join(here, 'expected')

names = open(os.path.join(build, 'tests.txt')).read().split()
total_fail = 0
for name in names:
    desc = open(os.path.join(build, name + '.list')).read().splitlines()
    exp = open(os.path.join(expdir, name + '.txt')).read().splitlines()
    try:
        p = subprocess.run([gxemul, '-q', '-E', 'testalpha',
                            os.path.join(build, name + '.gx')],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           timeout=120)
        out = p.stdout.decode('latin-1').splitlines()
    except subprocess.TimeoutExpired:
        out = ['(timeout)']
    #  Result lines are 16 hex digits; anything else is emulator output.
    res = [l for l in out if len(l) == 16 and
           all(c in '0123456789abcdef' for c in l)]
    other = [l for l in out if l not in res and l.strip()]
    fails = collections.Counter()
    shown = 0
    for i, d in enumerate(desc):
        got = res[i] if i < len(res) else '(none)'
        if exp[i] == '?' or got == exp[i]:
            continue
        fails[d.split()[0]] += 1
        if verbose or shown < 5:
            print('  %s: %s: got %s, expected %s' % (name, d, got, exp[i]))
            shown += 1
    nfail = sum(fails.values())
    total_fail += nfail
    status = 'ok' if nfail == 0 else 'FAIL'
    print('%-10s %s  %d/%d passed' % (name, status, len(desc) - nfail,
                                      len(desc)))
    if nfail:
        print('  by instruction: ' + ', '.join(
            '%s:%d' % kv for kv in sorted(fails.items())))
        if len(res) < len(desc):
            print('  only %d of %d results; emulator said:' % (
                len(res), len(desc)))
            for l in other[-8:]:
                print('    ' + l)
sys.exit(1 if total_fail else 0)
