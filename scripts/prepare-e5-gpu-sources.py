#!/usr/bin/env python3
"""Prepare pinned E5 GPU and ArmCpu sources without touching existing checkouts."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--reference', action='append', default=[], metavar='NAME=PATH',
                        help='Optional local Git object source for an exact pinned revision')
    args = parser.parse_args()
    device = Path(__file__).resolve().parent.parent
    manifest = json.loads((device / 'scripts/gpu/user-stack-sources.json').read_text())
    references = {}
    for value in args.reference:
        name, separator, path = value.partition('=')
        if not separator or name not in manifest['repositories']:
            parser.error('reference must be NAME=PATH for a manifest repository')
        references[name] = str(Path(path).resolve())
    output = args.output.resolve()
    if output.exists():
        parser.error('output must be new; partial or existing evidence is preserved')
    # Verify all patch inputs before cloning or modifying any target.
    for repository in manifest['repositories'].values():
        for patch in repository['patches']:
            actual = hashlib.sha256((device / patch['path']).read_bytes()).hexdigest()
            if actual != patch['sha256']:
                raise ValueError('patch hash mismatch: ' + patch['path'])
    output.mkdir(parents=True)
    prepared = {}
    for name, repository in manifest['repositories'].items():
        target = output / name
        command = ['git', 'clone', '--no-checkout', '--filter=blob:none']
        if name in references:
            command += ['--reference-if-able', references[name], '--dissociate']
        subprocess.run(command + [repository['url'], str(target)], check=True)
        subprocess.run(['git', '-C', str(target), 'checkout', '--detach', repository['revision']], check=True)
        for patch in repository['patches']:
            subprocess.run(['git', '-C', str(target), 'apply', '--index',
                            str(device / patch['path'])], check=True)
        tree = subprocess.check_output(['git', '-C', str(target), 'write-tree'], text=True).strip()
        prepared[name] = dict(repository, path=str(target), patched_tree=tree)
    result = {'repositories': prepared, 'device': str(device),
              'kernel_local_module': 'driver:ArmCpu=' + str(output / 'armcpu')}
    (output / 'prepared.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
