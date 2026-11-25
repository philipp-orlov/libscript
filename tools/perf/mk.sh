#!/bin/sh
# mk.sh: build $VMB; fails loudly
cd ${VMB:-/tmp/b/vm} && make -j18 2>&1 | grep -E 'error|\*\*\*' | head -8
[ -z "$(make -j18 2>&1 | grep -E '\*\*\*')" ]
