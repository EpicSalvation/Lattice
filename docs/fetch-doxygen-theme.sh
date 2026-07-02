#!/usr/bin/env bash
# Fetches the pinned doxygen-awesome-css release into docs/doxygen-awesome/
# (git-ignored; not vendored). Run this once before `doxygen docs/Doxyfile`,
# whether locally or in CI. Safe to re-run — it re-clones from scratch.
set -euo pipefail

VERSION="v2.3.4"
DEST="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/doxygen-awesome"

rm -rf "$DEST"
git clone --depth 1 --branch "$VERSION" \
    https://github.com/jothepro/doxygen-awesome-css.git "$DEST"
rm -rf "$DEST/.git"

echo "doxygen-awesome-css $VERSION fetched into $DEST"
