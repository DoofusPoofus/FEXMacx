#!/bin/sh
# builds the tiny static x86_64 test programs in here and runs them thru FEXMacx
# each one gets compared against the stdout and exit code macOS would give
# usage: unittests/MacOS/run_tests.sh [path to FEXMacx]

here=$(cd "$(dirname "$0")" && pwd)
fexmacx=${1:-$here/../../build/Bin/FEXMacx}
work_dir=$(mktemp -d)
trap 'rm -rf "$work_dir"' EXIT

passed=0
failed=0

run_test() {
    name=$1
    expected_exit=$2
    expected_output=$3
    shift 3

    if ! clang -target x86_64-apple-macos12 -nostdlib -static -Wl,-e,_start -o "$work_dir/$name" "$here/$name.s"; then
        echo "FAIL  $name (didnt assemble)"
        failed=$((failed + 1))
        return
    fi

    actual_output=$("$fexmacx" "$work_dir/$name" "$@")
    actual_exit=$?

    if [ "$actual_output" = "$expected_output" ] && [ "$actual_exit" = "$expected_exit" ]; then
        echo "PASS  $name"
        passed=$((passed + 1))
    else
        echo "FAIL  $name (exit $actual_exit want $expected_exit)"
        echo "      got:  $actual_output"
        echo "      want: $expected_output"
        failed=$((failed + 1))
    fi
}

run_test hello 7 "hello from x86_64 thru FEXMacx"
run_test args 4 "banana
carry set on EBADF ok" banana two three

echo
echo "$passed passed $failed failed"
[ "$failed" -eq 0 ]
