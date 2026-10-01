# JNA mmap attach experiment

Standalone proof of concept for a future async-profiler integration. This does not add an asprof profiling mode. Target: RHEL 9 x86_64 with JNA 5.8.0. Locally tested on macOS ARM64, JDK 21.0.4; Linux validation remains pending.

## Build and run

Requires a JDK 11+ (17/21 recommended), C compiler, Python 3, and curl for the initial dependency download. From this directory:

```sh
export JAVA_HOME=/path/to/jdk
bash fetch-deps.sh
bash build.sh
python3 check.py
```

For an offline build, copy `jna-5.8.0.jar` and `asm-9.7.1.jar` into deps/ first. fetch-deps.sh verifies pinned checksums and downloads only missing JARs. Dependencies and generated binaries are not committed.

The test creates a JNA Proxy and calls mmap/munmap BEFORE attaching an agent from a second JVM. It checks exact addresses/lengths, partial unmapping, failed calls and errno, and recording stop. A successful run ends in `PASS: prewarmed attach...`.

```sh
mv build/events.tsv build/events-warm.tsv
python3 check.py cold
mv build/events.tsv build/events-cold.tsv
python3 check.py restart
```

Cold tests JNA loading after attach; restart tests start/stop/start. Archive events.tsv between tests. The test refuses to overwrite an existing log. It launches and manages only its own disposable JVM.

## Attach to a test application

Use the target JVM's owner and PID/mount namespace. The absolute build directory must be visible and writable to the target. Use a separate directory for each JVM. With JNA 5.8.0 interface mapping returning long for mmap and int for munmap:

```sh
PROBE_BUILD="$(pwd)/build"
TARGET_PID=12345  # replace with your test JVM PID
"$JAVA_HOME/bin/java" --add-modules jdk.attach -cp "$PROBE_BUILD/app-classes" \
  probe.Attach "$TARGET_PID" "$PROBE_BUILD"
# Trigger actual slab allocation/release, then stop:
"$JAVA_HOME/bin/java" --add-modules jdk.attach -cp "$PROBE_BUILD/app-classes" \
  probe.Attach "$TARGET_PID" "$PROBE_BUILD" stop
```

Events append to build/events.tsv. Stop removes the transformer and retransforms Function; the helper and native library remain loaded. Repeated start uses the original log directory. Attach must be permitted by the JVM. `loaded Function classes=0` is expected if JNA loads later; look for a later `transformed Function` message. `TRANSFORM FAILED` means coverage is not established.

## Mechanism and limits

An Instrumentation agent inserts a long-to-long address redirect after peer reads in JNA Function's dispatch method. A bootstrap helper redirects only matching libc mmap/munmap addresses to JNI-library C wrappers. The cached Function.peer is not changed. Wrappers call the original function and preserve errno around synchronous TSV logging. ASM is a prototype dependency, not a proposed requirement for async-profiler itself.

No JFR, stacks, production leak analysis, mmap Pointer return, JNA direct mapping, mmap calls inside other C functions, raw syscalls or mremap coverage. Other FFI calls through the transformed method incur redirect overhead but are not logged. A custom symbol provider or interposed libc requires separate validation.

TSV columns: sequence, monotonic_ns, pthread ID, operation, address, requested length, flags, result, errno. mmap address is the returned pointer; -1 result denotes failure. munmap result 0 denotes success. errno is meaningful on failure only. Sequence is log order, not a guaranteed cross-thread mapping order. Logging uses mutex/fflush and is intended for short experiments. In-flight operations and existing frames at start/stop are not an atomic snapshot. I/O loss handling and concurrent ordering remain future work.

## Dependencies

JNA 5.8.0: https://github.com/java-native-access/jna/tree/5.8.0 (dual LGPL / Apache 2.0; license files in deps/).
ASM 9.7.1: https://asm.ow2.io/ (BSD license in deps/).
jna-platform is not needed for this experiment. No proprietary application sources are included.
