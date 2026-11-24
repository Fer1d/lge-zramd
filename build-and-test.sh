#!/bin/bash
set -euo pipefail
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$ROOT"
ulimit -c 0
NDK=${NDK:?Set NDK to the Android NDK r27c directory}
LLVM="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
BUILD="$ROOT/build"
mkdir -p "$BUILD"
CC=${CC:-cc}
FLAGS=(-std=c11 -Wall -Wextra -Werror -g -pthread -D_FILE_OFFSET_BITS=64 -I"$ROOT/src" -Wl,-z,now)
RBWARN=(-Wconversion -Wshadow -Wstrict-prototypes -Wmissing-prototypes)
source "$ROOT/sources.sh"
CORE_MODULES=(core lifecycle state_bootstrap backend backend_prepare dm_backend demand_watch demand_exec swap vm metrics policy executor execution intent event_daemon diagnostics boot_state memory_budget wear_budget wear_store log)
CORE_SOURCES=()
for SRC in "${CORE_MODULES[@]}"; do CORE_SOURCES+=("$ROOT/src/$SRC.c"); done
MODULES=("$ROOT/src/spawn_fd.c" "$ROOT/src/request_backend.c" "$ROOT/src/runtime.c" "$ROOT/src/psi.c")
SOURCES=(); for SRC in "${NZM_PRODUCTION_MODULES[@]}"; do SOURCES+=("$ROOT/src/$SRC.c"); done
LEGACY_FLAGS=(-DNZM_LEGACY_TEST)
export ASAN_OPTIONS=detect_leaks=1:abort_on_error=1
export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
PRODUCTION=(-ffunction-sections -fdata-sections -Wl,--gc-sections)
printf '%s\n' 'Demand production fixtures and explicit historical module regressions; no device operations.'
bash "$ROOT/build-demand.sh"
"$CC" "${FLAGS[@]}" "${PRODUCTION[@]}" "${SOURCES[@]}" -o "$BUILD/native-zram-manager.host"
"$CC" "${FLAGS[@]}" "$ROOT/tests/exec_fixture.c" -o "$BUILD/exec_fixture"
"$CC" "${FLAGS[@]}" -DIMPOSTOR "$ROOT/tests/exec_fixture.c" -o "$BUILD/impostor"
for SAN in ${NZM_TEST_VARIANTS:-plain sanitized}; do
    EXTRA=()
    if [[ "$SAN" == sanitized ]]; then EXTRA=(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie); fi
    "$CC" "${FLAGS[@]}" "${EXTRA[@]}" "$ROOT/tests/test_dm_backend.c" "$ROOT/src/dm_backend.c" -o "$BUILD/test_dm_backend.$SAN"
    "$BUILD/test_dm_backend.$SAN"
    "$CC" "${FLAGS[@]}" "${EXTRA[@]}" "$ROOT/tests/test_planner.c" "$ROOT/src/planner.c" "$ROOT/src/config.c" -o "$BUILD/test_planner.$SAN"
    "$BUILD/test_planner.$SAN"
    "$CC" "${FLAGS[@]}" "${EXTRA[@]}" "${LEGACY_FLAGS[@]}" -ffunction-sections -fdata-sections "$ROOT/tests/test_event_deadline.c" "$ROOT/src/event_daemon.c" "$ROOT/src/config.c" "$ROOT/src/runtime.c" "$ROOT/src/psi.c" "$ROOT/src/spawn_fd.c" -Wl,--gc-sections -o "$BUILD/test_event_deadline.$SAN"
    "$BUILD/test_event_deadline.$SAN"
    "$CC" "${FLAGS[@]}" "${EXTRA[@]}" "$ROOT/tests/test_config_ledger.c" "$ROOT/src/config.c" "$ROOT/src/ledger.c" -o "$BUILD/test_config_ledger.$SAN"
    "$BUILD/test_config_ledger.$SAN" "$ROOT/tests/user-profile.conf"
    # Single-threaded original fixture assertions, updated frozen execve boundary.
    "$CC" "${FLAGS[@]}" "${EXTRA[@]}" "$ROOT/tests/test_io_helper.c" "$ROOT/src/spawn_fd.c" -o "$BUILD/test_io_helper.$SAN"
    "$BUILD/test_io_helper.$SAN"
    "$CC" "${FLAGS[@]}" "${EXTRA[@]}" -ffunction-sections -fdata-sections "$ROOT/tests/test_directory.c" -Wl,--gc-sections -o "$BUILD/test_directory.$SAN"
    "$BUILD/test_directory.$SAN"
    "$CC" "${FLAGS[@]}" "${EXTRA[@]}" "$ROOT/tests/test_recovery.c" -o "$BUILD/test_recovery.$SAN"
    "$BUILD/test_recovery.$SAN"
    "$CC" "${FLAGS[@]}" "${EXTRA[@]}" "$ROOT/tests/test_mounts.c" -o "$BUILD/test_mounts.$SAN"
    "$BUILD/test_mounts.$SAN"
    CORE_OBJECTS=()
    for SRC in "${CORE_MODULES[@]}"; do
        "$CC" "${FLAGS[@]}" "${EXTRA[@]}" "${LEGACY_FLAGS[@]}" "-DNZM_CORE_ADAPTER=\"$ROOT/tests/core_adapter.h\"" -c "$ROOT/src/$SRC.c" -o "$BUILD/$SRC.$SAN.o"
        CORE_OBJECTS+=("$BUILD/$SRC.$SAN.o")
    done
    for SRC in io recovery; do
        "$CC" "${FLAGS[@]}" "${EXTRA[@]}" "${LEGACY_FLAGS[@]}" "-DNZM_${SRC^^}_ADAPTER=\"$ROOT/tests/core_adapter.h\"" -c "$ROOT/src/$SRC.c" -o "$BUILD/$SRC.$SAN.o"
    done
    "$CC" "${FLAGS[@]}" "${EXTRA[@]}" "${LEGACY_FLAGS[@]}" "$ROOT/src/planner.c" "$ROOT/tests/test_core.c" "${CORE_OBJECTS[@]}" "$BUILD/io.$SAN.o" "$ROOT/src/config.c" "$ROOT/src/ledger.c" "$ROOT/src/helper.c" "$BUILD/recovery.$SAN.o" "${MODULES[@]}" -o "$BUILD/test_core.$SAN"
    "$BUILD/test_core.$SAN"
    "$CC" "${FLAGS[@]}" "${EXTRA[@]}" -Dmain=nzm_entry -Dgeteuid=test_geteuid -c "$ROOT/src/main.c" -o "$BUILD/main.entry.$SAN.o"
    CORE_INTEGRATION_OBJECTS=()
    for SRC in "${CORE_MODULES[@]}"; do
        "$CC" "${FLAGS[@]}" "${EXTRA[@]}" "${LEGACY_FLAGS[@]}" -D__ANDROID__ -DNZM_INTEGRATION -I"$ROOT/tests/android-stub" "-DNZM_CORE_ADAPTER=\"$ROOT/tests/core_adapter.h\"" -c "$ROOT/src/$SRC.c" -o "$BUILD/$SRC.integration.$SAN.o"
        CORE_INTEGRATION_OBJECTS+=("$BUILD/$SRC.integration.$SAN.o")
    done
    "$CC" "${FLAGS[@]}" "${EXTRA[@]}" "${LEGACY_FLAGS[@]}" -DNZM_INTEGRATION "$ROOT/src/planner.c" "$ROOT/tests/test_core.c" "$ROOT/src/helper.c" "$BUILD/main.entry.$SAN.o" "${CORE_INTEGRATION_OBJECTS[@]}" "$BUILD/io.$SAN.o" "$ROOT/src/config.c" "$ROOT/src/ledger.c" "$BUILD/recovery.$SAN.o" "${MODULES[@]}" -o "$BUILD/test_main_success.$SAN"
    "$BUILD/test_main_success.$SAN"
    # Frozen runtime tests: actual worker/eventloop, including create failure and FD reuse.
    "$CC" "${FLAGS[@]}" "${EXTRA[@]}" "$ROOT/tests/runtime_test.c" "$ROOT/src/runtime.c" "$ROOT/src/psi.c" "$ROOT/src/spawn_fd.c" -Wl,--wrap=pthread_create,--wrap=close,--wrap=eventfd -o "$BUILD/runtime_test.$SAN"
    timeout 90s "$BUILD/runtime_test.$SAN"
    # Frozen request transport suite and both include-order ABI fixtures.
    "$CC" "${FLAGS[@]}" "${RBWARN[@]}" "${EXTRA[@]}" "$ROOT/tests/request_backend_test.c" "$ROOT/src/request_backend.c" -o "$BUILD/request_backend_test.$SAN"
    "$BUILD/request_backend_test.$SAN"
    for ORDER in 0 1; do
        "$CC" "${FLAGS[@]}" "${RBWARN[@]}" "${EXTRA[@]}" -DRB_UAPI_FIRST="$ORDER" "$ROOT/tests/header_fixture.c" -o "$BUILD/header_fixture.$ORDER.$SAN"
        "$BUILD/header_fixture.$ORDER.$SAN"
    done
    # Independent threaded request fixture; never share the serial core globals.
    "$CC" "${FLAGS[@]}" "${EXTRA[@]}" "$ROOT/tests/test_execution_gate.c" "$ROOT/src/request_backend.c" "$ROOT/src/spawn_fd.c" "$ROOT/src/planner.c" "$ROOT/src/config.c" -o "$BUILD/test_execution_gate.$SAN"
    timeout 30s "$BUILD/test_execution_gate.$SAN"
    # A dedicated real-multithread suite, NEVER parallelize the old fixture.
    "$CC" "${FLAGS[@]}" "${EXTRA[@]}" "$ROOT/tests/test_safe_spawn.c" "$ROOT/src/spawn_fd.c" -o "$BUILD/test_safe_spawn.$SAN"
    timeout 120s "$BUILD/test_safe_spawn.$SAN" "$BUILD/exec_fixture" "$BUILD/impostor"
