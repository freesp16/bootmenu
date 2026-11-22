#!/usr/bin/env python3
"""Build autoexec.bin: the boot menu and its launcher, encrypted for the unit's USB autoexec hook.

    BOOTMENU_AES_KEY='...' tools/build-image.py                 # -> out/autoexec.bin
    tools/build-image.py --key path/to/key.file
    tools/build-image.py --key KEYFILE --app path/to/app[:ID]   # also embed an app (optional; normally apps are
                                                                # plain files next to autoexec.bin on the stick)

The device key is NOT part of this repository. It comes from, in this order: --key FILE, the environment variable
BOOTMENU_AES_KEY (the key line itself), or BOOTMENU_AES_KEY_FILE (a path).

Image content (iso9660 + Rock Ridge, then tools/autoexec_cipher.py):

    autoexec.sh        image/autoexec.sh, the stub the hook runs (logs, then runs launcher.sh)
    launcher.sh        image/launcher.sh
    bootmenu           out/bootmenu (tools/build.sh)
    apps/<id>/...      each --app / --app-dir (optional)
    dropbearmulti, authorized_keys    only with --ssh-dropbear and --ssh-keys (a development SSH server)
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
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


def elf_arm_soft(path):
    h = Path(path).read_bytes()[:52]
    if len(h) < 52 or h[:4] != b"\x7fELF":
        return "not an ELF file"
    if h[4] != 1 or h[5] != 1 or int.from_bytes(h[18:20], "little") != 40:
        return "not a 32-bit little-endian ARM executable"
    if int.from_bytes(h[36:40], "little") & 0x400:
        return "a hard-float (armhf) build; the unit runs armel"
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    ap.add_argument("--key", help="device key file (else $BOOTMENU_AES_KEY / $BOOTMENU_AES_KEY_FILE)")
    ap.add_argument("--menu", default=str(ROOT / "out" / "bootmenu"), help="the armel menu binary (tools/build.sh)")
    ap.add_argument("--app", action="append", default=[], metavar="PATH[:ID]", help="embed an executable as apps/ID/")
    ap.add_argument("--app-ini", action="append", default=[], metavar="ID=FILE", help="app.ini for an --app")
    ap.add_argument("--app-dir", action="append", default=[], metavar="DIR[:ID]", help="embed a folder app")
    ap.add_argument("--ssh-dropbear", help="development: a Dropbear multi-binary (armel) to embed")
    ap.add_argument("--ssh-keys", help="development: authorized_keys for that SSH server")
    ap.add_argument("--out", default=str(ROOT / "out" / "autoexec.bin"))
    ap.add_argument("--keep-iso", help="also write the cleartext iso here")
    a = ap.parse_args()

    key = read_key(a.key)
    xorriso = shutil.which("xorriso") or sys.exit("xorriso not found (apt install xorriso)")
    menu = Path(a.menu)
    if not menu.is_file():
        sys.exit(f"no menu binary at {menu}: run tools/build.sh first")
    if (why := elf_arm_soft(menu)):
        sys.exit(f"{menu}: {why}")
    if bool(a.ssh_dropbear) != bool(a.ssh_keys):
        sys.exit("--ssh-dropbear and --ssh-keys go together")
    inis = dict(spec.split("=", 1) for spec in a.app_ini)

    with tempfile.TemporaryDirectory() as d:
        stage = Path(d) / "stage"
        (stage / "apps").mkdir(parents=True)

        def put(src, rel, mode):
            dst = stage / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dst)
            os.chmod(dst, mode)
            print(f"embed      {rel}  {Path(src).stat().st_size} bytes")

        put(ROOT / "image" / "autoexec.sh", "autoexec.sh", 0o755)
        put(ROOT / "image" / "launcher.sh", "launcher.sh", 0o755)
        put(menu, "bootmenu", 0o755)
        if a.ssh_dropbear:
            put(a.ssh_dropbear, "dropbearmulti", 0o755)
            put(a.ssh_keys, "authorized_keys", 0o644)

        ids = set()
        for spec in a.app:
            src, _, app_id = spec.partition(":")
            src = Path(src).expanduser()
            app_id = app_id or src.name.removesuffix(".bin")
            if not src.is_file():
                sys.exit(f"no app at {src}")
            if (why := elf_arm_soft(src)):
                sys.exit(f"{src}: {why}")
            if app_id in ids:
                sys.exit(f"two apps named {app_id}")
            ids.add(app_id)
            put(src, f"apps/{app_id}/{app_id}", 0o755)
            ini = stage / "apps" / app_id / "app.ini"
            if app_id in inis:
                shutil.copy2(inis[app_id], ini)
            else:
                ini.write_text(f"name={app_id}\n")
            os.chmod(ini, 0o644)
        for spec in a.app_dir:
            src, _, app_id = spec.partition(":")
            src = Path(src).expanduser()
            app_id = app_id or src.name
            if not src.is_dir() or app_id in ids:
                sys.exit(f"bad --app-dir {spec}")
            ids.add(app_id)
            shutil.copytree(src, stage / "apps" / app_id)
            print(f"embed      apps/{app_id}/  (folder)")

        iso_path = Path(a.keep_iso) if a.keep_iso else Path(d) / "image.iso"
        subprocess.run([xorriso, "-as", "mkisofs", "-r", "-J", "-V", "AUTOEXEC", "-o", str(iso_path), str(stage)],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        iso = iso_path.read_bytes()
        out = Path(a.out)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(cipher.encrypt(iso, key))
        print(f"wrote      {out}  {out.stat().st_size} bytes  sha256 {hashlib.sha256(out.read_bytes()).hexdigest()}")

        # Verify: decrypt what was written and list the files in it.
        back = cipher.decrypt(out.read_bytes(), key)
        if back[32768:32768 + len(cipher.ISO_PRIMARY_MARKER)] != cipher.ISO_PRIMARY_MARKER:
            sys.exit("VERIFY FAILED: the decrypted image is not iso9660")
        check = Path(d) / "check.iso"
        check.write_bytes(back)
        listing = subprocess.run([xorriso, "-indev", str(check), "-find", "/", "-type", "f"],
                                 capture_output=True, text=True).stdout.split()
        files = sorted(p.strip("'") for p in listing if p.startswith("'/"))
        for need in ["/autoexec.sh", "/launcher.sh", "/bootmenu"] + [f"/apps/{i}/app.ini" for i in ids]:
            if need not in files:
                sys.exit(f"VERIFY FAILED: {need} not in the image ({files})")
        print("verify     ok:", " ".join(files))


if __name__ == "__main__":
    main()
