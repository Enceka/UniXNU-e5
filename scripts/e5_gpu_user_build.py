#!/usr/bin/env python3
"""Build the signed E5 userspace GPU probe and its kernel-carried image."""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys


def build(tree, out):
    tree, out = Path(tree), Path(out)
    out.mkdir(parents=True, exist_ok=True)
    source = tree / 'scripts/gpu/user/E5GpuUserProbe.c'
    sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
    clang = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--find', 'clang'], text=True).strip()
    program = out / 'e5gpu-probe'
    env = dict(os.environ)
    for name in ('SDKROOT', 'CPATH', 'CCC_OVERRIDE_OPTIONS'):
        env.pop(name, None)
    command = [clang, '-target', 'arm64-apple-ios17.0', '-isysroot', sdk, '-O2',
               '-Wall', '-Wextra', '-Werror', '-Wno-deprecated-declarations',
               str(source), '-framework', 'IOKit', '-o', str(program)]
    subprocess.run(command, env=env, check=True)
    subprocess.run(['codesign', '-f', '-s', '-', str(program)], check=True)
    signature = subprocess.run(['codesign', '-dvvv', str(program)], capture_output=True, text=True, check=True)
    cdhash = re.search(r'CDHash=([0-9a-f]{40})', signature.stderr).group(1)
    data = program.read_bytes()
    rows = [', '.join(f'0x{x:02x}' for x in data[i:i + 16]) for i in range(0, len(data), 16)]
    (out / 'E5GpuUserProbeImage.inc').write_text(
        '// Generated from the signed, normal-LC_MAIN E5GpuUserProbe.c.\n'
        'static const unsigned char e5GpuUserProbeImage[] = {\n' +
        ',\n'.join(rows) + '\n};\n')
    report = {'command': command, 'sdk': sdk, 'entry': 'normal LC_MAIN/main',
              'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
              'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest(), 'cdhash': cdhash}
    (out / 'user-probe-build.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


if __name__ == '__main__':
    print(json.dumps(build(sys.argv[1], sys.argv[2]), indent=2))
