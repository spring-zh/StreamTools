#!/bin/bash
# Run in Terminal or double-click in Finder. All paths are relative to this file.
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "$0")" && pwd)"
# Finder-launched Terminal sessions may not have loaded Homebrew's shell setup.
export PATH="$PATH:/opt/homebrew/bin:/usr/local/bin"
if ! command -v python3 >/dev/null 2>&1; then
    echo "需要 Python 3（仅打包电脑需要），请先安装 Xcode Command Line Tools。" >&2
    exit 1
fi
exec python3 "$project_dir/scripts/package_macos.py" "$@"
