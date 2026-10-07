#!/bin/sh
# Builds and runs the PC-side core tests with any host gcc (e.g. MSYS2 mingw64).
#   tests/run_host_tests.sh "/g/dev/Checkpoint Save" ["/g/dev/Chapter0 - Vanilla" ...]
# The first argument is a Checkpoint dump folder; any further arguments are single save files.
set -e
cd "$(dirname "$0")/.."
SAVES="${1:-saves}"
[ $# -gt 0 ] && shift
mkdir -p build-host
gcc -std=gnu11 -O2 -g -Wall -Wextra -Iinclude \
    source/core/crc32.c source/core/huffman.c source/core/fesave.c source/core/fedata_tables.c \
    source/core/feglobal.c source/core/feimport.c source/core/femod.c source/core/femod_pids.c tests/host_test.c -o build-host/host_test
for f in Map1 Map0 Chapter2 Chapter1 Chapter0; do
    if [ -f "$SAVES/$f" ]; then set -- "$SAVES/$f" "$@"; fi
done
if [ -f "$SAVES/Global" ]; then set -- "--global=$SAVES/Global" "$@"; fi
build-host/host_test "$@"
