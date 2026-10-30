#!/bin/sh
# builds the x86_64 test programs in here (static asm ones and dynamic C ones) and runs them thru FEXMacx
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

    if [ -f "$here/$name.c" ]; then
        build_command="clang -target x86_64-apple-macos12 -O1 -o $work_dir/$name $here/$name.c"
    else
        build_command="clang -target x86_64-apple-macos12 -nostdlib -static -Wl,-e,_start -o $work_dir/$name $here/$name.s"
    fi
    if ! $build_command; then
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
run_test dynamic_hello 3 "hello from a dynamic x86_64 program (3 args)" a b
run_test threads 0 "4 threads done, total 50000500000"
run_test gcd 0 "gcd done, total 18018000"
run_test gcd_timers 0 "dispatch_apply sum 2016
serial queue order 1023
dispatch_after fired
timer source ticks 5"
run_test signals 0 "fault fixed 1 value 42
skipped fault rax 1234 alt stack 1
alarm hits during busy loop 1
thread got usr2 1
blocked usr2 pending 1 delivered after unblock 1
caught ud2"

echo
echo "$passed passed $failed failed"
[ "$failed" -eq 0 ]
