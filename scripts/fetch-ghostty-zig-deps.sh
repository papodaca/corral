#!/usr/bin/env bash
# Fill ZIG_GLOBAL_CACHE_DIR with Ghostty's Zig packages.
# Nested bubblewrap (Flatpak) often cannot resolve DNS even with --share=network.
#
# Do not use `zig fetch` for this. Zig 0.17 rejects --global-cache-dir after
# the fetch subcommand, and the tarball `zig fetch` writes is wrapped in an
# extra directory. zig build then hashes that as a different package.
# Download with curl and store p/<declared-hash>.tar.gz with the hash as the
# only prefix, which is what zig build reads back.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
SRC="${ROOT}/third_party/ghostty"
CACHE=${ZIG_GLOBAL_CACHE_DIR:-${ROOT}/third_party/zig-cache}

if [[ ! -f ${SRC}/build.zig.zon ]]; then
  echo "Ghostty sources missing at ${SRC}. Run scripts/sync-ghostty.sh" >&2
  exit 1
fi

mkdir -p "${CACHE}/p"
export ZIG_GLOBAL_CACHE_DIR="${CACHE}"

python3 - "${SRC}" "${CACHE}" << 'PY'
import re
import subprocess
import sys
import tarfile
import tempfile
import time
from pathlib import Path

src = Path(sys.argv[1])
cache_p = Path(sys.argv[2]) / "p"
cache_p.mkdir(parents=True, exist_ok=True)

pairs = []
seen = set()
for zon in src.rglob("*.zig.zon"):
    if "example" in zon.parts:
        continue
    text = zon.read_text(errors="replace")
    for match in re.finditer(
        r'\.url\s*=\s*"(https://[^"]+)"\s*,\s*\.hash\s*=\s*"([^"]+)"',
        text,
    ):
        url, digest = match.group(1), match.group(2)
        if digest in seen:
            continue
        seen.add(digest)
        pairs.append((url, digest))

if not pairs:
    sys.exit("no Zig package URLs found under " + str(src))

def download(url, dest):
    last = None
    for attempt in range(1, 6):
        print(f"  curl {url} (try {attempt})", flush=True)
        try:
            subprocess.run(
                [
                    "curl", "-fL", "--http1.1", "-sS",
                    "--retry", "3", "--retry-all-errors", "--retry-delay", "2",
                    "--speed-limit", "10000", "--speed-time", "20",
                    "--max-time", "180",
                    "-o", str(dest), url,
                ],
                check=True,
            )
            return
        except subprocess.CalledProcessError as err:
            last = err
            dest.unlink(missing_ok=True)
            time.sleep(attempt * 2)
    raise last

def pack(archive, digest, dest):
    unpack = Path(tempfile.mkdtemp())
    subprocess.run(["tar", "-xf", str(archive), "-C", str(unpack)], check=True)
    entries = []
    for path in unpack.rglob("*"):
        if path.is_symlink() or path.is_file():
            entries.append(path)
    rels = [path.relative_to(unpack).as_posix() for path in entries]
    tops = {rel.split("/", 1)[0] for rel in rels}
    strip = len(tops) == 1 and all("/" in rel for rel in rels)
    with tarfile.open(dest, "w:gz") as out:
        for path, rel in zip(entries, rels):
            if strip:
                rel = rel.split("/", 1)[1]
            if not rel:
                continue
            info = tarfile.TarInfo(f"{digest}/{rel}")
            info.mtime = 0
            info.mode = path.lstat().st_mode & 0o777
            if path.is_symlink():
                info.type = tarfile.SYMTYPE
                info.linkname = str(path.readlink())
                info.size = 0
                out.addfile(info)
            else:
                info.size = path.stat().st_size
                with path.open("rb") as raw:
                    out.addfile(info, raw)

for url, digest in pairs:
    dest = cache_p / f"{digest}.tar.gz"
    print(f"cache {digest}", flush=True)
    if dest.is_file() and dest.stat().st_size > 0:
        continue
    tmp = Path(tempfile.mkdtemp())
    archive = tmp / "pkg.tar"
    download(url, archive)
    pack(archive, digest, dest)
PY

echo "Zig package cache ready at ${CACHE}"
