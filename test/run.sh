#!/bin/sh
# Build and run the host tests. No hardware, no Arduino toolchain, about a second.
set -e
cd "$(dirname "$0")"
CXX="${CXX:-c++}"
FLAGS="-std=c++11 -Wall -Wextra -Wno-unused-parameter -I shim"
OUT="${TMPDIR:-/tmp}/scp_tests"
mkdir -p "$OUT"
rc=0

for t in bench amp_sim; do
  printf '\n=== %s ===\n' "$t"
  $CXX $FLAGS -DCONFIG_IDF_TARGET_ESP32 -o "$OUT/$t" "${t}_test.cpp"
  "$OUT/$t" || rc=1
done

# The sketches also have to build for a two-UART, native-USB part.
printf '\n=== cross-build for ESP32-H2 ===\n'
for t in bench amp_sim; do
  $CXX $FLAGS -DCONFIG_IDF_TARGET_ESP32H2 -c "${t}_test.cpp" -o /dev/null
  echo "  ${t}_test.cpp compiles for ESP32-H2"
done

# The web app's parsing and retry logic, if node is available.
if command -v node >/dev/null 2>&1; then
  printf '\n=== app (JavaScript) ===\n'
  node app_test.mjs || rc=1
else
  printf '\n(node not found: skipping the app tests)\n'
fi

printf '\n%s\n' "$([ $rc -eq 0 ] && echo 'ALL SUITES PASSED' || echo 'SOME SUITES FAILED')"
exit $rc
