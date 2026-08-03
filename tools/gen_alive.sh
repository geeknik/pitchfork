#!/usr/bin/env bash
# Regenerate tools/app_api_alive.txt — the reachability set the API generator
# filters against. Run after menus / hand-listed exports change (i.e. when the
# firmware's reachable function set changes). Builds once with the auto-export
# disabled, then records every defined function in that image.
set -e
cd "$(dirname "$0")/.."
M1_API_GEN_EMPTY=1 cmake --build build-exp --parallel
ELF=$(ls -t build-exp/M1OS_*.elf | head -1)
arm-none-eabi-nm --defined-only "$ELF" | awk '$2 ~ /^[TtWw]$/ {print $3}' | sort -u > tools/app_api_alive.txt
echo "alive.txt: $(wc -l < tools/app_api_alive.txt) symbols"