done
# TSan remains independent. A failed host runtime probe is NOT a suite PASS.
TSAN=(-fsanitize=thread -fno-omit-frame-pointer -fno-pie -no-pie)
"$CC" "${FLAGS[@]}" "${TSAN[@]}" "$ROOT/tests/tsan_probe.c" -o "$BUILD/tsan_probe"
for TEST in test_io_helper test_safe_spawn; do
    "$CC" "${FLAGS[@]}" "${TSAN[@]}" "$ROOT/tests/$TEST.c" "$ROOT/src/spawn_fd.c" -o "$BUILD/$TEST.tsan"
done
"$CC" "${FLAGS[@]}" "${TSAN[@]}" "$ROOT/tests/test_execution_gate.c" "$ROOT/src/request_backend.c" "$ROOT/src/spawn_fd.c" "$ROOT/src/planner.c" "$ROOT/src/config.c" -o "$BUILD/test_execution_gate.tsan"
"$CC" "${FLAGS[@]}" "${TSAN[@]}" "$ROOT/tests/runtime_test.c" "$ROOT/src/runtime.c" "$ROOT/src/psi.c" "$ROOT/src/spawn_fd.c" -Wl,--wrap=pthread_create,--wrap=close,--wrap=eventfd -o "$BUILD/runtime_test.tsan"
set +e
timeout 20s "$BUILD/tsan_probe" > "$BUILD/tsan-probe.log" 2>&1
PROBE=$?
set -e
if (( PROBE == 0 )); then
    TSAN_UNSUPPORTED=0
    for TEST in test_io_helper test_safe_spawn test_execution_gate runtime_test; do
        ARGS=(); if [[ "$TEST" == test_safe_spawn ]]; then ARGS=("$BUILD/exec_fixture" "$BUILD/impostor"); fi
        set +e
        TSAN_OPTIONS=halt_on_error=1:exitcode=66 timeout 180s "$BUILD/$TEST.tsan" "${ARGS[@]}" > "$BUILD/$TEST.tsan.log" 2>&1
        RESULT=$?
        set -e
        if (( RESULT != 0 )); then
            if grep -q '^FATAL: ThreadSanitizer: unexpected memory mapping' "$BUILD/$TEST.tsan.log" &&
               ! grep -q 'WARNING: ThreadSanitizer:' "$BUILD/$TEST.tsan.log"; then
                TSAN_UNSUPPORTED=1
                printf 'SKIP TSan runtime unsupported in %s: exit=%s; suite NOT passed.\n' "$TEST" "$RESULT" > "$BUILD/tsan-status.txt"
                break
            fi
            printf 'FAIL TSan %s exit=%s; inspect suite log\n' "$TEST" "$RESULT"
            exit "$RESULT"
        fi
    done
    if (( TSAN_UNSUPPORTED == 0 )); then
        printf '%s\n' 'PASS TSan (helper, execution gate and runtime suites executed)' > "$BUILD/tsan-status.txt"
    fi
