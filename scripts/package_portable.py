import argparse
import os
from pathlib import Path
import shutil
import tarfile


def main():
    parser = argparse.ArgumentParser(description="Prepare a self-contained VFS binary bundle.")
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--target", required=True, choices=("windows-x64", "linux-x64", "linux-arm64", "macos-x64", "macos-arm64"))
    parser.add_argument("--output", type=Path, default=Path("dist"))
    args = parser.parse_args()

    binary = args.binary.resolve()
    if not binary.is_file():
        parser.error(f"compiled executable not found: {binary}")
    root = args.output.resolve() / f"vfs-{args.target}"
    root.mkdir(parents=True, exist_ok=True)
    windows = args.target.startswith("windows-")
    executable = root / ("vfs.exe" if windows else "vfs")
    shutil.copy2(binary, executable)
    shutil.copy2(Path("LICENSE"), root / "LICENSE")
    launcher = "Run VFS.bat" if windows else ("Run VFS.command" if args.target.startswith("macos-") else "run-vfs.sh")
    if windows:
        (root / launcher).write_text('@echo off\r\ncd /d "%~dp0"\r\nvfs.exe\r\npause\r\n', encoding="ascii")
    else:
        (root / launcher).write_text('#!/bin/sh\nset -eu\ncd "$(dirname "$0")"\nexec ./vfs\n', encoding="utf-8")
        os.chmod(executable, 0o755)
        os.chmod(root / launcher, 0o755)
    readme = (
        f"Virtual File System - portable build ({args.target})\n\n"
        "No compiler, CMake, or third-party runtime is required to run this bundle.\n"
        "Windows: double-click Run VFS.bat.\n"
        "macOS: double-click Run VFS.command, or run it from Terminal.\n"
        "Linux: open a terminal in this folder and run ./run-vfs.sh.\n\n"
        "The program creates virtual_disk.dat beside the executable. Keep this folder writable.\n"
        "The disk image is the simulator's data file; it is not a host filesystem mount.\n"
        "For source builds and project details, see the repository README.\n"
    )
    (root / "README.txt").write_text(readme, encoding="utf-8")
    archive = args.output.resolve() / f"vfs-{args.target}.tar.gz"
    with tarfile.open(archive, "w:gz") as tar:
        tar.add(root, arcname=root.name)
    print(f"PACKAGE={archive}")
    print(f"BUNDLE={root}")


if __name__ == "__main__":
    main()
