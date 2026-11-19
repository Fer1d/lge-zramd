#!/bin/bash
set -euo pipefail
ROOT=$(dirname "$(readlink -f "$0")")
B="$ROOT/build/demand"
mkdir -p "$B"
FLAGS=(-std=gnu11 -Wall -Wextra -Werror -pthread -D_FILE_OFFSET_BITS=64 -I"$ROOT/src" -ffunction-sections -fdata-sections -Wl,--gc-sections,-z,now)
source "$ROOT/sources.sh"
SOURCES=(); for f in "${NZM_PRODUCTION_MODULES[@]}"; do [[ $f == main ]] || SOURCES+=("$ROOT/src/$f.c"); done
LEGACY_SOURCES=("${SOURCES[@]}"); for f in "${NZM_LEGACY_MODULES[@]}"; do LEGACY_SOURCES+=("$ROOT/src/$f.c"); done
for variant in plain sanitized; do
 E=(); [[ $variant != sanitized ]] || E=(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie)
 cc "${FLAGS[@]}" "${E[@]}" "$ROOT/tests/test_boot_state.c" "$ROOT/src/boot_state.c" -o "$B/boot-state.$variant"
 "$B/boot-state.$variant"
 cc "${FLAGS[@]}" "${E[@]}" "$ROOT/tests/test_wear_budget.c" "$ROOT/src/wear_budget.c" "$ROOT/src/ledger.c" -o "$B/budget.$variant"
 "$B/budget.$variant"
 cc "${FLAGS[@]}" "${E[@]}" -DNZM_LEGACY_TEST "$ROOT/tests/test_wear_store.c" "${LEGACY_SOURCES[@]}" -o "$B/store.$variant"
 "$B/store.$variant"
 cc "${FLAGS[@]}" "${E[@]}" "$ROOT/tests/test_memory_budget.c" "$ROOT/src/memory_budget.c" -o "$B/memory.$variant"
 "$B/memory.$variant"
 cc "${FLAGS[@]}" "${E[@]}" "$ROOT/tests/test_demand_exec.c" "${SOURCES[@]}" -o "$B/exec.$variant"
 "$B/exec.$variant"
 cc "${FLAGS[@]}" "${E[@]}" "$ROOT/tests/test_demand_watch.c" "$ROOT/src/demand_watch.c" -o "$B/watch.$variant"
 "$B/watch.$variant"
 cc "${FLAGS[@]}" "${E[@]}" "$ROOT/tests/test_config_ledger.c" "$ROOT/src/config.c" "$ROOT/src/ledger.c" -o "$B/config.$variant"
 "$B/config.$variant" "$ROOT/lge-zram.conf"
 "$B/config.$variant" "$ROOT/tests/legacy-fixture.conf"
 cc "${FLAGS[@]}" "${E[@]}" "$ROOT/tests/test_config_snapshot.c" "$ROOT/src/diagnostics.c" "$ROOT/src/config.c" -o "$B/snapshot.$variant"
 "$B/snapshot.$variant"
done
NDK=${NDK:?Set NDK to the Android NDK r27c directory}
LLVM="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
for mode in disarmed candidate; do
 E=(); [[ $mode != candidate ]] || E=(-DNZM_DEMAND_RUNTIME_QUALIFIED)
 "$LLVM/aarch64-linux-android29-clang" "${FLAGS[@]}" "${E[@]}" -O2 -fPIE -pie "$ROOT/src/main.c" "${SOURCES[@]}" -llog -o "$B/lge-zramd-$mode.android29.arm64"
done
printf '%s\n' 'PASS demand/config transaction and parser suites; API29 disarmed and opt-in candidates built; not deployed'
