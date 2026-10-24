#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# Radio's tests: the synth and the favorites' list alone (cc), then the app in fm1-base's host simulator.
#   sh tests/run_tests.sh   (from the repo root; PYTHON as for ./build.sh)
set -e
cd "$(dirname "$0")/.."
mkdir -p app/build/tests
for t in synth fav; do
    [ -f tests/${t}_test.c ] || continue
    cc -O1 -Wall -Wno-unused-function -Ibase/include -o app/build/tests/${t}_test tests/${t}_test.c
    app/build/tests/${t}_test
done
if ls tests/*.host >/dev/null 2>&1; then
    ./build.sh --host >/dev/null
    for s in tests/*.host; do
        echo "== $s"
        app/build/host/radio_host "$s" app/build/tests
    done
fi
echo "all tests passed"
