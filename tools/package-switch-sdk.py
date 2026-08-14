#!/usr/bin/env python3
# Copyright © 2026 Mesa Switch contributors
# SPDX-License-Identifier: MIT

import argparse
import hashlib
import os
from pathlib import Path
import stat
import time
import zipfile


def parse_args():
    parser = argparse.ArgumentParser(
        description="Create a deterministic Mesa Switch SDK zip")
    parser.add_argument("--stage", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--epoch", type=int,
                        default=int(os.environ.get("SOURCE_DATE_EPOCH", "315532800")))
    return parser.parse_args()


def zip_timestamp(epoch):
    epoch = max(epoch, 315532800)
    stamp = time.gmtime(epoch)
    return stamp[:6]


def main():
    args = parse_args()
    stage = args.stage.resolve()
    output = args.output.resolve()
    if not stage.is_dir():
        raise SystemExit(f"SDK stage does not exist: {stage}")

    files = sorted(path for path in stage.rglob("*") if path.is_file())
    if not files:
        raise SystemExit(f"SDK stage is empty: {stage}")

    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_suffix(output.suffix + ".tmp")
    timestamp = zip_timestamp(args.epoch)
    with zipfile.ZipFile(temporary, "w", compression=zipfile.ZIP_DEFLATED,
                         compresslevel=9) as archive:
        for path in files:
            relative = path.relative_to(stage).as_posix()
            info = zipfile.ZipInfo(relative, timestamp)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.create_system = 3
            mode = stat.S_IFREG | (0o755 if os.access(path, os.X_OK) else 0o644)
            info.external_attr = mode << 16
            archive.writestr(info, path.read_bytes())

    temporary.replace(output)
    digest = hashlib.sha256(output.read_bytes()).hexdigest()
    output.with_suffix(output.suffix + ".sha256").write_text(
        f"{digest}  {output.name}\n", encoding="ascii", newline="\n")
    print(f"SDK: {output}")
    print(f"SHA256: {digest}")


if __name__ == "__main__":
    main()
