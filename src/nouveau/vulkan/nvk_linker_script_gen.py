#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

import argparse
import os
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", required=True)
    parser.add_argument("--link-library", action="append", default=[])
    parser.add_argument("archives", nargs="+")
    args = parser.parse_args()

    out_path = Path(args.out)
    out_dir = out_path.parent.resolve()

    lines = ["/* Auto-generated; see nvk_linker_script_gen.py. */", "GROUP ("]
    for archive in args.archives:
        archive_path = Path(archive).resolve()
        rel_path = os.path.relpath(archive_path, start=out_dir)
        lines.append(f"  {Path(rel_path).as_posix()}")
    for library in args.link_library:
        lines.append(f"  -l{library}")
    lines.append(")")
    lines.append("")

    out_path.write_text("\n".join(lines), encoding="utf-8")


if __name__ == "__main__":
    main()
