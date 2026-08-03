#!/usr/bin/env python3
"""Encoder differential: our x86-64 encoder vs GNU 'as', byte for byte.

docs/backend.md, validation #3. The encoder is the one layer where a mistake produces
working-but-wrong code — an instruction that assembles, links, runs, and computes
something other than what it says. Byte comparison against an independent assembler is
what catches that, and it gets more valuable with every instruction added, which is why
this is automated rather than a hand-checked table.

How it works: 'build/x86_encoder_test --dump' prints one line per pinned form,

    <AT&T source>\t<hex bytes>

Each source line is handed to 'as', and the resulting object's .text is compared
against the encoder's bytes. A difference is a failure UNLESS it is one of the
deliberate divergences below.

DELIBERATE DIVERGENCES. This encoder always uses disp32 and imm32 where 'as' picks the
shortest form (disp8, imm8, the 0x83 sign-extended-immediate ALU opcodes). That is a
size choice, not a semantic one: one uniform form keeps both the emitters and their
tests simple, and the stage-6 allocator will not care.

Rather than guess at which byte differences are "the size choice" — a classifier that
is itself easy to get wrong, and that a genuinely bad encoding could hide behind — the
comparison is SEMANTIC: when the bytes differ, both sequences are disassembled by
objdump and their instruction text compared. Two encodings of the same instruction
disassemble identically no matter how long they are, while a wrong opcode, register or
displacement changes the text. objdump is thus used as an independent decoder, which is
a stronger check than byte equality alone and needs no whitelist.

Skipped when 'as' or 'objdump' is unavailable (a Mirage checkout does not require
binutils), and it says so rather than silently passing.
"""

import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
ENCODER_TEST = REPO_ROOT / "build" / "x86_encoder_test"

failures = 0
longer = 0


def assemble(source: str) -> bytes | None:
    """The .text bytes 'as' produces for one instruction, or None if it rejects it."""
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / "in.s"
        obj = Path(tmp) / "out.o"
        src.write_text(f".text\n{source}\n")
        done = subprocess.run(["as", "--64", str(src), "-o", str(obj)],
                              capture_output=True, text=True)
        if done.returncode != 0:
            return None
        dumped = subprocess.run(["objdump", "-d", "-j", ".text", str(obj)],
                                capture_output=True, text=True)
        if dumped.returncode != 0:
            return None
        out = bytearray()
        for line in dumped.stdout.splitlines():
            # objdump body lines look like '   0:\t48 89 e5             \tmov ...'
            parts = line.split("\t")
            if len(parts) < 2 or not parts[0].strip().rstrip(":").strip():
                continue
            try:
                int(parts[0].strip().rstrip(":"), 16)
            except ValueError:
                continue
            for token in parts[1].split():
                if len(token) == 2:
                    out.append(int(token, 16))
        return bytes(out)


def disassemble(code: bytes) -> str | None:
    """The instruction text objdump decodes 'code' to, normalized.

    Raw-binary mode, so the bytes are decoded exactly as the CPU would see them — no
    assembler in the loop, which is what makes this an independent check of what our
    encoder actually produced.
    """
    with tempfile.TemporaryDirectory() as tmp:
        blob = Path(tmp) / "code.bin"
        blob.write_bytes(code)
        done = subprocess.run(
            ["objdump", "-D", "-b", "binary", "-m", "i386:x86-64", "-M", "att", str(blob)],
            capture_output=True, text=True)
        if done.returncode != 0:
            return None
        text = []
        for line in done.stdout.splitlines():
            parts = line.split("\t")
            if len(parts) < 3:
                continue
            try:
                int(parts[0].strip().rstrip(":"), 16)
            except ValueError:
                continue
            # Drop objdump's trailing '# comment' annotations (RIP targets etc).
            text.append(parts[2].split("#")[0].strip().replace(" ", ""))
        return " | ".join(text) if text else None


def main() -> int:
    global failures, longer

    if not ENCODER_TEST.exists():
        print(f"FAIL: {ENCODER_TEST} not found; run 'just build' first")
        return 1
    if not shutil.which("as") or not shutil.which("objdump"):
        print("skip: 'as'/'objdump' not available — install binutils to run the "
              "encoder differential")
        return 0

    dumped = subprocess.run([str(ENCODER_TEST), "--dump"], capture_output=True, text=True)
    if dumped.returncode != 0 or not dumped.stdout.strip():
        print("FAIL: x86_encoder_test --dump produced nothing")
        return 1

    checked = 0
    for line in dumped.stdout.splitlines():
        if "\t" not in line:
            continue
        source, hex_bytes = line.split("\t", 1)
        ours = bytes(int(b, 16) for b in hex_bytes.split())
        theirs = assemble(source)
        if theirs is None:
            failures += 1
            print(f"FAIL: 'as' rejected our own reference source: {source!r}")
            continue
        checked += 1
        if ours == theirs:
            print(f"ok: {source}")
            continue
        ours_text = disassemble(ours)
        theirs_text = disassemble(theirs)
        if ours_text is not None and ours_text == theirs_text:
            longer += 1
            print(f"ok: {source}  [same instruction, longer uniform form: "
                  f"{ours.hex(' ')} vs {theirs.hex(' ')}]")
        else:
            failures += 1
            print(f"FAIL: {source}\n  ours:  {ours.hex(' ')}  -> {ours_text}"
                  f"\n  as:    {theirs.hex(' ')}  -> {theirs_text}")

    print(f"\n{checked} instruction(s) compared against 'as'; {longer} encode the same "
          f"instruction in this encoder's longer uniform form")
    if failures:
        print(f"{failures} failure(s)")
        return 1
    print("encoder differential passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
