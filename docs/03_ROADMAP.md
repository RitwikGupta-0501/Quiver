# Engineering Roadmap (v2.0)

## Phase 1: Build Modernization & Lock-Free State Store
- [ ] **Migrate Build System to CMake:** Replace the legacy `Makefile`[cite: 1] with a modern C++20 `CMakeLists.txt` supporting Ninja, `compile_commands.json`, AddressSanitizer (`-fsanitize=address,undefined`), and optional CUDA/NVML discovery.
- [ ] **Remove Legacy Bloated Modules:** Delete `src/database_manager.cpp`, `include/database_manager.hpp`, `src/package_manager.cpp`, and `include/package_manager.hpp`[cite: 1].
- [ ] **Implement Atomic JSON `StateStore`:** Build `StateStore` (`src/state_store.cpp`) using `nlohmann::json` to persist container, image, volume, and network state inside `~/.quiver/containers/<id>/state.json` and `~/.quiver/images/index.json`.

## Phase 2: Core Container Lifecycle & RAII Refactoring
- [ ] **Eliminate Static/Global State:** Refactor `Container`, `Terminal`, `Network`, and `CommandLineHandler`[cite: 1] into clean, non-static RAII classes driven by a `ContainerSpec` configuration struct.
- [ ] **Fix Rootless User Namespace & OverlayFS:**
  - Fix `setup_user_namespace()` to target `/proc/<child_pid>/uid_map` and `/proc/<child_pid>/gid_map` using dynamic host `getuid()` / `getgid()`.
  - Add `,userxattr` to `Mount::overlay()` so rootless OverlayFS works natively without requiring full rootfs copies.
  - Fix `Utils::extract_tarball()` (`-C` flag and `--no-same-owner`)[cite: 1].
- [ ] **Arbitrary Command Execution & OCI Config Support:** Support `quiver run -i <image> <cmd> [args...]` via `execvp()` and parse default `Env` and `Cmd` fields from registry image manifests.
- [ ] **Integrate Async `Monitor` Logging:** Route daemon and container lifecycle logs through `Monitor` (`src/monitor.cpp`)[cite: 1] into `~/.quiver/logs/<container_id>.log`.

## Phase 3: Cgroup v2 Hardware Resource Throttling
- [ ] **Implement `CgroupManager` (`src/cgroup.cpp`):**
  - Support rootless systemd user slice delegation and direct `/sys/fs/cgroup/` v2 hierarchy creation.
  - Implement `--memory <limit>` (`memory.max` and `memory.swap.max`).
  - Implement `--cpus <quota>` (`cpu.max`).
  - Implement `--pids-limit <max>` (`pids.max`) to prevent container fork bombs.
  - Automatically clean up cgroup directories on container exit.

## Phase 4: Native GPU Passthrough & NVML Telemetry
- [ ] **Implement `GpuDeviceManager` (`src/gpu_manager.cpp`):**
  - Add `--gpus <all | 0,1,...>` flag to `run` and `create`.
  - Bind-mount NVIDIA control nodes (`/dev/nvidiactl`, `/dev/nvidia-uvm`, `/dev/nvidia-uvm-tools`) and requested GPU character devices (`/dev/nvidia0`, etc.) from `/old_root/dev/` into the container's `/dev` prior to `umount2("/old_root", MNT_DETACH)`[cite: 1].
  - Discover and read-only bind-mount essential host driver libraries (`libcuda.so.1`, `libnvidia-ml.so.1`, `libnvidia-ptxjitcompiler.so.1`) into the container rootfs so containerized CUDA binaries can communicate with the host kernel driver.
- [ ] **Live Container & GPU Telemetry (`stats` subcommand):**
  - Dynamically load `libnvidia-ml.so.1` (`dlopen`) to query per-container GPU memory usage and GPU utilization alongside Cgroup v2 `memory.current` and `cpu.stat`.

## Phase 5: Verification & Benchmark Suite
- [ ] **Concurrency Stress Test:** Verify 50–100 concurrent rootless container spawns/teardowns with zero state corruption (benchmarking the lock-free JSON `StateStore` against the legacy SQLite implementation).
- [ ] **GPU Workload Proof:** Execute a containerized CUDA workload (e.g., `nvidia-smi` and a custom CUDA benchmark binary) inside a rootless container with `--gpus 0 --memory 2g --cpus 2.0`.
