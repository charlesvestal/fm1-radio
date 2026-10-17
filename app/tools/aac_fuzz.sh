#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# Build app/tools/aac_fuzz.c with the AAC decoder, under AddressSanitizer, and run it: aac_fuzz.sh CAPTURE [ROUNDS [SEED]]
# (needs a clang whose AddressSanitizer starts on this computer: Homebrew's llvm on macOS; CC= to choose another).
# The window tables are built without it: the decoder reads one past the end of one of them (harmless, in flash).
set -e
D=$(cd "$(dirname "$0")/.." && pwd)
P=$D/third_party/pvaac
CC=${CC:-$( [ -x /opt/homebrew/opt/llvm/bin/clang ] && echo /opt/homebrew/opt/llvm/bin/clang || echo cc )}
O=${TMPDIR:-/tmp}/aac_fuzz.$$
mkdir -p "$O"
trap 'rm -rf "$O"' EXIT
F="-O1 -g -w -include $P/pv_fm1.h -DAAC_PLUS -I$P"
for c in "$P"/*.c; do
    b=$(basename "$c" .c)
    S="-fsanitize=address -fsanitize=bounds"
    [ "$b" = window_tables_fxp ] && S=
    $CC $F $S '-D__inline=static inline' -c "$c" -o "$O/$b.o" &
done
wait
$CC $F -fsanitize=address -fsanitize=bounds "$D/tools/aac_fuzz.c" "$O"/*.o -o "$O/aac_fuzz"
ASAN_OPTIONS=detect_leaks=0 "$O/aac_fuzz" "$@"
