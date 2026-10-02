# Experimental mmap profiling

This fork adds `-e mmap` / `--mmap` to **asprof itself** and `--mmap` to **jfrconv**. The same option now records successful `mmap`, Linux `mmap64`, and `munmap` calls through native library imports, as well as the existing JNA 5.8.0 interface mapping path. Allocation stacks can contain Java and native frames. JFR output is required. The target is RHEL 9 x86_64; the libc extension has been tested locally on macOS ARM64 with JDK 21. RHEL validation remains pending.

## Offline build

The integrated agent and converter have no new external dependencies: neither ASM nor the prototype's JARs are needed to build them. JNA is only needed if the target application uses it. Use a JDK (17/21 recommended) and a C/C++ toolchain:

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

Transfer the updated source tree to the offline machine, including `src`, `scripts`, and `test/mmap`. The build script does not fetch dependencies or access the network. A Git pull is only needed on the connected machine used to obtain the sources. Use the freshly built asprof **and** jfrconv. Restart a JVM that has already loaded the previous agent binary before trying this revision: replacing the on-disk library does not upgrade the agent already loaded in that JVM.

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

`allocated.html` shows cumulative successful mapped bytes by allocation stack. `remaining.html` shows bytes from observed allocations that were not unmapped by the end of the recording. Partial unmaps subtract only their overlapping ranges; a middle unmap can split an allocation into two fragments without counting it as two allocations. Without `--total`, leak output counts original allocations with a nonempty remainder. Matching is global across threads: a Cleaner thread or native pthread can release a mapping created elsewhere.

The reports now also include mappings from JDK/JVM internals and imported calls in other native libraries. This is expected. To inspect FileChannel separately, a useful converter filter on supported OpenJDK builds is `--include 'Java_sun_nio_ch_.*_map0'`. Apply it to the converter, after global lifecycle matching. Closing a FileChannel alone does not prove that its mapped buffers have been unmapped. For debugging hook installation, add `--log debug` to asprof; successful patches identify the function and library. `JNA Function classes found: 0` is normal for a program without JNA and does not disable libc hooks.

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

The native agent patches mapping function imports in loaded libraries, using async-profiler's existing import patching and unload protection infrastructure. On Linux this includes mmap64 as well as mmap and munmap. New libraries are patched through the existing dlopen update paths. The profiler's own imports are excluded. Hooks remain installed after stop but forward directly to the original functions while inactive.

The existing JVMTI backend also retransforms `com.sun.jna.Function`. In its dispatch method, a three-byte `getfield peer:J` is replaced with a three-byte call to the embedded `MmapBridge.address(Object):long`. Stack shape and instruction lengths are unchanged. The helper reads the original pointer and redirects recognized libc addresses to the same wrappers; Function.peer and JNA's function cache remain unchanged. Already-loaded and later-loaded Function classes are supported. Stop disables recording and retransforms loaded classes back; the helper remains available for in-flight old frames. No additional Java agent or external bytecode library is required.

The wrappers preserve function return values and errno, then record `profiler.Mmap` or `profiler.Munmap`. Allocation stack walking uses the native/asynchronous path used for malloc events, without synchronous JVMTI GetStackTrace. Unmap events do not collect a stack. Calls through the wrappers are serialized through the syscall and recording to preserve address reuse order between participating threads. This adds overhead on workloads with frequent mappings; validate overhead and JVM stability on the target workload. A thread-local reentrancy guard suppresses nested recording, including profiler bookkeeping; it does not associate ownership of a mapping with a thread.

Boundaries:

- Native calls are covered when they go through patched imports. This includes the tested FileChannel map/unmap path and calls from inside JNI libraries. Hidden/internal libc calls, direct syscalls, arbitrary cached dlsym pointers and static linkage can bypass these hooks. Calls from library constructors before hooks are installed may be missed. Coverage depends on the actual JDK/library binary.
- The extra JNA backend covers **interface mapping** with `long mmap(...)` and `int munmap(...)`, as in the tested interface. Pointer-returning interfaces and JNA direct mapping are not added in this change.
- **mremap is not intercepted.** If used on a tracked range, resizing or moving it can make the remaining report incorrect. A zero dropped-event counter cannot detect this unobserved operation.
- `madvise` does not close a virtual mapping and is not treated as munmap.
- Only operations observed after activation are attributed. Existing mappings, operations in active old frames during retransformation and mutations through unobserved callers cannot be reconstructed. Start/stop is not an atomic snapshot of all process mappings.
- Standard-page mappings, partial unmapping, successful replacement at an existing address and address reuse are supported. Explicit `MAP_HUGETLB` mappings are currently marked unsupported; they invalidate a complete mapping report. Transparent huge pages retain ordinary mmap range semantics.
- Unsupported bytecode and recording drops are counted in the JFR `mmapDroppedEvents` setting. The converter refuses mmap output if the final counter is nonzero. A zero counter does **not** establish coverage of unobserved APIs.
- Supported converter outputs: HTML and collapsed. Time/latency/tag/state filters, diff and combining several profile selections in one conversion are rejected. Stack include/exclude filtering remains available after lifecycle matching.
- JFR synchronization with a JDK recording and simultaneous method tracing are not supported in this mode. Sampling modes and nativemem may be recorded alongside it.
- Nested callbacks/signals and custom symbol providers/interposers require further validation. This is a feature-branch experiment, not a general OS mapping monitor.

## Tests

No network is used by these commands. With Python 3 and the build prerequisites installed, run these tests without JNA:

```sh
python3 test/mmap/check-libc.py
python3 test/mmap/check-libc.py native
python3 test/mmap/check-libc.py native combined restart
```

The FileChannel test checks an unaligned file offset, unmapping on another Java thread, repeated allocation/free and exact remaining bytes before and after cleanup. The native test loads a JNI library after attach, exercises unattached pthreads, partial unmaps, MAP_FIXED, errno on failed operations, and mmap64 on Linux. Both generate JFR and HTML under `build/test/mmap/`. The FileChannel test uses Unsafe.invokeCleaner only for deterministic test cleanup; the agent does not require application changes.

The existing JNA tests need a JNA 5.8.0 JAR (the one already transferred for the prototype is suitable):

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
