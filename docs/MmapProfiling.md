# Experimental JNA mmap profiling

This fork adds `-e mmap` / `--mmap` to **asprof itself** and `--mmap` to **jfrconv**. It records successful `mmap` and `munmap` calls through JNA interface mapping, with Java allocation stacks. It requires JFR output. This is an experimental implementation for JNA 5.8.0's long-returning libc interface, targeting RHEL 9 x86_64. The integrated implementation has been tested locally on macOS ARM64; RHEL validation of this implementation remains pending.

## Offline build

The integrated agent and converter have no new external dependencies: neither ASM nor the prototype's JARs are needed to build them. JNA is supplied by the target application. Use a JDK (17/21 recommended) and a C/C++ toolchain:

```sh
git switch feature/jna-mmap-tracking
export JAVA_HOME=/path/to/jdk
bash scripts/build-mmap.sh
```

If Linux linking fails with `cannot find -lstdc++` and a message about the static library, the default build requires a static C++ runtime archive that may be missing from the installed toolchain. To use the installed shared C++ runtime instead (no package download):

```sh
STATIC_CPP_RUNTIME=false bash scripts/build-mmap.sh
```

For a direct make invocation, pass `STATIC_CPP_RUNTIME=false`. The default remains static linkage. When changing this option after an already successful build, remove only `build/lib/libasyncProfiler.so` to force relinking; Make does not track flag changes. A failed link is rebuilt automatically.

Dynamic linkage requires the corresponding shared `libstdc++.so.6` and libgcc runtime on the target machine. Build with the target machine's toolchain. If the toolchain cannot link even a dynamic C++ program, its development libraries must be supplied offline; this option cannot replace missing shared libraries.

Build on RHEL for RHEL. A macOS dylib is not a Linux shared library.

## Record and convert

Run from the repository root, as the JVM's owner. The absolute output directory must exist and be writable by the target process. Stop the previous standalone prototype before using this mode.

```sh
TARGET_PID=12345  # replace with your test application's PID
RECORDING="$PWD/mappings.jfr"
./build/bin/asprof start -e mmap -f "$RECORDING" "$TARGET_PID"
# Trigger your workload that creates/releases slabs.
./build/bin/asprof stop "$TARGET_PID"

./build/bin/jfrconv --mmap --total "$RECORDING" allocated.html
./build/bin/jfrconv --mmap --leak --tail 0 --total "$RECORDING" remaining.html
```

`allocated.html` shows cumulative successful mapped bytes by Java call stack. `remaining.html` shows bytes from observed allocations that were not unmapped by the end of the recording. Partial unmaps subtract only their overlapping ranges; a middle unmap can split an allocation into two fragments without counting it as two allocations. Without `--total`, leak output counts original allocations with a nonempty remainder.

`--tail 0` includes all allocation ages. Omitting it preserves the converter's existing default of ignoring the last 10% of allocations by recording-event time. Remaining mappings are candidates for investigation, not proof of a leak. They describe virtual memory ranges, not RSS or live objects within a slab.

To record malloc alongside mappings:

```sh
./build/bin/asprof start --mmap --nativemem 0 -f "$RECORDING" "$TARGET_PID"
# Run workload, then stop as above.
./build/bin/jfrconv --nativemem --leak --total "$RECORDING" malloc.html
./build/bin/jfrconv --mmap --leak --tail 0 --total "$RECORDING" remaining.html
```

The layers are kept separate to avoid counting allocator backing memory twice. `--nofree` affects malloc only. mmap operations are not byte-sampled or rate-limited. `--all` does not implicitly enable mmap.

## Mechanism and coverage

The native JVMTI agent retransforms `com.sun.jna.Function`. In its dispatch method, a three-byte `getfield peer:J` is replaced with a three-byte call to the embedded `MmapBridge.address(Object):long`. Stack shape and instruction lengths are unchanged. The native helper reads the original pointer and redirects recognized libc addresses to wrappers; Function.peer and JNA's function cache remain unchanged. No additional Java agent or external bytecode library is required. Already-loaded and later-loaded Function classes are supported. Stop disables recording and retransforms loaded classes back; the small helper remains available for in-flight old frames.

The wrappers preserve syscall return values and errno, then record `profiler.Mmap` or `profiler.Munmap` using async-profiler's stack/JFR machinery. Calls through this backend are serialized through the syscall and recording to preserve address reuse order between participating threads. This can add overhead on workloads with frequent mappings. Java stacks are collected synchronously; native stack reconstruction is not part of this first backend.

Boundaries:

- This backend covers JNA **interface mapping** with `long mmap(...)` and `int munmap(...)`, as in the tested interface. Pointer-returning mappings, JNA direct mapping, calls from inside a separate C function, plain C imports, other FFI, direct syscalls and mremap are not covered.
- `madvise` does not close a virtual mapping and is not treated as munmap.
- Only operations observed after activation are attributed. Existing mappings, operations in active old frames during retransformation and mutations through unobserved callers cannot be reconstructed. Start/stop is not an atomic snapshot of all process mappings.
- Standard-page mappings, partial unmapping, successful replacement at an existing address and address reuse are supported. Explicit `MAP_HUGETLB` mappings are currently marked unsupported; they invalidate a complete mapping report. Transparent huge pages retain ordinary mmap range semantics.
- Unsupported bytecode and recording drops are counted in the JFR `mmapDroppedEvents` setting. The converter refuses mmap output if the final counter is nonzero. A zero counter does **not** establish coverage of unobserved APIs.
- Supported converter outputs: HTML and collapsed. Time/latency/tag/state filters, diff and combining several profile selections in one conversion are rejected. Stack include/exclude filtering remains available after lifecycle matching.
- JFR synchronization with a JDK recording and simultaneous method tracing are not supported in this mode. Sampling modes and nativemem may be recorded alongside it.
- Nested callbacks/signals and custom symbol providers/interposers require further validation. This is a feature-branch experiment, not a general OS mapping monitor.

## Tests

No network is used by these commands. The integration test alone needs a JNA 5.8.0 JAR (the one already transferred for the prototype is suitable):

```sh
export JNA_JAR=/path/to/jna-5.8.0.jar
python3 test/mmap/check.py
python3 test/mmap/check.py cold
python3 test/mmap/check.py combined
python3 test/mmap/check.py concurrent
python3 test/mmap/check.py restart
python3 test/mmap/check.py chunks
```

These tests drive the real asprof CLI, record JFR, generate HTML/collapsed reports, and verify exact mapped/remaining byte counts and Java attribution. The concurrent case uses four threads with repeated partial unmaps; the chunks case verifies matching a partial unmap against an allocation from an earlier JFR chunk. Artifacts are under `build/test/mmap/<case>/`.

Converter range tests:

```sh
mkdir -p build/test/mmap
"$JAVA_HOME/bin/javac" --release 8 -cp build/jar/jfr-converter.jar \
  -d build/test/mmap test/mmap/MappingConverterTest.java
"$JAVA_HOME/bin/java" -cp build/test/mmap:build/jar/jfr-converter.jar MappingConverterTest
```

Existing regressions: `make test-cpp` and `make test-java TESTS=nativemem,instrument,jfr TEST_THREADS=2`, with JAVA_HOME set.
