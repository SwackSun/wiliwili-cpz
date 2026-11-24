#!/usr/bin/env python3
"""Stage the AArch64 dependency closure, including libraries loaded by SDL and Mesa."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

media, prefix, base, source_root = map(Path, sys.argv[1:])
target = media / 'target'
destination = prefix / 'lib/wiliwili'
destination.mkdir(parents=True, exist_ok=True)
readelf = media / 'host/bin/aarch64-buildroot-linux-gnu-readelf'
strip = media / 'host/bin/aarch64-buildroot-linux-gnu-strip'
patchelf = base / 'host/bin/patchelf'
system = {'ld-linux-aarch64.so.1', 'libc.so.6', 'libpthread.so.0', 'libdl.so.2',
          'libm.so.6', 'librt.so.1', 'libresolv.so.2', 'libgcc_s.so.1', 'libstdc++.so.6'}
needed = re.compile(r'\(NEEDED\).*?\[([^\]]+)\]')
runpath = re.compile(r'\((RPATH|RUNPATH)\).*?\[([^\]]+)\]')
rodata = re.compile(r'^\s*\[\s*\d+\]\s+\.rodata\s+PROGBITS\s+[\da-f]+\s+([\da-f]+)\s+([\da-f]+)', re.M)
build_roots = sorted({os.fsencode(source_root.resolve().as_posix()),
                      os.fsencode(base.resolve().as_posix())}, key=len, reverse=True)


def strip_for_delivery(elf):
    subprocess.run([strip, '--strip-unneeded', elf], check=True)
    dynamic = subprocess.check_output([readelf, '-d', elf], text=True)
    for tag, value in runpath.findall(dynamic):
        sibling = os.path.relpath(destination, elf.parent)
        portable = '$ORIGIN' if sibling == '.' else '$ORIGIN/' + sibling
        entries = [portable if any(os.fsencode(entry).startswith(root + b'/') for root in build_roots)
                   else entry for entry in value.split(':')]
        if entries != value.split(':'):
            tool = patchelf if patchelf.is_file() else shutil.which('patchelf')
            if not tool:
                raise SystemExit('Install patchelf or use a completed czlite SDK.')
            options = ['--force-rpath'] if tag == 'RPATH' else []
            subprocess.run([tool, *options, '--set-rpath', ':'.join(dict.fromkeys(entries)), elf], check=True)

    # FFmpeg and mpv record configure arguments and source names in read-only
    # strings. Keep their storage length and all executable offsets intact.
    data = elf.read_bytes()
    if not any(root in data for root in build_roots):
        return
    sections = subprocess.check_output([readelf, '-WS', elf], text=True)
    if match := rodata.search(sections):
        offset, size = map(lambda value: int(value, 16), match.groups())
        strings = data[offset:offset + size]
        for root in build_roots:
            public = b'/usr/src/cpz'
            if len(public) > len(root):
                public = b'/src'[:len(root)]
            strings = strings.replace(root, public.ljust(len(root), b'/'))
        data = data[:offset] + strings + data[offset + size:]
    if any(root in data for root in build_roots):
        raise SystemExit(f'Build path outside supported metadata sections in {elf.name}')
    elf.write_bytes(data)


pending = [prefix / 'bin/wiliwili']
staged = set()
strip_for_delivery(pending[0])


def find(root, name):
    for directory in ('usr/lib', 'lib'):
        candidate = root / directory / name
        if candidate.is_file():
            return candidate
    raise SystemExit(f'Missing target library {name} under {root}')


def stage(source, folder):
    name = source.name
    if (folder, name) in staged:
        return
    staged.add((folder, name))
    folder.mkdir(parents=True, exist_ok=True)
    real = source.resolve(strict=True)
    if not real.is_relative_to(target.resolve()):
        raise SystemExit(f'Library outside media target: {source}')
    dest = folder / real.name
    if (folder, real.name) not in copied:
        shutil.copy2(real, dest)
        strip_for_delivery(dest)
        copied.add((folder, real.name))
        pending.append(dest)
    if name != real.name:
        alias = folder / name
        if not alias.is_symlink() or os.readlink(alias) != real.name:
            alias.unlink(missing_ok=True)
            alias.symlink_to(real.name)


copied = set()
for name in ('libEGL.so.1', 'libGLESv2.so.2'):
    stage(find(target, name), destination)
for driver in (target / 'usr/lib/dri').glob('*_dri.so'):
    stage(driver, destination / 'dri')
for module in (target / 'usr/lib/gbm').glob('*.so'):
    stage(module, destination / 'gbm')
while pending:
    elf = pending.pop()
    dynamic = subprocess.check_output([readelf, '-d', elf], text=True)
    for soname in needed.findall(dynamic):
        if soname in system:
            find(base / 'target', soname)
        else:
            stage(find(target, soname), destination)
