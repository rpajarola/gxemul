# Packages

gxemul is packaged as `gxemul-rp` (called `gxemul` before 1.0.0): a
`.deb` for Debian and Ubuntu (amd64 and arm64, built on Ubuntu 24.04,
published in the shared apt repository [rpajarola/apt](https://github.com/rpajarola/apt)),
a `.pkg` for macOS, and a Homebrew formula in
[rpajarola/homebrew-tap](https://github.com/rpajarola/homebrew-tap).
The `.deb` conflicts with, replaces and provides `gxemul`, so it replaces
an installed `gxemul` package, including Debian's and Ubuntu's, and
never competes with it for upgrades. The program is still `gxemul`.
Publishing a GitHub release builds all of them
([`.github/workflows/release.yml`](../.github/workflows/release.yml)).

## Install

**Ubuntu and Debian**, from the [rpajarola apt repository](https://rpajarola.github.io/apt),
which also has the other packages built this way:

```
sudo curl -fsSLo /usr/share/keyrings/rpajarola.gpg https://rpajarola.github.io/apt/rpajarola.gpg
echo "deb [signed-by=/usr/share/keyrings/rpajarola.gpg] https://rpajarola.github.io/apt stable main" \
  | sudo tee /etc/apt/sources.list.d/rpajarola.list
sudo apt update
sudo apt install gxemul-rp
```

or a downloaded package: `sudo apt install ./gxemul-rp_<version>_<arch>.deb`.

**macOS** with Homebrew:

```
brew install rpajarola/tap/gxemul-rp
```

or the `.pkg` from a release: `sudo installer -pkg gxemul-rp-<version>-arm64.pkg -target /`
(the package is not signed, so double-clicking it needs right-click → Open).

## What is installed

Debian and Ubuntu:

```
/usr/bin/gxemul
/usr/share/man/man1/gxemul.1.gz
/usr/share/doc/gxemul/              # the HTML documentation
```

Its dependencies are worked out by `dpkg-shlibdeps` when it is built.

macOS (`.pkg`):

```
/usr/local/bin/gxemul
/usr/local/lib/gxemul/               # libX11 and the libraries it needs, from Homebrew
/usr/local/share/man/man1/gxemul.1
/usr/local/share/doc/gxemul/
/usr/local/share/gxemul/uninstall.sh
```

The package runs without Homebrew: `packaging/bundle-dylibs.sh` copies
libX11 and the libraries it needs into it. Framebuffers need an X
server, such as XQuartz. It is built for the Mac's architecture (Apple
silicon in CI); on Intel Macs, use Homebrew. Remove it with
`sudo /usr/local/share/gxemul/uninstall.sh`.

## Build

Debian or Ubuntu:

```
sudo apt install build-essential pkg-config libx11-dev dpkg-dev
packaging/build-deb.sh      # dist/gxemul-rp_<version>_<arch>.deb
```

macOS:

```
brew install pkgconf libx11
packaging/build-pkg.sh      # dist/gxemul-rp-<version>-<arch>.pkg
```

The `.deb` needs nfpm (`go install github.com/goreleaser/nfpm/v2/cmd/nfpm@latest`,
or a release binary). The version is the git tag without its `v`, or
`1.0.0~git<sha>`; set `VERSION` to override it, and `configure` takes it
from `GXEMUL_VERSION`.

## Releasing

Tag, then publish a GitHub release for the tag:

```
git tag v1.0.0 && git push origin v1.0.0
gh release create v1.0.0 --generate-notes
```

The release workflow then attaches the `.deb`s and the `.pkg`, has
[rpajarola/apt](https://github.com/rpajarola/apt) rebuild the apt
repository (from the `.deb`s of every release of every project in it),
and updates the formula's URL and checksum in
[rpajarola/homebrew-tap](https://github.com/rpajarola/homebrew-tap). To
rebuild a release's packages, run the workflow by hand with its tag.

One-time setup: a `PACKAGES_TOKEN` secret, a fine-grained token with
contents read and write on `rpajarola/homebrew-tap` and `rpajarola/apt`
(the same token in every project):

```
gh secret set PACKAGES_TOKEN
```

Without it, the release still gets its packages, but the formula is not
updated and the apt repository is only rebuilt by its weekly run (or by
hand: Actions → publish → Run workflow in rpajarola/apt). The signing key
lives in rpajarola/apt.
