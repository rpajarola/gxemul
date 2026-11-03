#!/usr/bin/env python3
#
#  Makes the reference output for the Alpha instruction tests.
#
#  Usage: make_expected.py builddir expecteddir
#
#  For each test, builddir must contain NAME.list and NAME.qemu (the output
#  of NAME.linux under "qemu-alpha -cpu ev4-alpha-cpu"). The floating-point
#  model in fpmodel.py is used where it has a value: it replaces QEMU for
#  the VAX tests, and any disagreement with QEMU on the other tests is
#  reported (and the model's value is used).

import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fpmodel                                          # noqa: E402

build, outdir = sys.argv[1], sys.argv[2]
os.makedirs(outdir, exist_ok=True)
names = open(os.path.join(build, 'tests.txt')).read().split()
for name in names:
    desc = open(os.path.join(build, name + '.list')).read().splitlines()
    qpath = os.path.join(build, name + '.qemu')
    qemu = open(qpath).read().splitlines() if os.path.exists(qpath) else []
    use_qemu = not name.endswith('_vax')
    out = []
    disagree = collections.Counter()
    missing = collections.Counter()
    for i, d in enumerate(desc):
        model = fpmodel.expected(d) if name.startswith('fp_') else None
        q = qemu[i] if use_qemu and i < len(qemu) else None
        if model is not None:
            mv = '%016x' % model
            if q is not None and q != mv:
                disagree[d.split()[0]] += 1
                if disagree[d.split()[0]] <= 3:
                    print('%s: %s: model %s, qemu %s' % (name, d, mv, q))
            out.append(mv)
        elif q is not None:
            out.append(q)
        else:
            missing[d.split()[0]] += 1
            out.append('?')
    with open(os.path.join(outdir, name + '.txt'), 'w') as f:
        f.write('\n'.join(out) + '\n')
    for k, v in sorted(disagree.items()):
        print('%s: %s: %d disagreements with QEMU' % (name, k, v))
    for k, v in sorted(missing.items()):
        print('%s: %s: %d without reference' % (name, k, v))
