#!/usr/bin/env python3
"""Encrypt a program for the stick with the unit's key: the boot menu lists it under its file name and runs it.

    tools/encrypt-app.py --key KEYFILE PROGRAM [--out FILE]      # default: PROGRAM.enc
    BOOTMENU_AES_KEY='...' tools/encrypt-app.py PROGRAM

The cipher is the one of autoexec.bin (tools/autoexec_cipher.py): the program, zero-padded to a multiple of 512
bytes, AES-256-CBC per 512-byte sector. There is no header; the menu recognises the file by its first sector
decrypting to an ELF header, so the file may be given any name (a trailing .enc, .bin, .elf or .app is left out of
the name the menu shows). The key is the unit's own (the first line of its key file) and is not part of this
repository: --key FILE, $BOOTMENU_AES_KEY or $BOOTMENU_AES_KEY_FILE, as for tools/build-image.py.
"""
import argparse
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
try:
    import autoexec_cipher as cipher
except ImportError:
    sys.exit("needs the 'cryptography' package: python3 -m pip install cryptography")


def read_key(arg):
    if arg:
        return cipher.key_from_bytes(Path(arg).read_bytes())
    if os.environ.get("BOOTMENU_AES_KEY"):
        return cipher.key_from_bytes(os.environ["BOOTMENU_AES_KEY"].encode())
    if os.environ.get("BOOTMENU_AES_KEY_FILE"):
        return cipher.key_from_bytes(Path(os.environ["BOOTMENU_AES_KEY_FILE"]).read_bytes())
    sys.exit("no device key: pass --key FILE or set BOOTMENU_AES_KEY (or BOOTMENU_AES_KEY_FILE)")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    ap.add_argument("program")
    ap.add_argument("--key", help="the unit's key file (else $BOOTMENU_AES_KEY / $BOOTMENU_AES_KEY_FILE)")
    ap.add_argument("--out", help="output file (default: PROGRAM.enc)")
    a = ap.parse_args()
    key = read_key(a.key)
    plain = Path(a.program).read_bytes()
    if plain[:4] != b"\x7fELF":
        sys.exit(f"{a.program}: not an ELF program")
    out = Path(a.out or a.program + ".enc")
    data = cipher.encrypt(plain, key)
    if cipher.decrypt(data, key)[:len(plain)] != plain:
        sys.exit("VERIFY FAILED")
    out.write_bytes(data)
    print(f"wrote {out}: {len(data)} bytes (program {len(plain)} bytes)")


if __name__ == "__main__":
    main()
