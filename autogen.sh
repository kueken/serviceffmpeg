#!/bin/sh
set -e

mkdir -p m4

# libtoolize installs libtool.m4 into m4/ - required for LT_INIT
if command -v libtoolize >/dev/null 2>&1; then
    libtoolize --force --copy --install
elif command -v glibtoolize >/dev/null 2>&1; then
    # macOS with Homebrew
    glibtoolize --force --copy --install
else
    echo "ERROR: libtoolize not found. Install libtool." >&2
    exit 1
fi

autoreconf --force --install --verbose
