#!/usr/bin/env python3
"""Exercise the driver's actual imported-page policy with host memory/cache mocks."""
from pathlib import Path
import subprocess
import tempfile

tests = Path(__file__).resolve().parent
gpu = tests.parent
source = (gpu / 'E5MaliG1.cpp').read_text()
start = source.index('namespace E5UserCache {')
end = source.index('// ===========================================================================\n// Lifecycle', start)
body = source[start:end]
for instruction, operation in [('cvac', 1), ('ivac', 2), ('civac', 3)]:
    old = '__asm__ volatile("dc ' + instruction + ', %0" :: "r"(at) : "memory")'
    assert body.count(old) == 1
    body = body.replace(old, 'test_cache_op(at,' + str(operation) + ')')
body = body.replace('__asm__ volatile("dsb sy" ::: "memory")', '(void)0')
with tempfile.TemporaryDirectory(prefix='e5-cache-test-') as temporary:
    folder = Path(temporary)
    source = folder / 'policy.cpp'
    source.write_text((tests / 'user-cache-mock.h').read_text() + body +
                      (tests / 'user-cache-cases.inc').read_text())
    executable = folder / 'policy'
    subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(source), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
