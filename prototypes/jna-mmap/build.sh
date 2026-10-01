#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
: "${JAVA_HOME:?Set JAVA_HOME to a JDK 11 or newer (17/21 recommended)}"
ASM=deps/asm-9.7.1.jar
JNA=deps/jna-5.8.0.jar
mkdir -p build/agent-classes build/bridge-classes build/app-classes
"$JAVA_HOME/bin/javac" --release 8 -d build/bridge-classes src/probe/Bridge.java
"$JAVA_HOME/bin/jar" cf build/bridge.jar -C build/bridge-classes .
"$JAVA_HOME/bin/javac" --release 8 -cp "$ASM" -d build/agent-classes src/probe/Agent.java
(cd build/agent-classes && "$JAVA_HOME/bin/jar" xf ../../deps/asm-9.7.1.jar)
printf 'Manifest-Version: 1.0\nAgent-Class: probe.Agent\nCan-Retransform-Classes: true\n\n' > build/agent.mf
"$JAVA_HOME/bin/jar" cfm build/agent.jar build/agent.mf -C build/agent-classes .
"$JAVA_HOME/bin/javac" --release 8 -cp "$JNA" -d build/app-classes src/probe/Demo.java
"$JAVA_HOME/bin/javac" --add-modules jdk.attach -d build/app-classes src/probe/Attach.java
case "$(uname -s)" in
  Linux) os=linux; lib=libmmapprobe.so ;;
  Darwin) os=darwin; lib=libmmapprobe.dylib ;;
  *) echo 'Linux or macOS required' >&2; exit 1 ;;
esac
"${CC:-cc}" -std=c11 -O2 -Wall -Wextra -Werror -fPIC -shared \
    -I"$JAVA_HOME/include" -I"$JAVA_HOME/include/$os" native/probe.c \
    -o "build/$lib" -ldl -lpthread
printf 'Built in %s/build\n' "$PWD"
