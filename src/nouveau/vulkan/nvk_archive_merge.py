#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def resolve_archiver(requested: str) -> str:
    requested_path = Path(requested)
    if requested_path.is_file():
        return str(requested_path)

    discovered = shutil.which(requested)
    if discovered is not None:
        return discovered

    # Native Meson launched from an MSYS2 environment can omit /usr/bin from
    # the Windows child-process PATH.  The Horizon cross compiler still ships
    # a GNU MRI-capable archiver; find it relative to DEVKITPRO or the active
    # MSYS2 Python rather than baking that host detail into meson.build.
    candidates: list[Path] = []
    devkitpro = os.environ.get("DEVKITPRO")
    if devkitpro:
        candidates.append(
            Path(devkitpro) / "devkitA64" / "bin" / "aarch64-none-elf-gcc-ar.exe"
        )

    python_path = Path(sys.executable).resolve()
    for parent in python_path.parents:
        candidates.extend(
            [
                parent / "usr" / "bin" / "ar.exe",
                parent
                / "opt"
                / "devkitpro"
                / "devkitA64"
                / "bin"
                / "aarch64-none-elf-gcc-ar.exe",
            ]
        )

    for candidate in candidates:
        if candidate.is_file():
            return str(candidate)

    raise FileNotFoundError(
        f"unable to find GNU archiver {requested!r}; set DEVKITPRO or add ar to PATH"
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--ar", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--whole-archive", action="append", default=[])
    parser.add_argument("archives", nargs="+")
    parser.add_argument("--link-command", nargs=argparse.REMAINDER)
    args = parser.parse_args()

    output_path = Path(args.output).resolve()
    input_paths = [Path(archive).resolve() for archive in args.archives]
    whole_paths = [Path(archive).resolve() for archive in args.whole_archive]
    if whole_paths and not args.link_command:
        parser.error("--whole-archive requires --link-command")
    if len(set(whole_paths)) != len(whole_paths):
        parser.error("duplicate --whole-archive input")
    if not set(whole_paths).issubset(input_paths):
        parser.error("--whole-archive inputs must also be listed in archives")
    output_path.parent.mkdir(parents=True, exist_ok=True)

    # Mesa uses thin archives internally, so they must remain at their
    # original paths while ar reads their members.  Run MRI from a common
    # ancestor and use relative, whitespace-safe paths in the command stream.
    common_root = Path(
        os.path.commonpath([output_path.parent, *(path.parent for path in input_paths)])
    )
    temp_output = output_path.with_name(output_path.name + ".tmp")
    temp_output.unlink(missing_ok=True)

    with tempfile.TemporaryDirectory(prefix="nvk-link-", dir=output_path.parent) as tmp:
        mri_lines = [
            "CREATE " + Path(os.path.relpath(temp_output, common_root)).as_posix()
        ]
        if whole_paths:
            # Bind weak dispatch references before standalone archive extraction.
            linked = Path(tmp) / "vulkan-dispatch.o"
            subprocess.run([
                *args.link_command, "-nostdlib", "-r", "-Wl,--whole-archive",
                *(str(path) for path in whole_paths), "-Wl,--no-whole-archive",
                "-o", str(linked),
            ], check=True)
            linked_relative = Path(os.path.relpath(linked, common_root)).as_posix()
            mri_lines.append(f"ADDMOD {linked_relative}")

        for archive in input_paths:
            if archive in whole_paths:
                continue
            archive_relative = Path(os.path.relpath(archive, common_root)).as_posix()
            mri_lines.append(f"ADDLIB {archive_relative}")

        mri_lines.extend(["SAVE", "END", ""])
        subprocess.run(
            [resolve_archiver(args.ar), "-M"],
            cwd=common_root,
            input="\n".join(mri_lines),
            text=True,
            check=True,
        )

    os.replace(temp_output, output_path)


if __name__ == "__main__":
    main()
