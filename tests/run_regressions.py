#!/usr/bin/env python3
"""Run the actual host input/time code with native HID/JNI test adapters.

No Switch SDK or proprietary game data is needed. Code is extracted verbatim
from main.c into temporary includes, so tests cannot silently exercise a copy
of an outdated implementation. A missing extraction boundary fails the run.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default=os.environ.get('CC', 'cc'),
                        help='native C compiler executable (default: CC or cc)')
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    source = (root / 'source/main.c').read_text(encoding='utf-8')
    input_start = source.index('static int64_t event_time_ms(void)')
    input_end = source.index('\ntypedef struct {\n  GLuint program;', input_start)
    time_start = source.index('typedef void (*time_service_notify_fn)')
    time_end = source.index('\ntypedef void *(*splash_game_get_fn)', time_start)
    with tempfile.TemporaryDirectory(prefix='jetpack-regressions-') as temp:
        directory = Path(temp)
        (directory / 'input_under_test.inc').write_text(
            source[input_start:input_end], encoding='utf-8')
        (directory / 'time_sync_under_test.inc').write_text(
            source[time_start:time_end], encoding='utf-8')
        for name in ['input_regression', 'time_sync_regression']:
            exe = directory / (name + ('.exe' if os.name == 'nt' else ''))
            subprocess.run([args.cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-O2', '-I', str(directory),
                            str(root / 'tests' / (name + '.c')), '-o', str(exe)],
                           check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
