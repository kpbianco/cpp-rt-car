#!/usr/bin/env python3
"""Build the optional SDK, remove its original install, and exercise public consumers."""
from __future__ import annotations
import argparse
from pathlib import Path
import shutil
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser()
p.add_argument('--work-directory',type=Path,required=True)
p.add_argument('--artifact-output',type=Path,required=True)
a=p.parse_args()
work=a.work_directory.resolve(); work.mkdir(parents=True,exist_ok=False)

def run(*args, cwd=ROOT):
    subprocess.run([str(x) for x in args],cwd=cwd,check=True,timeout=600)

def executable(build: Path,name: str) -> Path:
    suffix='.exe' if sys.platform=='win32' else ''
    direct=build/(name+suffix)
    return direct if direct.is_file() else build/'Release'/(name+suffix)

build=work/'build'
run('cmake','-S',ROOT,'-B',build,'-DCMAKE_BUILD_TYPE=Release','-DENABLE_TESTS=OFF',
    '-DRTFW_BUILD_EXPERIMENTAL=OFF','-DRTFW_BUILD_BENCHMARKS=ON','-DSIM_WERROR=ON')
run('cmake','--build',build,'--config','Release','--parallel','2')
run('cmake','--install',build,'--config','Release','--prefix',work/'stage')
run('cmake','-E','tar','cf',work/'installed.zip','--format=zip','stage',cwd=work)
shutil.rmtree(work/'stage')
relocated=work/'relocated'; relocated.mkdir()
run('cmake','-E','tar','xf',work/'installed.zip',cwd=relocated)
prefix=relocated/'stage'
consumer=work/'consumer'
run('cmake','-S',ROOT/'tests/package_consumer','-B',consumer,'-DCMAKE_BUILD_TYPE=Release',
    f'-DCMAKE_PREFIX_PATH={prefix}','-DRTFW_TEST_BENCHMARK=ON')
run('cmake','--build',consumer,'--config','Release','--parallel','2')
run('ctest','--test-dir',consumer,'-C','Release','--output-on-failure')
# Same optional install, but benchmark not requested: its imported target must be absent.
run('cmake','-S',ROOT/'tests/package_consumer','-B',work/'unrequested',
    '-DCMAKE_BUILD_TYPE=Release',f'-DCMAKE_PREFIX_PATH={prefix}','-DRTFW_TEST_BENCHMARK=OFF')
output=a.artifact_output.resolve()
run(executable(consumer,'rtfw_consumer_benchmark'),output)
run(sys.executable,prefix/'share/rtfw/tools/check_benchmark_artifact.py','--artifact-root',output)
embedded=work/'embedding'
run('cmake','-S',ROOT/'tests/benchmark_fixtures/consumer','-B',embedded,
    '-DCMAKE_BUILD_TYPE=Release',f'-DRTFW_SOURCE_DIR={ROOT}')
run('cmake','--build',embedded,'--config','Release','--parallel','2')
run('ctest','--test-dir',embedded,'-C','Release','--output-on-failure')
print('M23 optional installed/archive-relocated/unrequested/add-subdirectory consumers passed')
