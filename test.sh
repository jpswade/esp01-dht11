#!/bin/sh
set -eu
PROJECT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
TEST_BINARY=$(mktemp "${TMPDIR:-/tmp/}esp01-history-test.XXXXXX")
trap 'rm -f "$TEST_BINARY"' EXIT HUP INT TERM
c++ -std=c++11 -Wall -Wextra -pedantic "$PROJECT_DIR/tests/history_test.cpp" -o "$TEST_BINARY"
"$TEST_BINARY"
c++ -std=c++11 -Wall -Wextra -pedantic "$PROJECT_DIR/tests/restart_record_test.cpp" -o "$TEST_BINARY"
"$TEST_BINARY"
node "$PROJECT_DIR/tests/chart_test.cjs"
c++ -std=c++11 -Wall -Wextra -pedantic "$PROJECT_DIR/tests/history_import_test.cpp" -o "$TEST_BINARY"
"$TEST_BINARY"
python3 -B -m unittest discover -s "$PROJECT_DIR/tests" -p 'test_*.py'
