#!/usr/bin/env python3
"""Take the official application and its screen resources out of an SP-16 update file (.UPD), ready for the stick.

    tools/extract-upd.py --key KEYFILE UPDATE.UPD --name my163 --out STICK_DIR
    tools/extract-upd.py --key KEYFILE UPDATE.UPD --name my163 --out STICK_DIR --base TORAIZSP16_1.60.UPD --encrypt

writes, in STICK_DIR:

    my163           the update's EP085_Main_R (with --encrypt: encrypted with the key, as tools/encrypt-app.py does)
    my163-gui/      the update's screen resources (root/gui: Resource/skin16/...)

Copy both to the stick's root: the boot menu lists my163, runs it as the official application and lays my163-gui/ over
the unit's own resources while it runs. With --base, my163-gui/ holds only the files that differ from that other
update (e.g. the official release the unit runs), which is enough since the menu starts from the unit's resources.

An update is an iso9660 image encrypted like autoexec.bin (tools/autoexec_cipher.py) plus a 20-byte trailer; the
application is in images/pdj.tar.gz and the resources in one of images/rootfs*.tar.gz. Needs xorriso and the
'cryptography' package. The key is the unit's (--key FILE, $BOOTMENU_AES_KEY or $BOOTMENU_AES_KEY_FILE) and is not
part of this repository. Nothing here is written anywhere but STICK_DIR.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
try:
    import autoexec_cipher as cipher
except ImportError:
    sys.exit("needs the 'cryptography' package: python3 -m pip install cryptography")

TRAILER = 20
GUI = "root/gui/"


def read_key(arg):
    if arg:
        return cipher.key_from_bytes(Path(arg).read_bytes())
    if os.environ.get("BOOTMENU_AES_KEY"):
        return cipher.key_from_bytes(os.environ["BOOTMENU_AES_KEY"].encode())
    if os.environ.get("BOOTMENU_AES_KEY_FILE"):
        return cipher.key_from_bytes(Path(os.environ["BOOTMENU_AES_KEY_FILE"]).read_bytes())
    sys.exit("no device key: pass --key FILE or set BOOTMENU_AES_KEY (or BOOTMENU_AES_KEY_FILE)")


def open_update(path, key, work):
    """Decrypts the update into work/ and returns its images/ folder and its version."""
    data = Path(path).read_bytes()
    if len(data) < TRAILER or data[-TRAILER:-TRAILER + 11] != b"LIVESAMPLER":
        sys.exit(f"{path}: not an SP-16 update file (no LIVESAMPLER trailer)")
    version = data[-TRAILER + 11:-TRAILER + 16].split(b"\0")[0].decode(errors="replace")
    body = data[:(len(data) - TRAILER) // 512 * 512]
    plain = cipher.decrypt(body, key)
    if plain[32768:32768 + len(cipher.ISO_PRIMARY_MARKER)] != cipher.ISO_PRIMARY_MARKER:
        sys.exit(f"{path}: does not decrypt to an iso9660 image (another key?)")
    iso = Path(work) / "update.iso"
    iso.write_bytes(plain)
    images = Path(work) / "images"
    xorriso = shutil.which("xorriso") or sys.exit("xorriso not found (apt install xorriso)")
    subprocess.run([xorriso, "-osirrox", "on", "-indev", str(iso), "-extract", "/images", str(images)],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    iso.unlink()
    return images, version


def gui_files(images):
    """{path under root/gui: bytes} from whichever rootfs*.tar.gz holds root/gui."""
    for tgz in sorted(images.glob("rootfs*.tar.gz")):
        files = {}
        with tarfile.open(tgz) as tar:
            for m in tar:
                name = m.name[2:] if m.name.startswith("./") else m.name
                if m.isfile() and name.startswith(GUI):
                    files[name[len(GUI):]] = tar.extractfile(m).read()
        if files:
            return files, tgz.name
    sys.exit("no root/gui in the update's rootfs*.tar.gz")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    ap.add_argument("update")
    ap.add_argument("--key", help="the unit's key file (else $BOOTMENU_AES_KEY / $BOOTMENU_AES_KEY_FILE)")
    ap.add_argument("--name", default="EP085_Main_R", help="file name on the stick (default EP085_Main_R)")
    ap.add_argument("--out", required=True, help="folder to write to (e.g. the stick's root)")
    ap.add_argument("--base", help="another update: write only the screen resources that differ from it")
    ap.add_argument("--encrypt", action="store_true", help="encrypt the application with the key")
    a = ap.parse_args()
    key = read_key(a.key)
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    prog, gui_dir = out / a.name, out / f"{a.name}-gui"
    if gui_dir.exists():
        sys.exit(f"{gui_dir} exists: remove it first")

    with tempfile.TemporaryDirectory() as work:
        images, version = open_update(a.update, key, work)
        with tarfile.open(images / "pdj.tar.gz") as tar:
            exe = tar.extractfile("pdj/EP085_Main_R").read()
        files, where = gui_files(images)
        print(f"update {version}: EP085_Main_R {len(exe)} bytes, {len(files)} screen resource files ({where})")
        if a.base:
            with tempfile.TemporaryDirectory() as work2:
                base_images, base_version = open_update(a.base, key, work2)
                base, _ = gui_files(base_images)
            files = {k: v for k, v in files.items() if base.get(k) != v}
            print(f"base {base_version}: {len(files)} file(s) differ")

    prog.write_bytes(cipher.encrypt(exe, key) if a.encrypt else exe)
    os.chmod(prog, 0o755)
    for rel, data in sorted(files.items()):
        dst = gui_dir / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_bytes(data)
    size = sum(len(v) for v in files.values())
    print(f"wrote {prog}{' (encrypted)' if a.encrypt else ''} and {gui_dir}/ ({len(files)} files, {size // 1024} KB)")


if __name__ == "__main__":
    main()
