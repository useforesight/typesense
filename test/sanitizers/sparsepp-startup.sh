#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
build_dir=$(mktemp -d -t typesense-sparsepp-XXXXXX)
trap 'rm -rf "$build_dir"' EXIT

"${CXX:-g++}" -std=c++17 -g -O1 -fsanitize=thread -pthread \
  -I "$repo_root/include" "$repo_root/test/sanitizers/sparsepp_startup.cpp" \
  -o "$build_dir/sparsepp-startup"

# Each process exercises first-use initialization. A race report or incorrect
# map result fails the check; ThreadSanitizer exits with 66 on a race.
for attempt in {1..100}; do
  TSAN_OPTIONS=halt_on_error=1 "$build_dir/sparsepp-startup"
done
echo "Sparsepp concurrent initialization passed in 100 fresh processes."
