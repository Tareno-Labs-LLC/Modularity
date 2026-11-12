"""Check exported script dependencies, page alignment, and bundle placeholders."""
import argparse
import io
from pathlib import Path
import struct
import subprocess
import tempfile
import zipfile


def script_soname(path):
    raw = path.removesuffix('.so').encode('utf-8')
    value = 1469598103934665603
    for byte in raw:
        value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
    name = ''.join(chr(c) if 48 <= c <= 57 or 65 <= c <= 90 or 97 <= c <= 122 else '_' for c in raw)[:80]
    return f'libmodu_{name}_{value & 0xffffffff:08x}.so'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('apk', type=Path)
    parser.add_argument('--readelf', required=True, type=Path)
    parser.add_argument('--asset', action='append', default=[], help='Required bundle path; repeat for script-loaded assets')
    args = parser.parse_args()
    with zipfile.ZipFile(args.apk) as apk, tempfile.TemporaryDirectory() as scratch:
        bundle = io.BytesIO(apk.read('assets/content.modbundle'))
        assert bundle.read(11) == b'MODRUNTIME1', 'Expected Android runtime bundle v1'
        version, count = struct.unpack('<II', bundle.read(8))
        assert version == 1
        entries = {}
        for _ in range(count):
            length, size = struct.unpack('<IQ', bundle.read(12))
            path = bundle.read(length).decode('utf-8')
            entries[path] = size
            bundle.seek(size, 1)
        for asset in args.asset:
            assert entries.get(asset, 0) > 0, f'Missing script-loaded asset: {asset}'

        placeholders = [p for p, size in entries.items() if p.endswith('.so') and size == 0]
        assert placeholders, 'No compiled script placeholders in bundle'
        abis = {p.split('/')[1] for p in apk.namelist() if p.startswith('lib/') and p.endswith('/libModularityPlayer.so')}
        assert abis, 'No Android player library'
        for abi in sorted(abis):
            prefix = f'lib/{abi}/'
            assert prefix + 'libc++_shared.so' in apk.namelist(), 'Missing shared C++ runtime'
            libraries = [p for p in apk.namelist() if p.startswith(prefix + 'libmodu_') and p.endswith('.so')]
            assert len(libraries) == len(placeholders), 'Script library/placeholder count mismatch'
            for placeholder in placeholders:
                # The output directory is project-configurable; Runtime/... below it
                # must map to one packaged soname without a .staging component.
                parts = placeholder.split('/')
                matches = [prefix + script_soname('/'.join(parts[i:])) for i in range(len(parts))]
                assert any(p in libraries for p in matches), f'No library for {placeholder}'
            for library in libraries:
                local = Path(scratch) / Path(library).name
                local.write_bytes(apk.read(library))
                dynamic = subprocess.check_output([str(args.readelf), '-d', str(local)], text=True)
                assert '[libModularityPlayer.so]' in dynamic, f'{library} has no engine dependency'
                assert '[libc++_shared.so]' in dynamic, f'{library} has no shared C++ runtime dependency'
                headers = subprocess.check_output([str(args.readelf), '-lW', str(local)], text=True)
                loads = [line.split() for line in headers.splitlines() if line.strip().startswith('LOAD ')]
                assert loads and all(int(line[-1], 16) >= 16384 for line in loads), f'{library} lacks 16 KB alignment'
            print(f'PASS: {abi}: {len(libraries)} script libraries, engine dependencies, placeholders, 16 KB segments')
        print(f'PASS: {len(args.asset)} required script-loaded assets present')


if __name__ == '__main__':
    main()
