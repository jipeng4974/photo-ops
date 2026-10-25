#!/usr/bin/env bash
# Local fallback when xmake/brew is not available. No system packages modified.
set -euo pipefail
cd "$(dirname "$0")/.."
project_root=$PWD
mkdir -p .tools
archive="$project_root/.tools/xmake-3.1.2.tar.gz"
curl -fL --retry 3 https://github.com/xmake-io/xmake/releases/download/v3.1.2/xmake-v3.1.2.tar.gz -o "$archive"
printf '%s  %s\n' 0e068e76c66dddd87573a31b77f37c8c4ebe7d50b0fb48d507dcb902c77e6cc4 "$archive" | sha256sum -c -
tar -xzf "$archive" -C .tools
cd .tools/xmake-3.1.2
./configure --prefix="$project_root/.tools/install"
make -j"${JOBS:-4}"
make install
