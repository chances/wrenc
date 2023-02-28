#!/bin/env python3
# This script wraps the tree-sitter command. Unfortunately it puts it's
# output files into the working directory, extracts a header we already
# have in the runtime library, etc.
#
# This wrapper runs it in a cleaner way, pulling out only the parser C file.

import argparse
import subprocess
import sys
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--ts-path', required=True, help='The path to the tree-sitter executable')
    parser.add_argument('--output', required=True, help='The path to the final output C file')
    parser.add_argument('--tmp-dir', required=True, help='The temporary directory to run tree-sitter in')
    parser.add_argument('--grammar-file', required=True, help='The grammar JavaScript file')

    opt = parser.parse_args()

    ts_bin = Path(opt.ts_path)
    tmp_dir = Path(opt.tmp_dir)
    output_target = Path(opt.output)

    # Meson doesn't create our temporary directory for us
    if not tmp_dir.is_dir():
        tmp_dir.mkdir()

    # The source file needs to be absolute, since we'll pass it to tree-sitter
    # which is running in another directory.
    src = Path(opt.grammar_file).absolute()

    # If the output C file already exists, delete it to make sure that if something
    # goes wrong we find out about it.
    generated_file = tmp_dir / 'src/parser.c'
    generated_file.unlink(True)

    # Note: use ABI=14 to avoid potential breaks when future tree-sitter versions
    # are used on older versions of this project. Is this necessary?
    # This was developed (at least initially) with tree-sitter 0.20.7.
    subprocess.run(
        args=[ts_bin, 'generate', '--no-bindings', '--abi=14', src],
        check=True,
        cwd=tmp_dir,
    )

    # Move the output file to it's specified new location
    output_target.unlink(True)
    generated_file.rename(output_target)


main()
