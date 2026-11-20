#!/usr/bin/env python3
"""Check the C text loader without an FPGA device."""
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]


def encode(line):
    cells = [0 if c == "." else int(c) for c in line]
    record = bytearray(64)
    for i, value in enumerate(cells):
        record[9 + i // 2] |= value << (4 * (i % 2))
    return record


def main():
    with tempfile.TemporaryDirectory(prefix="acceldoku-text-") as tmp:
        tmp = pathlib.Path(tmp)
        harness = tmp / "loader.c"
        harness.write_text('''
#define main acceldoku_run_main
#include "host/tools/acceldoku_run.c"
#undef main
int main(int argc, char **argv) {
    (void)argc;
    size_t count;
    uint8_t *records = load_text(argv[1], &count);
    int failed = fwrite(records, 64, count, stdout) != count;
    free(records);
    return failed;
}
''')
        exe = tmp / "loader"
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-I", str(ROOT), "-I", str(ROOT / "host/driver/include"),
                        str(harness), "-o", str(exe)], check=True)
        for name in ("17puz49158.txt", "HardestDatabase110626.txt"):
            path = ROOT / "sim/data" / name
            expected = b"".join(encode(line.strip()) for line in path.read_text().splitlines())
            assert subprocess.check_output([exe, path]) == expected

        fixture = tmp / "input.txt"
        puzzle = "1234567890." * 7 + "1234"
        fixture.write_bytes(("  # comment\r\n\r\n\t" + puzzle + " \r\n" + puzzle).encode())
        assert subprocess.check_output([exe, fixture]) == encode(puzzle) * 2
        for content, message in ((b"# empty\n", b"Empty puzzle file"),
                                 (b".\n", b"line 1"),
                                 (b"\n" + b"." * 80 + b"x\n", b"line 2")):
            fixture.write_bytes(content)
            result = subprocess.run([exe, fixture], capture_output=True)
            assert result.returncode != 0 and message in result.stderr
    print("PASS: text input conversion and validation")


if __name__ == "__main__":
    main()
