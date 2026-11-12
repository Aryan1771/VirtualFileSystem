# Virtual File System with Permissions and Recovery

A C++17 command-line simulation of a small persistent file system. It stores a virtual disk image in `virtual_disk.dat`; the host file system is used only to store that image and its recovery journal.

## Features

- Nested directories and file create, read, overwrite, append, rename, and recursive removal.
- A fixed-size block device backed by `virtual_disk.dat`, bitmap allocation, free-space reporting, and free-extent fragmentation metrics.
- Per-entry read, write, and execute bits; `admin` manages users, while owners and `admin` can change permissions.
- A write-ahead journal that replays a fully written transaction after a simulated interruption.
- Thread-safe operations and a condition-variable worker pool for concurrent reads.
- FIFO and LRU block-cache policies with hit and miss counters.
- FCFS, SSTF, SCAN, and C-SCAN disk request scheduling with total head movement.
- A session operation log and validation of image signature, version, checksum, allocation map, and paths at mount.

The simulator is intended for operating-systems coursework. It does not mount a real disk or provide production-grade security or hardware-failure guarantees.

## Build

Requirements: CMake 3.16+, a C++17 compiler, and Ninja or another CMake generator.

### Windows with MSYS2 UCRT64

Open an MSYS2 UCRT64 shell:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
./build/vfs.exe
```

### Linux or macOS

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
./build/vfs
```

On the first run, the program creates a 512-block disk with 512-byte blocks and an `admin` user. To choose a different image or capacity, pass `--disk FILE` and `--blocks COUNT` on the first run. Existing images retain their stored capacity.

```sh
vfs --disk lab_disk.dat --blocks 1024
```

Keep the disk image and its `.journal` file together when moving or backing up a disk. Do not edit either file by hand.

## Commands

Paths are virtual paths rooted at `/`. Commands accept quoted text so spaces can appear in file contents.

| Command | Purpose |
| --- | --- |
| `mkdir PATH` | Create a directory |
| `touch PATH` | Create an empty file |
| `write PATH "TEXT"` | Replace a file's contents |
| `append PATH "TEXT"` | Append text to a file |
| `cat PATH` | Read a file |
| `ls [PATH]` | List direct children |
| `stat PATH` | Display entry metadata |
| `mv SOURCE DEST` | Rename or move a file/directory |
| `rm PATH` | Remove a file or directory tree |
| `useradd NAME` | Add a user (admin only) |
| `su USER` | Switch the active user |
| `chmod PATH rwx` | Set permission bits; use `-` to clear a bit |
| `access PATH r|w|x` | Check read, write, or execute permission |
| `df` | Report used/free blocks and free extents |
| `cache-policy FIFO` / `cache-policy LRU` | Choose cache replacement policy |
| `cache-stats` | Show cache hits and misses |
| `schedule ALG HEAD R1,R2,... [up|down]` | Simulate disk scheduling |
| `concurrent-read PATH [PATH ...]` | Read files using worker threads |
| `log` | Show operations in this run |
| `license` | Print the bundled GPL-3.0 license |
| `help` / `exit` | Show commands / quit |

Example session:

```text
vfs[admin]> mkdir /docs
vfs[admin]> touch /docs/readme.txt
vfs[admin]> write /docs/readme.txt "Persistent data"
vfs[admin]> cat /docs/readme.txt
Persistent data
vfs[admin]> useradd student
vfs[admin]> chmod /docs/readme.txt r--
vfs[admin]> su student
vfs[student]> cat /docs/readme.txt
Persistent data
```

## Crash recovery demonstration

Create a file and write its initial contents, then run `crash-write`:

```text
vfs[admin]> touch /pending.txt
vfs[admin]> write /pending.txt "before"
vfs[admin]> crash-write /pending.txt "after"
simulated crash: pending transaction is journaled; restart to recover
```

The command returns exit code 77 and leaves `virtual_disk.dat.journal` in place. Start the same disk again and inspect the recovered value:

```text
vfs[admin]> cat /pending.txt
after
```

The journal contains a complete checksummed image. On startup, recovery validates it, installs it as the current image, and clears the journal.

## Design

- `VirtualFileSystem` owns metadata, block allocation, permissions, disk I/O, cache statistics, and journal recovery.
- Directory and file records are indexed by normalized virtual paths; file data is stored in fixed-size blocks referenced by each file record.
- Mutations operate on a candidate state. The candidate image is written to the journal before the disk image, so an interrupted commit can be replayed.
- A mutex protects shared filesystem state. `WorkerPool` uses a task queue, condition variable, and worker threads to demonstrate concurrent requests safely.
- The standalone algorithms module implements FIFO/LRU cache behavior and disk scheduling.

## Tests

CTest covers persistence across remounts, multi-block data, recursive rename and deletion, path traversal rejection, access control, journal replay, disk-full rollback, cache counters, scheduler order and movement, and worker-pool results.
