#!/usr/bin/env python3
"""Smoke-check the streaming tool with generated input or a puzzle corpus."""
import argparse
import pathlib
import subprocess
import tempfile

from acceldoku_device import RECORDS
from check_stream import DIGITS


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("executable")
    p.add_argument("--corpus", type=pathlib.Path)
    p.add_argument("--seconds", default="3")
    p.add_argument("--repeat", type=int, default=1)
    args = p.parse_args()
    exe = str(pathlib.Path(args.executable).resolve())
    with tempfile.TemporaryDirectory(prefix="acceldoku-example-") as tmp:
        source = pathlib.Path(tmp) / "input.txt"
        n = RECORDS * 3 + 19
        source.write_text((DIGITS + "\n") * n)
        for _ in range(args.repeat):
            for tx, rx in ((8, 8), (8, 64)):
                subprocess.run([exe, str(source), str(tx), str(rx), args.seconds], check=True)
                if args.corpus:
                    subprocess.run([exe, str(args.corpus), str(tx), str(rx), args.seconds], check=True)
        for data in (b"", b"." * 80, b"." * 80 + b"x"):
            source.write_bytes(data)
            assert subprocess.run([exe, str(source), "1", "1", args.seconds]).returncode != 0
        print("PASS: streaming tool completed; bad inputs rejected")


if __name__ == "__main__":
    main()
