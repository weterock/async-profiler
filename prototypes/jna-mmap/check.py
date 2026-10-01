#!/usr/bin/env python3
"""End-to-end check; launches only our disposable Demo JVM and external Attach JVM."""
import os, pathlib, subprocess, sys, threading, queue
root = pathlib.Path(__file__).resolve().parent
java = str(pathlib.Path(os.environ['JAVA_HOME']) / 'bin/java')
build = root / 'build'
cold = 'cold' in sys.argv[1:]
restart = 'restart' in sys.argv[1:]
log = build / 'events.tsv'
if log.exists():
    raise SystemExit('events.tsv exists. Archive it before this test; never truncate a live recording.')
p = subprocess.Popen([java, '-cp', str(build/'app-classes') + os.pathsep + str(root/'deps/jna-5.8.0.jar'),
                      'probe.Demo'] + (['cold'] if cold else []),
                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
lines = queue.Queue()
def pump():
    for line in p.stdout:
        print(line, end='', flush=True)
        lines.put(line)
    lines.put('EOF')
threading.Thread(target=pump, daemon=True).start()
def until(prefix):
    while True:
        line = lines.get(timeout=30)
        if line.startswith(prefix): return line
        if line == 'EOF': raise AssertionError('Demo exited early')
def attach(stop=False):
    subprocess.run([java, '--add-modules', 'jdk.attach', '-cp', str(build/'app-classes'),
                    'probe.Attach', str(p.pid), str(build)] + (['stop'] if stop else []), check=True, timeout=30)
try:
    until('READY')
    attach()
    if restart:
        attach(True)
        attach()
    p.stdin.write('go\n'); p.stdin.flush()
    done = until('DONE')
    rows = [x.split('\t') for x in log.read_text().splitlines() if not x.startswith('#')]
    maps = [r for r in rows if r[3] == 'mmap']
    unmaps = [r for r in rows if r[3] == 'munmap']
    assert len(maps) == (5 if cold else 4), maps
    assert len(unmaps) == (6 if cold else 5), unmaps
    assert sum(int(r[7]) == -1 for r in maps) == 1
    assert sum(int(r[7]) == -1 for r in unmaps) == 1
    assert all(int(r[8]) == 22 for r in rows if int(r[7]) == -1)
    intervals = []
    for r in rows:
        if int(r[7]) == -1: continue
        a, size = int(r[4], 16), int(r[5]); b = a + size
        if r[3] == 'mmap': intervals.append((a, b))
        else:
            out = []
            for lo, hi in intervals:
                if hi <= a or lo >= b: out.append((lo, hi))
                else:
                    if lo < a: out.append((lo, a))
                    if hi > b: out.append((b, hi))
            intervals = out
    fields = dict(x.split('=', 1) for x in done.split()[1:])
    expected = int(fields['retained'], 16)
    assert intervals == [(expected, expected + int(fields['bytes']))], intervals
    attach(True)
    before = log.read_text()
    p.stdin.write('cleanup\n'); p.stdin.flush()
    until('CLEANED')
    assert p.wait(timeout=15) == 0
    assert log.read_text() == before, 'Events after stop'
    print('PASS: ' + ('restart, ' if restart else '') + ('cold-load' if cold else 'prewarmed attach') + ', exact addresses/lengths, partial unmap, errors/errno, stop')
finally:
    if p.poll() is None:
        p.kill(); p.wait()
