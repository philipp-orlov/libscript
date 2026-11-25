#!/bin/sh
# fz.sh SEED [N]: both fuzzers, both memory modes, two tier settings; expect "0 disagreements" six times
cd "$(dirname "$0")/../.."; T=$(mktemp); cp ${VMB:-/tmp/b/vm}/tools/libscript $T; chmod +x $T; S=$1; N=${2:-1200}
for d in 1 0; do export SCRIPT_VM_DEFERRED=$d
python3 tools/js_jit_fuzz.py --cores $T $N $S | tail -2
SCRIPT_VM_TIER1=1 SCRIPT_VM_TIER2=2 python3 tools/js_jit_fuzz.py --cores $T $((N/2)) $((S+10000)) | tail -2
SCRIPT_VM_TIER1=1 SCRIPT_VM_TIER2=2 python3 tools/vm_fuzz.py $T $N $((S+20000)) | tail -2
done
rm -f $T
