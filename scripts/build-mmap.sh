#!/usr/bin/env bash
# Offline build of the fork, including asprof, the mmap helper and jfrconv.
set -euo pipefail
cd "$(dirname "$0")/.."
: "${JAVA_HOME:?Set JAVA_HOME to your JDK installation (17 or 21 recommended)}"
for tool in make cc c++; do
  command -v "$tool" >/dev/null || { echo "Missing build tool: $tool" >&2; exit 1; }
done
[ -x "$JAVA_HOME/bin/javac" ] || { echo 'JAVA_HOME must point to a JDK, not a JRE' >&2; exit 1; }
make -j"${BUILD_JOBS:-4}" JAVA_HOME="$JAVA_HOME" STATIC_CPP_RUNTIME="${STATIC_CPP_RUNTIME:-true}"
./build/bin/asprof --version
printf '\nReady: build/bin/asprof and build/bin/jfrconv\nSee docs/MmapProfiling.md for attach and conversion commands.\n'
