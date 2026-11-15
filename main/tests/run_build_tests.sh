#!/bin/sh
# Build tests: every board's firmware compiles from clean project sources
# with no warnings, each firmware is its own, and the PC scripts parse.
# Nothing is uploaded.
#
#   sh tests/run_build_tests.sh              all five boards
#   sh tests/run_build_tests.sh board1       just these environments
set -u
cd "$(dirname "$0")/.."
envs="${*:-board1 board2 board3 board4 board5}"
pio="$(command -v pio || echo "$HOME/.platformio/penv/bin/pio")"
logs="$(mktemp -d)"
trap 'rm -rf "$logs"' EXIT
passed=0
failed=0

result()
{
    # result STATUS ID DESCRIPTION
    if [ "$1" -eq 0 ]; then
        passed=$((passed + 1))
        echo "PASS $2 $3"
    else
        failed=$((failed + 1))
        echo "FAIL $2 $3"
    fi
}

# T1 compiles and the firmware is made, T2 no warnings.
for env in $envs; do
    # Only the project's own objects, so its sources compile again (and any
    # warning shows) without rebuilding the framework and libraries.
    rm -rf ".pio/build/$env/src"
    "$pio" run -e "$env" >"$logs/$env.txt" 2>&1
    built=$?
    result "$built" T1 "$env compiles"
    if [ "$built" -ne 0 ]; then
        grep -E "error|Error" "$logs/$env.txt" | head -20
        continue
    fi
    # Warnings from this project's files only; libraries have their own.
    warnings="$(grep -E ": warning" "$logs/$env.txt" | grep -v "\.pio/" |
        grep -E "(^|/)(src|include)/" || true)"
    [ -z "$warnings" ]
    result $? T2 "$env: no warnings from src/ or include/"
    [ -n "$warnings" ] && echo "$warnings" | head -20
    # PlatformIO fails the build above if the firmware outgrows its
    # partition; this is that it was made, and how full the flash is.
    [ -s ".pio/build/$env/firmware.bin" ]
    result $? T1 "$env: firmware.bin made ($(grep -E "^Flash:" \
        "$logs/$env.txt" | sed -E 's/.*\] +//'))"
done

# T3 each board's firmware is its own (built for its ID).
previous=""
distinct=0
for env in $envs; do
    for other in $previous; do
        if cmp -s ".pio/build/$env/firmware.bin" \
            ".pio/build/$other/firmware.bin"; then
            distinct=1
            echo "  $env and $other are the same firmware"
        fi
    done
    previous="$previous $env"
done
result "$distinct" T3 "no two boards' firmware is the same"

# T4 the PC test scripts compile and the test shell scripts parse.
python3 -m py_compile tools/*.py tests/*.py >"$logs/python.txt" 2>&1
result $? T4 "tools/*.py and tests/*.py compile"
[ -s "$logs/python.txt" ] && cat "$logs/python.txt"
shells=0
for script in tests/*.sh; do
    sh -n "$script" || shells=1
done
result "$shells" T4 "tests/*.sh parse"

echo "$passed passed, $failed failed (build tests)"
[ "$failed" -eq 0 ]
