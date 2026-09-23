#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Report imported symbols of prebuilt blobs that no library in their load
group defines.

The build's prebuilt ELF check resolves imports against library stubs, which
omit libc's LIBC_PRIVATE exports (the ARM __aeabi_* helpers); a blob with
those imports must skip the symbol check. This script checks the same blobs
against the libraries the build installed, following DT_NEEDED
transitively, the way the bionic linker searches a load group. Extra
libraries added by TARGET_LD_SHIM_LIBS are given after '='.

usage: $PYTHON check-blob-symbols.py OUT BLOB[=shim1.so,shim2.so] ...
exit status 1 when any import stays unresolved.
"""
import glob
import os
import subprocess
import sys


def readelf(args, path):
    return subprocess.run(['readelf'] + args + [path], capture_output=True,
                          text=True, check=True).stdout


def needed(path):
    return [line.split('[')[1].split(']')[0]
            for line in readelf(['-Wd'], path).splitlines()
            if '(NEEDED)' in line]


def symbols(path):
    """Return (defined, imported) dynamic symbol names; IFUNC rows have extra
    columns, so the section index and name are read from the end."""
    defined, imported = set(), set()
    for line in readelf(['-Ws', '--dyn-syms'], path).splitlines():
        f = line.split()
        if len(f) < 8 or not f[0].endswith(':'):
            continue
        name = f[-1].split('@')[0]
        if f[-2] == 'UND':
            if 'WEAK' not in f and name:
                imported.add(name)
        elif 'GLOBAL' in f or 'WEAK' in f:
            defined.add(name)
    return defined, imported


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    out = sys.argv[1]
    dirs = [out + d for d in (
        '/system/lib', '/system/vendor/lib', '/system/vendor/lib/egl',
        '/system/vendor/lib/hw',
        '/system/apex/com.android.runtime/lib/bionic')]
    dirs += glob.glob(out + '/system/apex/*/lib')

    def find(name):
        for d in dirs:
            if os.path.exists(os.path.join(d, name)):
                return os.path.join(d, name)
        return None

    status = 0
    for arg in sys.argv[2:]:
        blob, _, shims = arg.partition('=')
        queue = needed(blob) + [s for s in shims.split(',') if s]
        seen, missing, have = set(), [], symbols(blob)[0]
        while queue:
            name = queue.pop(0)
            if name in seen:
                continue
            seen.add(name)
            path = find(name)
            if path is None:
                missing.append(name)
                continue
            have |= symbols(path)[0]
            queue += needed(path)
        unresolved = sorted(symbols(blob)[1] - have)
        if missing or unresolved:
            status = 1
            print('%s: missing libraries %s, unresolved %s'
                  % (blob, missing, unresolved))
    sys.exit(status)


if __name__ == '__main__':
    main()
