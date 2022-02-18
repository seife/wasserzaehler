#!/bin/sh
if ! [ -d "$1" ]; then
	echo "need sketch dir as first argument" >&1
	exit 1
fi
rm -f "$1/wasserzaehler-version.h"
set -o noclobber
cat > "$1/wasserzaehler-version.h" << EOF
#define WASSER_VERSION "$(git describe --always --dirty)"
EOF
