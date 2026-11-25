#!/bin/sh
# gate2.sh: ctest in five tier modes and SCRIPT_VM_SSA=0, each with deferred frames (young cells on), deferred frames
# without young cells, and counted frames; then every allocation scanning, in two memory modes; then default
cd ${VMB:-/tmp/b/vm}
for d in "SCRIPT_VM_DEFERRED=1" "SCRIPT_VM_DEFERRED=1 SCRIPT_VM_YOUNG=0" "SCRIPT_VM_DEFERRED=0"; do for m in "A=1" "SCRIPT_VM_TIER1=1 SCRIPT_VM_TIER2=1" "SCRIPT_VM_TIER1=0" "SCRIPT_VM_TIER1=1 SCRIPT_VM_TIER2=2" "SCRIPT_VM_LAZY=1" "SCRIPT_VM_SSA=0"; do
  echo "$d $m: $(env $d $m ctest -j18 2>&1 | grep -E 'tests passed|\*\*\*' | tr '\n' ' ')"; done; done
echo "stress: $(SCRIPT_VM_ZCT=0 SCRIPT_VM_TIER1=1 SCRIPT_VM_TIER2=2 ctest -j18 --timeout 900 -E soak 2>&1 | grep -E 'tests passed|\*\*\*' | tr '\n' ' ')"
echo "stress old: $(SCRIPT_VM_YOUNG=0 SCRIPT_VM_ZCT=0 SCRIPT_VM_TIER1=1 SCRIPT_VM_TIER2=2 ctest -j18 --timeout 900 -E soak 2>&1 | grep -E 'tests passed|\*\*\*' | tr '\n' ' ')"
echo "default: $(ctest -j18 2>&1 | grep -E 'tests passed')"
