"""The SP-16 USB autoexec image cipher (also used by the official .UPD payloads).

An autoexec.bin is an iso9660 image encrypted with AES-256-CBC, each 512-byte sector on its own with
IV = little-endian 32-bit sector number + 12 zero bytes. The key is the first line of the device key file (up to
LF/NUL), at most 31 bytes, zero-padded to 32. The unit's own hook decrypts it (loop device), mounts it and runs
./autoexec.sh as root. There is no signature or checksum trailer.

The key is not part of this repository: pass it to tools/build-image.py.
"""
import struct

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

SECTOR = 512
ISO_PRIMARY_MARKER = b"\x01CD001\x01"  # at byte 32768 of an iso9660 image


def key_from_bytes(raw):
    line = raw.split(b"\n", 1)[0].split(b"\0", 1)[0].rstrip(b"\r")
    if not line:
        raise ValueError("the key is empty")
    return line[:31].ljust(32, b"\0")


def _xcrypt(data, key, decrypt):
    out = bytearray(len(data))
    for off in range(0, len(data), SECTOR):
        iv = struct.pack("<I", (off // SECTOR) & 0xFFFFFFFF) + bytes(12)
        c = Cipher(algorithms.AES(key), modes.CBC(iv))
        worker = c.decryptor() if decrypt else c.encryptor()
        out[off:off + SECTOR] = worker.update(data[off:off + SECTOR]) + worker.finalize()
    return bytes(out)


def encrypt(plain, key):
    if len(plain) % SECTOR:
        plain += b"\0" * (SECTOR - len(plain) % SECTOR)
    return _xcrypt(plain, key, decrypt=False)


def decrypt(cipher, key):
    return _xcrypt(cipher, key, decrypt=True)
