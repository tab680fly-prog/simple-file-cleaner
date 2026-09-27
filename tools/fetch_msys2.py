#!/usr/bin/env python3
"""Download the MSYS2 UCRT64 packages needed to cross-compile File Cleaner
for Windows (GTK 4, libadwaita and all their runtime dependencies) and
extract them into a sysroot.

Usage: fetch_msys2.py <output-dir>

Requires `zstd` and `tar` on PATH. Packages are cached in <output-dir>/pkgs,
so re-running only downloads what changed.
"""

import os
import re
import subprocess
import sys
import tarfile
import urllib.request

REPO = "https://repo.msys2.org/mingw/ucrt64"
PREFIX = "mingw-w64-ucrt-x86_64-"
ROOTS = ["gtk4", "libadwaita", "adwaita-icon-theme", "librsvg", "hicolor-icon-theme"]
# Build tools and interpreters pulled in by package metadata that are not
# needed at runtime by a GTK application.
SKIP = re.compile(r"^(python|perl|tcl|tk|gcc|binutils|crt|headers|windows-default-manifest|"
                  r"winpthreads-git|pkgconf|gettext-tools|ca-certificates|p11-kit|gtk-update-icon-cache)")


def fetch(url, dest):
    if os.path.exists(dest):
        return
    tmp = dest + ".part"
    with urllib.request.urlopen(url) as r, open(tmp, "wb") as f:
        while chunk := r.read(1 << 20):
            f.write(chunk)
    os.replace(tmp, dest)


def zst_tar(path):
    """Returns an open tarfile for a .tar.zst file via the zstd binary."""
    proc = subprocess.Popen(["zstd", "-dc", path], stdout=subprocess.PIPE)
    return tarfile.open(fileobj=proc.stdout, mode="r|"), proc


def load_db(db_path):
    pkgs, provides = {}, {}
    tf, proc = zst_tar(db_path)
    for member in tf:
        if not member.name.endswith("/desc"):
            continue
        text = tf.extractfile(member).read().decode()
        fields, key = {}, None
        for line in text.splitlines():
            if line.startswith("%") and line.endswith("%"):
                key = line.strip("%")
                fields[key] = []
            elif line and key:
                fields[key].append(line)
        name = fields["NAME"][0]
        pkgs[name] = fields
        for p in fields.get("PROVIDES", []):
            provides[re.split(r"[<>=]", p)[0]] = name
    proc.wait()
    return pkgs, provides


def main():
    out = os.path.abspath(sys.argv[1])
    pkg_dir = os.path.join(out, "pkgs")
    sysroot = os.path.join(out, "sysroot")
    os.makedirs(pkg_dir, exist_ok=True)
    os.makedirs(sysroot, exist_ok=True)

    db = os.path.join(pkg_dir, "ucrt64.db")
    if os.path.exists(db):
        os.remove(db)  # always refresh the package index
    fetch(f"{REPO}/ucrt64.db", db)
    pkgs, provides = load_db(db)

    wanted, queue = set(), [PREFIX + r for r in ROOTS]
    while queue:
        dep = re.split(r"[<>=]", queue.pop())[0]
        name = dep if dep in pkgs else provides.get(dep)
        if not name or name in wanted:
            continue
        if SKIP.match(name[len(PREFIX):]) and name[len(PREFIX):] != "gcc-libs":
            continue
        wanted.add(name)
        queue.extend(pkgs[name].get("DEPENDS", []))

    print(f"{len(wanted)} packages")
    for name in sorted(wanted):
        filename = pkgs[name]["FILENAME"][0]
        dest = os.path.join(pkg_dir, filename)
        if not os.path.exists(dest):
            print("  downloading", filename, flush=True)
            fetch(f"{REPO}/{filename}", dest)
        stamp = os.path.join(sysroot, ".extracted-" + filename)
        if not os.path.exists(stamp):
            subprocess.run(["tar", "--use-compress-program=zstd -d", "-xf", dest, "-C", sysroot,
                            "--exclude=.PKGINFO", "--exclude=.BUILDINFO", "--exclude=.MTREE",
                            "--exclude=.INSTALL"], check=True)
            open(stamp, "w").close()


if __name__ == "__main__":
    main()
