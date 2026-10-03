#!/usr/bin/env python3
# Copyright 2026 The Flutter Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Build a failing upstream control and the candidate with the same toolchain."""

import argparse
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--emxx', required=True, type=Path)
parser.add_argument('--skia', required=True, type=Path)
parser.add_argument('--output', required=True, type=Path)
args = parser.parse_args()
here = Path(__file__).resolve().parent
args.output.mkdir(parents=True, exist_ok=True)
for name, implementation in [
    ('original', args.skia / 'src/core/SkSemaphore.cpp'),
    ('fixed', here.parent / 'SkSemaphore_wasm_workers.cpp'),
]:
  subprocess.run([
      str(args.emxx.resolve()), '-std=c++20', '-O2', '-DSK_RELEASE',
      '-sWASM_WORKERS=1', '-sASSERTIONS=1', '-sENVIRONMENT=web,worker',
      '-I' + str(args.skia.resolve()), str(here / 'semaphore_test.cpp'),
      str(implementation.resolve()), '-o', str(args.output.resolve() / (name + '.html')),
  ], check=True)
