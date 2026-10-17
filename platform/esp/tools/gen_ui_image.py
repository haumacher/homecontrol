#!/usr/bin/env python3
# Copyright (c) 2026 Bernhard Haumacher
#
# Stages the web UI for the LittleFS image of the storage partition (doc/web.md):
#
#   gen_ui_image.py <web/ui/dist> <staging dir> [<language packs dir>]
#
# Every file of the UI is gzip-compressed (level 9, no name or time stamp, so the output depends on
# the content only) to <staging>/ui/<path>.gz; the device serves it with Content-Encoding: gzip.
# <staging>/ui/VERSION holds the UI's content hash (the first 16 hex digits of a SHA-256 over all
# paths and contents), which the device sends as the ETag of every file and reports as the UI
# version. The staging directory is emptied first, so removed files do not linger in the image.
#
# The language packs (doc/i18n.md) come from <language packs dir> (default: web/ui/lang next to
# dist) as ui/lang/<code>.json.gz, beside the base language dist/lang/en.json; other files in
# dist/lang (the POSIX build's copies of the packs) are not taken from there.

import gzip
import hashlib
import os
import shutil
import sys


def main():
    if len(sys.argv) not in (3, 4):
        sys.exit("usage: gen_ui_image.py <web/ui/dist> <staging dir> [<language packs dir>]")
    source, staging = sys.argv[1], sys.argv[2]
    packs = sys.argv[3] if len(sys.argv) == 4 else os.path.join(os.path.dirname(os.path.abspath(source)), "lang")
    if not os.path.isfile(os.path.join(source, "index.html")):
        sys.exit("gen_ui_image.py: %s has no index.html" % source)
    if os.path.isdir(staging):
        shutil.rmtree(staging)
    target = os.path.join(staging, "ui")
    os.makedirs(target)

    files = []  # (path in the image, source file)
    for root, dirs, names in os.walk(source):
        dirs.sort()
        for name in sorted(names):
            if name.startswith("."):
                continue
            path = os.path.join(root, name)
            rel = os.path.relpath(path, source).replace(os.sep, "/")
            if rel.startswith("lang/") and rel != "lang/en.json":
                continue
            files.append((rel, path))
    pack_count = 0
    if os.path.isdir(packs):
        for name in sorted(os.listdir(packs)):
            if name.endswith(".json") and not name.startswith(".") and name != "en.json":
                files.append(("lang/" + name, os.path.join(packs, name)))
                pack_count += 1
    files.sort()

    digest = hashlib.sha256()
    raw_total = 0
    packed_total = 0
    for rel, path in files:
        with open(path, "rb") as f:
            content = f.read()
        digest.update(rel.encode("utf-8") + b"\0" + len(content).to_bytes(8, "big") + content)
        packed = gzip.compress(content, compresslevel=9, mtime=0)
        out = os.path.join(target, rel + ".gz")
        os.makedirs(os.path.dirname(out), exist_ok=True)
        with open(out, "wb") as f:
            f.write(packed)
        raw_total += len(content)
        packed_total += len(packed)

    version = digest.hexdigest()[:16]
    with open(os.path.join(target, "VERSION"), "w") as f:
        f.write(version + "\n")
    print("web UI %s: %d files (%d language packs), %d B, gzip %d B" % (version, len(files), pack_count, raw_total,
                                                                      packed_total))


if __name__ == "__main__":
    main()
