#!/usr/bin/env bash
# Decompile the functions containing the given addresses (read-only Ghidra run).
#   re/tools/decomp.sh 08a9fb88 0887dfc4
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
export JAVA_HOME=$HOME/tools/jdk-25.0.5+8
export PATH=$JAVA_HOME/bin:$PATH GHIDRA_HEADLESS_MAXMEM=4G
"$HOME/tools/ghidra_12.2_PUBLIC/support/analyzeHeadless" "$ROOT/re/ghidra_project" pangya \
	-process EBOOT_decrypted.elf -noanalysis -readOnly -scriptPath "$ROOT/re/ghidra_scripts" \
	-postScript DecompileAt.java "$@" 2>&1 |
	sed -n '/DecompileAt.java> ==/,/^INFO  REPORT/p' | sed -e 's/^INFO  DecompileAt.java> //' -e '/^INFO  REPORT/d' -e 's/ (GhidraScript)  $//'
