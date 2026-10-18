# Test data

Machines to test gxemul with, and the disk, CD-ROM, tape, and ROM images
they need.  Only the descriptions are in git (`*.json`); the images are
fetched with `test/testdata.py`, and are ignored by git.

## Layout

The directories follow gxemul's machine names (`gxemul -H`): architecture,
then machine type.  In gxemul, `decstation` is the type and `pmax` one of
its models (the 3100), so the type directory is `decstation`.

```
testdata/
  mips/
    decstation/
      rom/                      system and option ROM images, shared
        kn02-notests.bin
        pmad-aa.bin
        pmagb-b.bin
        pmaz-a.bin
      media/                    install media, shared
        ultrix-4.5.iso
      3max-ultrix-4.5/          one machine: model + OS
        manifest.json           the files it needs
        machine.json            gxemul machine.json: runs the installed disk
        install.json            the same, writing to the disk, to install
        disk/rz0.img
```

ROMs and media are kept once per machine type, so other machines of the
type (another model, or another OS) use the same files.

## manifest.json

```json
{
  "name": "3max-ultrix-4.5",
  "description": "Ultrix/RISC 4.5 on a DECstation 5000/200, running its own PROM",
  "files": [
    {
      "path": "../media/ultrix-4.5.iso",
      "sha256": "…",
      "size": 0,
      "notes": "Ultrix/RISC 4.5 install CD",
      "sources": [
        { "url": "https://…/ultrix-risc-4.5-mode1.ufs.bz2", "unpack": "bz2" },
        { "url": "gh:owner/gxemul-testdata@v1/ultrix-4.5.iso.zst", "unpack": "zst" }
      ]
    },
    { "path": "disk/rz0.img", "sha256": "…", "create_mb": 800, "sources": [] }
  ]
}
```

- `path` is relative to the machine's directory.
- `sha256` and `size` are of the file as used (after unpacking).  An
  empty `sha256` means the file is not pinned yet.
- `sources` are tried in order.  A `url` is http(s), or
  `gh:owner/repo@tag/asset` for a GitHub release asset (needs `gh`, and
  works with private repositories).  `unpack` is `gz`, `bz2`, `xz`, or
  `zst` (needs `zstd`).
- `create_mb`: when nothing can be fetched, an empty image of that size is
  created (a disk to install onto).

## Fetching and adding files

```sh
test/testdata.py list
test/testdata.py fetch  mips/decstation/3max-ultrix-4.5   # get what is missing
test/testdata.py verify mips/decstation/3max-ultrix-4.5   # check the sha256s
test/testdata.py pin    mips/decstation/3max-ultrix-4.5   # record sha256s of the files there
```

To add a file you have (e.g. a ROM dump): put it at its path, run `pin`,
and commit the manifest.  `fetch` never replaces a file whose sha256 does
not match unless given `--force`, since that may be a disk with changes
worth keeping.

With `GXEMUL_TESTDATA_CACHE=dir`, files are also kept in `dir/sha256/<hex>`.
`fetch` looks there first, and `fetch` and `pin` add to it, so a cache
shared between checkouts or hosts needs each file only once.

## Installing

```sh
test/testdata.py fetch mips/decstation/3max-ultrix-4.5    # also creates an empty disk/rz0.img
cd testdata/mips/decstation/3max-ultrix-4.5
gxemul install.json                                       # >> boot 5/rz4/vmunix, and install
cd - && test/testdata.py pin mips/decstation/3max-ultrix-4.5
```

`machine.json` then runs the installed disk with `"overlay": true`, so
what a test writes goes to a temporary overlay, and `disk/rz0.img` stays
as installed.
