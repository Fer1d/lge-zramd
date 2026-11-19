#!/bin/bash
# Host-only independent main/event integration suite; callable by the main build.
set -euo pipefail
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD=${NZM_MAIN_EVENT_BUILD:-$(mktemp -d /tmp/nzm-main-event-build-XXXXXXXX)}
mkdir -p "$BUILD"
printf 'main-event artifacts=%s\n' "$BUILD"
ulimit -c 0
CC=${CC:-cc}
FLAGS=(-std=c11 -Wall -Wextra -Werror -g -pthread -D_FILE_OFFSET_BITS=64 -I"$ROOT/src" -I"$ROOT/tests")
CORE=(core lifecycle backend dm_backend demand_watch demand_exec swap vm metrics policy executor execution intent event_daemon diagnostics)
MODULES=(planner config ledger helper request_backend runtime psi spawn_fd)
ADAPTER="$ROOT/tests/event_adapter.h"
export ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=1:abort_on_error=1}
export UBSAN_OPTIONS=${UBSAN_OPTIONS:-halt_on_error=1:print_stacktrace=1}
for VARIANT in ${NZM_MAIN_EVENT_VARIANTS:-plain sanitized tsan}; do
    EXTRA=()
    case "$VARIANT" in
        plain) ;;
        sanitized) EXTRA=(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie);;
        tsan) EXTRA=(-fsanitize=thread -fno-omit-frame-pointer -fno-pie -no-pie);;
        *) printf 'Unknown variant %s\n' "$VARIANT" >&2; exit 2;;
    esac
    OBJECTS=()
    for SRC in "${CORE[@]}"; do
        OBJ="$BUILD/$SRC.$VARIANT.o"
        "$CC" "${FLAGS[@]}" "${EXTRA[@]}" -D__ANDROID__ -I"$ROOT/tests/android-stub" "-DNZM_CORE_ADAPTER=\"$ADAPTER\"" -c "$ROOT/src/$SRC.c" -o "$OBJ"
        OBJECTS+=("$OBJ")
    done
    for SRC in io recovery; do
        OBJ="$BUILD/$SRC.$VARIANT.o"
        "$CC" "${FLAGS[@]}" "${EXTRA[@]}" "-DNZM_${SRC^^}_ADAPTER=\"$ADAPTER\"" -c "$ROOT/src/$SRC.c" -o "$OBJ"
        OBJECTS+=("$OBJ")
    done
    "$CC" "${FLAGS[@]}" "${EXTRA[@]}" -Dmain=nzm_entry -Dgeteuid=test_geteuid -c "$ROOT/src/main.c" -o "$BUILD/main.$VARIANT.o"
    SOURCES=(); for SRC in "${MODULES[@]}"; do SOURCES+=("$ROOT/src/$SRC.c"); done
    "$CC" "${FLAGS[@]}" "${EXTRA[@]}" "$ROOT/tests/test_main_event.c" "$BUILD/main.$VARIANT.o" "${OBJECTS[@]}" "${SOURCES[@]}" -Wl,--wrap=close -Wl,--wrap=read -Wl,--wrap=pthread_join -Wl,-z,now -o "$BUILD/test_main_event.$VARIANT"
    if [[ "$VARIANT" == tsan ]]; then
        # This WSL host is independently known to reject TSan's address layout.
        # Compile is useful; unsupported execution is SKIP, never suite PASS.
        if [[ -f "$ROOT/build/tsan-probe.log" ]] && grep -q '^FATAL: ThreadSanitizer: unexpected memory mapping' "$ROOT/build/tsan-probe.log"; then
            printf '%s\n' 'SKIP TSan execution: existing independent host probe reports unexpected memory mapping; suite compiled only.' | tee "$BUILD/tsan-status.log"
            continue
        fi
    fi
    timeout 30s "$BUILD/test_main_event.$VARIANT" --phase1 > "$BUILD/phase1.$VARIANT.log" 2>&1 || {
        printf 'FAIL phase1 %s; log=%s/phase1.%s.log\n' "$VARIANT" "$BUILD" "$VARIANT" >&2; exit 1;
    }
    grep '^PASS phase1 ' "$BUILD/phase1.$VARIANT.log"
    ARGS=(); if [[ -n "${NZM_MAIN_EVENT_CASE:-}" ]]; then ARGS+=("$NZM_MAIN_EVENT_CASE"); fi
    timeout 180s "$BUILD/test_main_event.$VARIANT" "${ARGS[@]}" > "$BUILD/$VARIANT.log" 2>&1 || {
        printf 'FAIL main-event %s; log=%s/%s.log\n' "$VARIANT" "$BUILD" "$VARIANT" >&2; exit 1;
    }
    grep -E '^(PASS main-event|main-event:)' "$BUILD/$VARIANT.log"
done
if [[ ${NZM_MAIN_EVENT_ANDROID:-1} == 1 ]]; then
    NDK=${NDK:?Set NDK to the Android NDK r27c directory}
    LLVM="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
    SOURCES=("$ROOT/src/main.c")
    for SRC in "${CORE[@]}" "${MODULES[@]}" io recovery; do SOURCES+=("$ROOT/src/$SRC.c"); done
    "$LLVM/aarch64-linux-android29-clang" "${FLAGS[@]}" -O2 -fPIE -pie -ffunction-sections -fdata-sections -Wl,-z,now,--gc-sections "${SOURCES[@]}" -o "$BUILD/native-zram-manager.android29.arm64"
    "$LLVM/llvm-readelf" -h -l -d "$BUILD/native-zram-manager.android29.arm64" > "$BUILD/android29-readelf.log"
    grep -q AArch64 "$BUILD/android29-readelf.log"
    grep -q BIND_NOW "$BUILD/android29-readelf.log"
    printf '%s\n' 'PASS API29 arm64 production compile only; Android binary NOT executed.'
fi
sha256sum "$ROOT/tests/test_main_event.c" "$ADAPTER" "$ROOT/tests/build-main-event.sh" "$BUILD"/test_main_event.* > "$BUILD/SHA256SUMS"
printf 'main-event completed; artifacts=%s\n' "$BUILD"
