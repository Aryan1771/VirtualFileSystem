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

The simulator is intended for operating-systems coursework. It does not mount a real disk or provide production-grade security or hardware-failure guarantees. In particular, `crash-write` demonstrates recovery after a journaled interruption; it does not simulate power loss or guarantee physical-media durability.

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

On the first run, the program creates a 512-block disk with 512-byte blocks and an `admin` user. The supported disk size is 1 to 16,384 blocks. To choose a different image or capacity, pass `--disk FILE` and `--blocks COUNT` on the first run. Existing images retain their stored capacity; a non-default requested capacity must match the image.

```sh
vfs --disk lab_disk.dat --blocks 1024
```

Keep the disk image and its `.journal` file together when moving or backing up a disk. Do not edit either file by hand.

## Commands

The prompt shows the active user and current virtual directory. Paths are rooted at `/`; relative paths are resolved from the directory shown in the prompt, and `.` / `..` cannot escape the virtual root. Commands accept quoted text; within quotes, `\n`, `\r`, and `\t` represent newline, carriage return, and tab. Linux-like commands operate on the simulated disk; `export` is the explicit command for copying virtual data to host files.

| Command | Purpose |
| --- | --- |
| `mkdir PATH` | Create a directory |
| `touch PATH` | Create an empty file |
| `write PATH "TEXT"` | Replace a file's contents |
| `append PATH "TEXT"` | Append text to a file |
| `cat PATH` | Read a file |
| `head [-n COUNT] PATH` / `tail [-n COUNT] PATH` | Show the first or last lines of a file |
| `wc PATH` | Count newline characters, words, and bytes |
| `echo TEXT` | Print text to the terminal |
| `grep [-i] PATTERN PATH` | Search lines in a virtual text file |
| `sort [-r] PATH` | Sort text-file lines |
| `uniq [-c] PATH` | Collapse adjacent duplicate lines, optionally with counts |
| `cut -d DELIM -f FIELDS PATH` | Select fields from delimiter-separated lines |
| `pwd` / `cd [PATH]` | Show or change the current virtual directory |
| `ls [-l] [PATH]` | List direct children and display the directory path |
| `tree [PATH]` | Display nested files and directories |
| `find [PATH] NAME` | Recursively find names containing text |
| `cp SOURCE DEST` | Copy a virtual file |
| `export PATH HOST_PATH [--force]` | Copy a virtual file or nested directory to the host computer |
| `rmdir PATH` | Remove an empty directory |
| `stat PATH` | Display entry metadata |
| `mv SOURCE DEST` | Rename or move a file/directory |
| `rm PATH` / `rm -r PATH` | Remove a file / recursively remove an entry tree |
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
| `man [COMMAND]` | Show built-in command documentation |
| `clear` | Clear a compatible terminal display |
| `COMMAND > PATH` / `COMMAND >> PATH` | Save or append command output to a virtual file |
| `csv-info PATH` | Show CSV dimensions and header names |
| `describe PATH [COLUMN]` | Calculate numeric descriptive statistics |
| `freq PATH COLUMN` | Count categorical values |
| `filter PATH COLUMN OP VALUE` | Filter CSV rows |
| `select PATH COLUMN[,COLUMN...]` | Select CSV columns |
| `groupby PATH KEY [VALUE_COLUMN]` | Count categories or aggregate numeric groups |
| `histogram PATH COLUMN [BINS]` | Bin numeric CSV values |
| `join LEFT RIGHT KEY` | Inner-join CSV files on a shared header |
| `license` | Print the bundled GPL-3.0 license |
| `help` / `exit` | Show commands / quit |

`man` documents the built-in commands. This is a focused Linux-style filesystem CLI, not a general-purpose shell: filesystem commands operate on the virtual disk. `export` writes a virtual file or directory tree to an explicitly chosen host path. Existing targets are refused unless `--force` is supplied; force mode replaces matching files while keeping unrelated files. Output redirection writes to the virtual disk; pipes and executable programs are outside its scope.

### Exporting to the host computer

Use `export VIRTUAL_PATH HOST_PATH` to copy a virtual file to a local path, or recursively copy a virtual directory and its nested contents. Quote host paths containing spaces, for example `export /reports/summary.csv "C:/Users/Student/Desktop/summary.csv"`. Existing destination paths are refused by default. Add `--force` to replace matching files; unrelated files in an existing destination directory are retained. The simulator checks virtual read permissions before writing the export.

### CSV analysis

CSV files use the first row as headers, require a consistent number of fields per row, and support quoted fields, escaped quotes, and embedded newlines. Column arguments accept a header name or a 1-based column number. Numeric summaries skip empty values and report population standard deviation. Filter operators are `=`, `!=`, `>`, `>=`, `<`, `<=`, and `contains`.

```text
vfs[admin]:/> write /sales.csv "region,amount\nWest,120\nEast,75\nWest,90"
vfs[admin]:/> csv-info /sales.csv
vfs[admin]:/> describe /sales.csv amount
vfs[admin]:/> groupby /sales.csv region amount > /regional-summary.csv
vfs[admin]:/> cat /regional-summary.csv
```

Example session:

```text
vfs[admin]:/> mkdir /docs
vfs[admin]:/> touch /docs/readme.txt
vfs[admin]:/> write /docs/readme.txt "Persistent data"
vfs[admin]:/> cd /docs
vfs[admin]:/docs> pwd
/docs
vfs[admin]:/docs> ls
Directory: /docs
readme.txt
vfs[admin]:/docs> cat readme.txt
Persistent data
vfs[admin]:/docs> useradd student
vfs[admin]:/docs> chmod readme.txt r--
vfs[admin]:/docs> su student
vfs[student]:/docs> cat readme.txt
Persistent data
```

For example, compare scheduling policies on the same request queue:

```text
vfs[admin]> schedule FCFS 25 10,45,5
service order: 10 45 5
head movement: 90 blocks
vfs[admin]> schedule SSTF 25 10,45,5
service order: 10 5 45
head movement: 60 blocks
```

Scheduling results are model calculations over block numbers. The cache commands similarly track simulated block identifiers and report replacement-policy hits and misses; they do not represent a hardware disk scheduler or cache of file bytes.

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

### Source layout

| Path | Responsibility |
| --- | --- |
| `src/main.cpp` | CLI parsing, command dispatch, and reports |
| `src/vfs.cpp` | Virtual paths, directory entries, permissions, allocation, persistence, and recovery |
| `src/algorithms.cpp` | FIFO/LRU cache accounting and disk scheduling algorithms |
| `src/worker_pool.cpp` | Worker lifecycle, task queue, and condition-variable synchronization |
| `include/` | Public interfaces for the filesystem, algorithms, and worker pool |
| `tests/test_vfs.cpp` | Automated behavior and regression coverage |

### Persistence sequence

Each mutation serializes a candidate image with its checksum, writes the journal, then replaces the disk image. A normal successful commit removes the journal. If the process exits after journaling but before replacing the disk image, the next start validates and replays the journal. The journal and disk image should be backed up or moved together.

## Tests

CTest covers persistence across remounts, multi-block data, recursive rename and deletion, path traversal rejection, access control, journal replay, disk-full rollback, cache counters, scheduler order and movement, and worker-pool results.
