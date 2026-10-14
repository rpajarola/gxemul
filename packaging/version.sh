# Sourced by the build scripts: sets VERSION from the environment, the git
# tag (without its v), or 1.0.0~git<sha>.
if [ -z "${VERSION:-}" ]; then
  if tag=$(git describe --tags --exact-match 2>/dev/null); then
    VERSION="${tag#v}"
  else
    VERSION="1.0.0~git$(git rev-parse --short HEAD)"
  fi
fi