elif grep -q '^FATAL: ThreadSanitizer: unexpected memory mapping' "$BUILD/tsan-probe.log"; then
    printf 'SKIP TSan runtime unsupported: independent probe exit=%s; suites compiled, NOT passed.\n' "$PROBE" > "$BUILD/tsan-status.txt"
else
    printf 'FAIL TSan probe exit=%s\n' "$PROBE"; exit 1
fi
TRACE=$(mktemp -d "$BUILD/trace-XXXXXXXX")
strace -ff -o "$TRACE/child" "$BUILD/test_safe_spawn.plain" "$BUILD/exec_fixture" "$BUILD/impostor" > "$BUILD/trace-run.log" 2>&1
python3 -I "$ROOT/tests/check_child_trace.py" "$TRACE" > "$BUILD/child-trace-check.log"
"$LLVM/aarch64-linux-android29-clang" "${FLAGS[@]}" "${PRODUCTION[@]}" -O2 -fPIE -pie "${SOURCES[@]}" -llog -o "$BUILD/native-zram-manager.android29.arm64"
"$LLVM/llvm-nm" --defined-only "$BUILD/native-zram-manager.android29.arm64" > "$BUILD/symbols-arm64.txt"
# Production must not depend on historical schedulers or persistent budgets.
if grep -Eq ' (nzm_(core_(boot|daemon|cycle)|event_daemon|runtime_create|execution_execute|wear|wear_store_[a-z_]+|budget_[a-z_]+|ledger_[a-z_]+|io_ledger_commit)|rb_execute)$' "$BUILD/symbols-arm64.txt"; then
    printf '%s\n' 'FAIL: legacy writeback/provisioning/runtime code in disarmed artifact' >&2
    exit 1
fi
"$LLVM/llvm-readelf" -h -l -d "$BUILD/native-zram-manager.android29.arm64" > "$BUILD/readelf-arm64.txt"
grep -q 'AArch64' "$BUILD/readelf-arm64.txt"
grep -q 'BIND_NOW' "$BUILD/readelf-arm64.txt"
"$LLVM/llvm-readelf" -h "$BUILD/native-zram-manager.android29.arm64"
# Real main with independent worker/eventloop-safe fixture; no serial globals.
NZM_MAIN_EVENT_BUILD="$BUILD/main-event" NZM_MAIN_EVENT_VARIANTS="${NZM_TEST_VARIANTS:-plain sanitized} tsan" NZM_MAIN_EVENT_ANDROID=0 bash "$ROOT/tests/build-main-event.sh"
printf '%s\n' 'Core/intent/deadline/gate, real-main event and frozen module suites plus API29 arm64 build passed; no device operations.'
