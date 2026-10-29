# xv6 Memory Snapshots

Per-process memory snapshots for the **xv6** teaching kernel (x86, C). A process can save up to four copies of its own memory, roll back to any of them, compare two of them page by page, and free them, through six new system calls.

Operating Systems course assignment, School of Computing (RAF), Belgrade, 2026.

## What it does

A snapshot is an independent deep copy of a process's user memory: its own two-level x86 page table and its own physical pages. It is the same idea as a VM snapshot or checkpoint/restore, inside a small Unix-like kernel.

| System call | What it does | Returns |
|---|---|---|
| `int snap_take(int slot)` | Copies the current user memory into `slot` (0–3) | `0` ok · `-1` out of memory · `-2` invalid slot · `-3` slot already used |
| `int snap_restore(int slot)` | Replaces the current memory with a copy of the snapshot. The snapshot stays, so the same state can be restored many times | `0` ok · `-1` empty slot · `-2` invalid slot · `-3` out of memory |
| `int snap_diff(int a, int b)` | Counts the pages whose contents differ between two snapshots. Pages that exist in only one of them count as different | `>=0` pages · `-1` empty slot · `-2` invalid slot |
| `int snap_release(int slot)` | Frees the snapshot's page table and physical pages | `0` ok · `-1` already empty · `-2` invalid slot |
| `int snap_status(int slot, struct snap_info *si)` | Writes the snapshot's size in bytes and number of physical pages to `si` | `0` ok · `-1` empty slot · `-2` invalid slot or pointer |
| `int snap_empty(void)` | Returns the first free slot | `>=0` slot · `-1` all slots used |

## How it works

- **Built on what xv6 already does well.** `fork` uses `copyuvm()`, which builds a new page table, allocates fresh physical pages and copies the contents to the same virtual addresses, and frees everything it allocated if memory runs out. `snap_take` is `copyuvm()` plus bookkeeping; `snap_release` and process exit use `freevm()`.
- **Restore never leaves the process half-replaced.** `snap_restore` first builds a fresh copy of the snapshot. Only if that succeeds does it install the new page table in CR3 with `switchuvm()` (which also flushes the TLB) and then free the old one. Freeing first would pull the page table out from under the running CPU.
- **Page-by-page comparison.** `snap_diff` walks both page tables with `walkpgdir()`, finds each physical page through `P2V(PTE_ADDR(pte))`, and compares contents with `memcmp()`.
- **Whole process lifecycle covered.** `allocproc` starts every process with empty slots, `fork` gives the child its own copies of the parent's snapshots (and undoes a partial copy if memory runs out), and `exit` frees all snapshots, so memory does not leak.
- **User pointers are checked.** `sys_snap_status` validates the user's struct pointer with `argptr()` before the kernel writes to it.
- **Restore rewinds execution, too.** The stack is part of the restored memory, so the program resumes right after the `snap_take` call that created the snapshot, much like `longjmp`. The test tells the two passes apart with a second "marker" snapshot: slot state lives in the kernel, so it survives the restore.
- The kernel prints nothing; errors are reported only through return codes, as the assignment required.

## Where the code is

| File | Change |
|---|---|
| [`kernel/vm.c`](kernel/vm.c) | The six system calls and the helpers used by `fork`/`exit` (bottom of the file) |
| [`kernel/proc.h`](kernel/proc.h), [`kernel/param.h`](kernel/param.h) | `struct snimak { pgdir, sz }` and 4 slots per process |
| [`kernel/proc.c`](kernel/proc.c) | Hooks in `allocproc`, `fork` and `exit` |
| [`kernel/stat.h`](kernel/stat.h) | `struct snap_info { size, num_pages }` |
| `kernel/sysproc.c`, `syscall.c`, `syscall.h`, `defs.h` | System call wrappers and table entries (calls 22–27) |
| `user/user.h`, `user/usys.S` | User-space stubs |
| [`user/snaptest.c`](user/snaptest.c) | Test program, 41 checks |

Everything else is unchanged xv6, except two build tweaks in the `Makefile`: the Perl helper scripts are run through `perl`, so the project builds even if file permissions were lost, and the xv6 readme was renamed to `README.xv6`.

Identifiers and comments are in Serbian, the course language: *snimak* = snapshot, *snimci* = snapshots, *broj stranica* = number of pages, *kopiraj* = copy, *oslobodi* = free.

## Build and run

You need Linux (or WSL), `make`, `perl`, QEMU, and a GCC that can build 32-bit code (or an `i686-elf` cross-compiler). On Ubuntu or Debian:

```sh
sudo apt install build-essential gcc-multilib qemu-system-x86
make qemu-nox
```

xv6 boots in the terminal. At the `$` prompt, run the tests:

```
$ snaptest
```

To quit QEMU, press `Ctrl-A`, then `X`.

## Tests

`user/snaptest.c` checks normal use and the error cases: the first and last valid slot, invalid and negative slots, a used slot, all slots full, slot reuse after release, an invalid user pointer, a real rollback (memory changed after the snapshot comes back, and the snapshot is still there afterwards), and a forked child that inherits snapshots as independent copies. All 41 checks pass, and xv6's own `usertests` and `forktest` still pass. The test output is in Serbian:

```
=== snaptest: pocetak ===
[OK ] snap_empty na pocetku vraca 0 (prvi prazan slot)
[OK ] snap_take(0) uspeva [donja granica slota]
...
[OK ] snap_diff(0,1) > 0 (bafer je promenjen)
    -> snap_diff(0,1) = 4 razlicitih stranica
...
[OK ] [roditelj] slot 0 i dalje pun posle fork/release deteta (kopije su nezavisne)
=== snaptest: kraj ===
```

## Credits

Based on [xv6](https://github.com/mit-pdos/xv6-public) by Frans Kaashoek, Robert Morris and Russ Cox (MIT License, see [`LICENSE`](LICENSE) and [`README.xv6`](README.xv6)), in the version used for the Operating Systems course at RAF.
