#!/bin/sh
# Type-checks the 3DS sources on a PC (no devkitPro needed) against the stub
# headers in tests/stubs. Catches typos and type errors, not libctru API drift.
cd "$(dirname "$0")/.."
status=0
for f in source/*.c source/core/*.c; do
    gcc -std=gnu11 -fsyntax-only -Wall -Wextra -DAPP_VERSION='"0.1.0"' -D__3DS__ \
        -Itests/stubs -Iinclude "$f" || status=1
done
[ $status -eq 0 ] && echo "3DS sources: syntax/type check OK"
exit $status
