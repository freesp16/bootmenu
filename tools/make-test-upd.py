#!/usr/bin/env python3
"""Development only: a stand-in SP-16 firmware update (.UPD) for the tests, encrypted with a throwaway key.

    tools/make-test-upd.py --key KEYFILE --version 9.99 --exe PROGRAM --out TEST.UPD [--keep DIR]

Laid out like an official update, with nothing of one in it: an iso9660 image (Rock Ridge names) holding
images/pdj.tar.gz (pdj/EP085_Main_R = PROGRAM, pdj/app.rev) and images/rootfs2.tar.gz (./root/gui/Resource/version.txt),
encrypted with tools/autoexec_cipher.py, then the 20-byte trailer: "LIVESAMPLER", the version, the CRC32 of the
ciphertext. --keep also writes the cleartext pdj.tar.gz there, to compare with what the menu decrypts.
"""
import argparse
import io
import shutil
import subprocess
import sys
import tarfile
import tempfile
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import autoexec_cipher as cipher  # noqa: E402


def add(tar, name, data, mode):
    info = tarfile.TarInfo(name)
    info.size, info.mode, info.mtime = len(data), mode, 1569400000
    tar.addfile(info, io.BytesIO(data))


def targz(entries):
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w:gz") as tar:
        for name, data, mode in entries:
            add(tar, name, data, mode)
    return buf.getvalue()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--key", required=True)
    ap.add_argument("--version", default="9.99")
    ap.add_argument("--exe", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--keep")
    a = ap.parse_args()
    if len(a.version.encode()) > 4:
        sys.exit("the version is at most 4 bytes (x.yz)")
    key = cipher.key_from_bytes(Path(a.key).read_bytes())
    xorriso = shutil.which("xorriso") or sys.exit("xorriso not found (apt install xorriso)")
    rev = f"Rev.{a.version.replace('.', '')}9\n".encode()
    pdj = targz([("pdj/EP085_Main_R", Path(a.exe).read_bytes(), 0o755), ("pdj/app.rev", rev, 0o644)])
    rootfs = targz([("./root/gui/Resource/version.txt", f"update {a.version}\n".encode(), 0o644)])
    with tempfile.TemporaryDirectory() as d:
        stage = Path(d) / "stage"
        (stage / "images").mkdir(parents=True)
        (stage / "images" / "pdj.tar.gz").write_bytes(pdj)
        (stage / "images" / "rootfs2.tar.gz").write_bytes(rootfs)
        (stage / "images" / "app.rev").write_bytes(rev)
        (stage / "update").write_bytes(b"not the updater\n")
        iso = Path(d) / "upd.iso"
        subprocess.run([xorriso, "-as", "mkisofs", "-r", "-V", "UsbAuto", "-o", str(iso), str(stage)],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        body = cipher.encrypt(iso.read_bytes(), key)
    trailer = b"LIVESAMPLER" + a.version.encode().ljust(5, b"\0") + zlib.crc32(body).to_bytes(4, "little")
    Path(a.out).write_bytes(body + trailer)
    if a.keep:
        Path(a.keep).mkdir(parents=True, exist_ok=True)
        (Path(a.keep) / "pdj.tar.gz").write_bytes(pdj)
    print(f"wrote {a.out}: {len(body) + len(trailer)} bytes, version {a.version}")


if __name__ == "__main__":
    main()
