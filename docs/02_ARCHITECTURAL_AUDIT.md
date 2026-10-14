# Architectural Audit & Remediation Plan (v0.1 -> v2.0)

This document catalogs the architectural bottlenecks, design anti-patterns, and implementation bugs identified in the v0.1 prototype[cite: 1] and specifies the engineering remediations for v2.0.

---

## 1. High-Level Architectural Bottlenecks

### 1.1 Pervasive `static` Class State & Global Variables
* **Current Flaw:** In `include/container.hpp`, `include/terminal.hpp`, and `include/network.hpp`, almost all member fields (`m_child_pid`, `m_container_id`, `m_volumes`, `m_forward_ports`, `m_sfd`) are declared `static`[cite: 1]. Similarly, `src/command_line_handler.cpp` stores parsed CLI flags in file-scope global variables[cite: 1].
* **Impact:** `Container`, `Terminal`, and `Network` are not true instantiable objects. Managing multiple containers within the same process space overwrites shared static memory, preventing multi-container orchestration and unit testing.
* **Remediation:** Strip `static` from all instance variables. Encapsulate container configuration inside an immutable `ContainerSpec` struct passed by `const&` into RAII-managed `Container`, `TerminalServer`, and `NetworkSession` instances.

### 1.2 Centralized SQLite State Store (`src/database_manager.cpp`)
* **Current Flaw:** Container, volume, image, and network metadata are stored in a single SQLite database (`~/.quiver/quiver.db`) managed via 460+ lines of raw `sqlite3_prepare_v2` boilerplate[cite: 1].Furthermore, `src/terminal.cpp` instantiates a rogue global `DatabaseManager db{ Utils::get_base_dir() + "quiver.db" }` (missing a path separator) on startup[cite: 1].
* **Impact:** When spawning dozens of containers concurrently, forked manager processes contend for SQLite write locks (`SQLITE_BUSY`), creating a serialization bottleneck.
* **Remediation:** Remove `sqlite3` entirely. Implement an OCI-compliant, lock-free `StateStore` using `nlohmann::json` (already a project dependency)[cite: 1] where each container owns an isolated directory (`~/.quiver/containers/<id>/state.json`) updated atomically via `rename(2)`.

### 1.3 Intrusive Distro Package Manager Mutation (`src/package_manager.cpp`)
* **Current Flaw:** On every container boot, `PackageManager::initialize()` inspects `/etc/os-release` and overwrites `/etc/apt`, `/etc/dpkg`, `/etc/pacman.conf`, or `/etc/yum.conf` inside the container rootfs (including calling `system("rpm --initdb")`)[cite: 1].
* **Impact:** Violates container immutability, slows down startup, and risks corrupting pre-configured OCI images.
* **Remediation:** Delete `src/package_manager.cpp` and `include/package_manager.hpp`[cite: 1]. Replace with native OCI Image Configuration parsing (`config.json` from the registry manifest) to respect the image's native `Env`, `WorkingDir`, `Entrypoint`, and `Cmd`.

### 1.4 Missing Cgroup v2 Resource Isolation
* **Current Flaw:** While process namespace isolation (`CLONE_NEWUSER`, `CLONE_NEWPID`, `CLONE_NEWNS`, `CLONE_NEWUTS`, `CLONE_NEWNET`) is implemented in `src/container.cpp`[cite: 1], hardware resource limits (CPU, Memory, PIDs) are completely absent[cite: 1].
* **Impact:** A single container can exhaust host RAM (triggering the host OOM killer) or execute a fork bomb.
* **Remediation:** Implement `CgroupManager` targeting the Linux **cgroup v2** unified hierarchy (`/sys/fs/cgroup/`), supporting `--memory`, `--cpus`, and `--pids-limit`.

---

## 2. Concrete Implementation Bugs to Fix

| File & Function | Identified Bug in v0.1 | v2.0 Fix |
| :--- | :--- | :--- |
| **`src/utils.cpp`** (`extract_tarball`) | Calls `execlp("tar", "tar", "-xzf", tarball_path.c_str(), "-", destination_path.c_str(), NULL)`[cite: 1]. Passing `"-"` instead of `"-C"` causes `tar` to fail directory extraction. | Replace `"-"` with `"-C"` and add `--no-same-owner` for rootless layer extraction. |
| **`src/container.cpp`** (`setup_user_namespace`) | Hardcodes `"0 1000 1\n"` for `uid_map`/`gid_map` and writes to `/proc/self/uid_map` from the **parent** process instead of targeting the child PID[cite: 1]. | Dynamically query `getuid()` / `getgid()` and write to `/proc/<child_pid>/uid_map` and `/proc/<child_pid>/gid_map`. |
| **`src/container.cpp`** (`run_container`) | Mounts OverlayFS with `lowerdir=...,upperdir=...,workdir=...` without `userxattr`[cite: 1], causing permission errors in rootless user namespaces and forcing a slow `system("cp -a ...")` VFS fallback[cite: 1]. | Append `,userxattr` to OverlayFS mount options (Linux 5.11+) and replace `system("cp -a ...")` with `std::filesystem::copy`. |
| **`src/container.cpp`** (`run_container`) | Ignores `args.commands` and `args.program_path`, hardcoding `execl(shell, shell, (char*)0)`[cite: 1]. | Execute user-supplied command vectors via `execvp()`, falling back to `/bin/sh` only when no command is passed. |
| **`src/command_line_handler.cpp`** (`vfs`) | `std::string vfs_path{ db_manager.get_container(container_id).vfs };` passes a `bool` to `std::string` instead of `.vfs_path`[cite: 1]. | Fixed automatically by replacing `DatabaseManager` with strongly-typed JSON state structs. |
| **`src/database_manager.cpp`** (`remove_vfs` & `list_containers_by_image`) | `remove_vfs` throws an error when `sqlite3_step == SQLITE_DONE` (inverted condition), and `list_containers_by_image` queries a non-existent `tag` column in `containers`[cite: 1]. | Eliminated by deprecating SQLite in favor of `StateStore`. |
| **`src/monitor.cpp`** (`Monitor`) | Thread-safe async logger is implemented[cite: 1] but never called anywhere in the codebase; code uses raw `std::cerr << "DEBUG: ..."`[cite: 1]. | Wire `Monitor::get_instance().log()` across all lifecycle modules and remove raw debug `std::cerr` noise. |
