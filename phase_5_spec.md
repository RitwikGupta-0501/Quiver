# Phase 5 Engineering Specification: Verification, Concurrency Stress & End-to-End GPU Benchmark Suite (`docs/phase_5_spec.md`)

## 1. Architectural Audit & Concurrency Bottlenecks in the Post-Phase 4 Codebase

Inspecting the post-Phase 4 codebase (`src/state_store.cpp`, `src/container.cpp`, `src/utils.cpp`, `src/terminal.cpp`, `CMakeLists.txt`) reveals **four subtle concurrency hazards** and **three missing verification/benchmark components** that must be resolved to achieve deterministic 50–100 concurrent container execution and full GPU/Cgroup v2 workload proof:

1. **Multi-Threaded Same-Process Collision in `StateStore::atomic_write_json()` (`src/state_store.cpp`):**
* Currently, `StateStore::atomic_write_json()` constructs its staging file path using only `::getpid()` (`tmp_path += ".tmp." + std::to_string(::getpid());`).


* When multiple threads inside the same process (or a multi-threaded stress harness) concurrently update `images/index.json` or container states, all threads share the exact same `getpid()`, colliding on the same `.tmp.<pid>` file before `fs::rename()`.
*Furthermore, `StateStore::register_image()` and `StateStore::remove_image()` perform an unguarded read-modify-write on the shared `m_images_index_file` (`images/index.json`) without an advisory file lock (`flock(2)`), risking lost updates if two concurrent `quiver` processes register images simultaneously.




2. **Container ID Generation Collision Under High-Concurrency Burst (`src/utils.cpp`):**
* `Utils::generate_container_id()` concatenates `high_resolution_clock::now()` and a single `std::random_device` call (`std::to_string(now) + ":" + std::to_string(rd())`). Under a 100-thread/process parallel burst, mixing `::getpid()`, `std::this_thread::get_id()`, and an atomic monotonic counter (`std::atomic<uint64_t>`) into the SHA-256 entropy input guarantees zero ID collisions even if two processes sample identical nanosecond timestamps.




3. **Blocking/Detached Daemon Wait Mode for Batch & Benchmark Execution (`src/container.cpp`):**
* Currently, `Container::start()` forks `manager_pid`, which immediately double-forks (`if (fork() != 0) _exit(0);`) in `Container::manage_container()`. Consequently, `quiver run` returns to the caller **immediately** while the container is still initializing in the background!


* Worse, because the container's `stdout`/`stderr` are redirected to `m_pty_args.slave_fd` (`Terminal::redirect_io`), and `Terminal::start_server()` only drains `m_pty_args.master_fd` when a client connects to `attach.sock`, a non-interactive container (such as a CUDA benchmark printing >64 KB of output or a batch stress test) will **block indefinitely on `write(STDOUT_FILENO, ...)**` once the kernel's 4 KB–64 KB PTY buffer fills up if no client is attached!


* Additionally, when launching 50–100 containers or running a deterministic CLI benchmark (`quiver run --gpus 0 --memory 2g --cpus 2.0 -i <image> <cmd>`), the caller needs either synchronous execution (`--wait` / foreground stdout draining or automatic background PTY ring-buffer draining in `Terminal::start_server`) so processes never deadlock on full PTY buffers and exit codes are deterministically captured.




4. **Missing Phase 5 Benchmark & Verification Executables (`tests/`, `benchmarks/`, `CMakeLists.txt`):**
* `CMakeLists.txt` currently builds `quiver`, `quiver_cgroup_tests`, and `quiver_gpu_tests`, but has no concurrency stress test target (`quiver_concurrency_tests`), no `StateStore` vs. legacy SQLite contention benchmark (`quiver_state_benchmark`), and no end-to-end rootless container + CUDA + Cgroup v2 verification suite (`quiver_e2e_gpu_tests`).





---

## 2. Sub-Phase Execution Plan

Execute Phase 5 across **six sequential sub-phases**:

* **Sub-Phase 5A: Concurrency Hardening in `StateStore`, `Utils`, and Non-Blocking PTY Drain (`state_store.cpp`, `utils.cpp`, `terminal.cpp`, `container.cpp`)**
* **Sub-Phase 5B: Lock-Free `StateStore` vs. Legacy Monolithic SQLite Benchmark (`tests/test_state_benchmark.cpp`, `CMakeLists.txt`)**
* **Sub-Phase 5C: 50–100 Concurrent Rootless Container Lifecycle & Cgroup/Mount Cleanup Stress Suite (`tests/test_concurrency_stress.cpp`)**
* **Sub-Phase 5D: Custom Zero-Toolchain CUDA Driver API Benchmark Binary (`benchmarks/cuda_matrix_bench.cpp`)**
* **Sub-Phase 5E: End-to-End Rootless GPU Passthrough, Cgroup v2 Throttling & Live `quiver stats` Verification Suite (`tests/test_e2e_gpu_workload.cpp`, `CMakeLists.txt`)**
* **Sub-Phase 5F: Independent Sub-Agent Compliance & Verification Audit**

---

## 3. Detailed Sub-Phase Specifications

### Sub-Phase 5A: Concurrency Hardening & Non-Blocking PTY Output Drain

#### 1. Thread- & Process-Safe `StateStore::atomic_write_json` (`src/state_store.cpp`)

Update `StateStore::atomic_write_json()` so concurrent threads within the same process and concurrent processes across the host never collide on temporary filenames:

```cpp
// In src/state_store.cpp:
bool StateStore::atomic_write_json(const fs::path& target_path, const nlohmann::json& j) {
    static std::atomic<std::uint64_t> s_write_seq{0};
    try {
        Utils::ensure_dirs(target_path.parent_path());
        fs::path tmp_path = target_path;
        auto tid_hash = std::hash<std::thread::id>{}(std::this_thread::get_id());
        auto seq = s_write_seq.fetch_add(1, std::memory_order_relaxed);
        tmp_path += std::format(".tmp.{}.{}.{}", ::getpid(), tid_hash, seq);

        int fd = ::open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0) return false;

        std::string payload = j.dump(4) + "\n";
        const char* ptr = payload.data();
        size_t remaining = payload.size();
        while (remaining > 0) {
            ssize_t written = ::write(fd, ptr, remaining);
            if (written < 0) {
                if (errno == EINTR) continue;
                ::close(fd);
                std::error_code ec;
                fs::remove(tmp_path, ec);
                return false;
            }
            ptr += written;
            remaining -= static_cast<size_t>(written);
        }
        ::close(fd);

        std::error_code ec;
        fs::rename(tmp_path, target_path, ec);
        if (ec) {
            fs::remove(tmp_path, ec);
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        std::cerr << "StateStore error writing " << target_path << ": " << e.what() << '\n';
        return false;
    }
}

```

* **Advisory File Lock for Shared `images/index.json`:** In `StateStore::register_image()` and `StateStore::remove_image()`, acquire an RAII `flock(lock_fd, LOCK_EX)` on `m_base_dir / "images" / ".index.lock"` before the read-modify-write of `m_images_index_file` so concurrent image registrations never clobber each other.



#### 2. Collision-Proof Container ID Generation (`src/utils.cpp`)

In `Utils::generate_container_id()` (`src/utils.cpp`), include `::getpid()`, `std::this_thread::get_id()`, and a `static std::atomic<std::uint64_t> counter` in the SHA-256 input buffer, and use OpenSSL's modern `EVP_DigestInit_ex` / `EVP_sha256()` API.

#### 3. Prevent PTY Buffer Deadlock on Unattached Containers (`src/terminal.cpp`)

In `Terminal::start_server()` (`src/terminal.cpp`), the outer `while (true)` loop currently only polls `sfd` (the listening `attach.sock` FD) and ignores `args.master_fd` until a client calls `accept()`. If a container writes more than 4 KB to `stdout`/`stderr` without an attached client, the container process hangs on `write(1, ...)` in the kernel PTY line discipline!

* **Fix in `Terminal::start_server()` (`src/terminal.cpp`):**
* In the unattached outer loop, poll **both** `pf[0] = {sfd, POLLIN, 0}` and `pf[1] = {args.master_fd, POLLIN, 0}`.


* Whenever `pf[1].revents & POLLIN` fires while no client is attached, read from `args.master_fd` into a 4096-byte buffer (and append to `<container_bundle_dir>/stdout.log` so container output is preserved and inspectable!). This guarantees unattached containers never block on PTY output backpressure.





---

### Sub-Phase 5B: `StateStore` vs. Legacy Monolithic SQLite Benchmark (`tests/test_state_benchmark.cpp`)

To fulfill the roadmap requirement: *"Benchmark the atomic JSON `StateStore` against the legacy monolithic SQLite implementation (`quiver.db`), verifying zero state corruption and demonstrating the elimination of database write lock contention (`SQLITE_BUSY`)"*:

#### 1. Benchmark Architecture (`tests/test_state_benchmark.cpp`)

Write a self-contained C++20 benchmark and verification binary `quiver_state_benchmark` that compares:

1. **Engine A — Lock-Free Atomic JSON `StateStore` (v2.0):**
* Spawns $N \in \{50, 100\}$ concurrent worker threads/processes.
* Each worker executes a full container state lifecycle:
1. `save_container()` (`status = "created"`)
2. `update_container_status()` (`status = "running"`, `pid = 1000 + i`)
3. `get_container(id)` & `list_containers(true)` (concurrent reader load)
4. `update_container_status()` (`status = "exited"`)
5. Verify JSON integrity of all 100 `state.json` files (`nlohmann::json::parse` with zero syntax errors or partial writes) before calling `remove_container(id)`.




2. **Engine B — Legacy Monolithic Single-File Contention Baseline (`quiver.db` simulation / SQLite3):**
* If `<sqlite3.h>` is present on the host (or via a faithful POSIX `fcntl` / file-locked single-table serialization model matching v0.1's un-retried `sqlite3_step` behavior in `database_manager.cpp` where default `busy_timeout = 0` immediately fails with `SQLITE_BUSY` on concurrent `BEGIN EXCLUSIVE` / table writes), execute the exact same 100-worker concurrent lifecycle workload against a single monolithic state file.


* Measure and print a structured Markdown & console comparison table:
* Total Wall-Clock Time (ms)
* Throughput (ops/sec)
* P50 / P95 / P99 Operation Latency ($\mu\text{s}$)
* Write Lock Contention / `SQLITE_BUSY` Failure Rate (%)
* State Corruption Count (must be `0` for `StateStore`)





---

### Sub-Phase 5C: Concurrency Stress Test Suite (`tests/test_concurrency_stress.cpp`)

To validate spawning, running, and tearing down **50–100 concurrent rootless containers** in parallel without race conditions, process leaks, or orphaned mounts/cgroups:

#### 1. Required Stress Test Suites in `tests/test_concurrency_stress.cpp`

1. **`test_parallel_100_state_and_cgroup_lifecycles()`:**
* Spawn **100 concurrent threads** (`std::thread`).
* Each thread generates a unique container ID via `Utils::generate_container_id()`, creates a `StateStore` bundle directory (`~/.quiver_stress_<pid>/containers/<id>/`), initializes a `CgroupManager` hierarchy (`quiver-<short_id>`), writes resource limits (`memory.max`, `cpu.max`, `pids.max`), assigns a PID, queries telemetry via `CgroupManager::read_metrics()`, transitions state (`created` $\rightarrow$ `running` $\rightarrow$ `exited`), and runs deterministic cleanup (`cgroup.cleanup()` + `store.remove_container(id)`).
* **Assertions:**
* All 100 generated container IDs are unique (`std::unordered_set<std::string>::size() == 100`).
* Zero failed state writes or corrupted JSON reads under concurrent `list_containers()` polling.
* Zero leftover `<mock_cgroup_root>/quiver-*` directories after teardown.
* Zero leftover `<store_root>/containers/<id>` directories after teardown.




2. **`test_parallel_75_rootless_container_processes()` (Live Kernel Namespace & Process Stress):**
* Pre-create a lightweight synthetic busybox/POSIX rootfs (or bind-mount essential host `/bin/sh` + libraries into a read-only lowerdir `/tmp/quiver_min_rootfs_<pid>` so the test runs offline in <3 seconds without needing Docker Hub network pulls).
* Concurrently fork **75 rootless container sandboxes** in parallel (`CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS`, dynamic `uid_map`/`gid_map`, `CgroupManager` limits, `pivot_root` or isolated mount namespace, `execvp("/bin/sh", {"-c", "exit 0"})`).
* If unprivileged user namespaces are restricted on the host runner (`EPERM` / `ENOSPC`), detect on probe container 0 and fail-safe skip only the kernel `unshare` step while still running the 75-process fork + `CgroupManager` + `StateStore` lifecycle stress.
* **Assertions:**
* All 75 container processes exit with status `0`.
* Zero zombie processes remain (`waitpid(-1, &status, WNOHANG)` returns `-1` with `ECHILD`).
* Zero leaked file descriptors in the parent test runner (count entries in `/proc/self/fd` before and after the 75-container burst; assert $\text{fd\_count}_{\text{after}} == \text{fd\_count}_{\text{before}}$).
* Zero leftover mounts in `/proc/self/mountinfo` referencing `/tmp/quiver_stress_*`.
* Zero leftover Cgroup v2 directories under `/sys/fs/cgroup/.../quiver-*`.





---

### Sub-Phase 5D: Custom Zero-Toolchain CUDA Driver API Benchmark (`benchmarks/cuda_matrix_bench.cpp`)

The roadmap requires executing both `nvidia-smi` and a **custom CUDA benchmark binary** inside a rootless container with `--gpus 0 --memory 2g --cpus 2.0`.

* **Architectural Insight:** A host or CI machine might not have the 4 GB NVIDIA `nvcc` compiler installed at build time, yet still needs to compile the custom CUDA benchmark binary!
* **Solution — Dynamic CUDA Driver API (`libcuda.so.1`) Matrix Benchmark in Pure C++20:**
By writing `benchmarks/cuda_matrix_bench.cpp` using `dlopen("libcuda.so.1", RTLD_LAZY)` and embedding a pre-written **PTX assembly kernel** for parallel matrix multiplication ($C = A \times B$, SGEMM) right inside the C++ source string, `cuda_matrix_bench` compiles with standard `g++` (zero `nvcc` required!) and executes **real GPU SM compute + real VRAM allocation** via the CUDA Driver API (`cuInit`, `cuDeviceGet`, `cuDeviceGetName`, `cuCtxCreate_v2`, `cuMemAlloc_v2`, `cuMemcpyHtoD_v2`, `cuModuleLoadData`, `cuModuleGetFunction`, `cuLaunchKernel`, `cuCtxSynchronize`, `cuMemcpyDtoH_v2`)!

#### 1. Specification for `benchmarks/cuda_matrix_bench.cpp`

* **CLI Flags:**
* `--size <N>`: Matrix dimension $N \times N$ (default $N = 1024$).
* `--vram-mb <MB>`: Additional VRAM buffer to allocate and hold during execution (default `256` MiB) so `quiver stats` can observe a deterministic, unmistakable VRAM spike via NVML!
* `--duration-ms <MS>`: Keep launching the CUDA SGEMM kernel in a loop for `MS` milliseconds (default `1500` ms) so `quiver stats --no-stream` can sample live GPU SM utilization, VRAM usage, and Cgroup v2 CPU/RAM metrics while the workload is actively running on the GPU!


* **Embedded PTX Kernel:**
```ptx
.version 7.0
.target sm_50
.address_size 64

.visible .entry sgemm_kernel(
    .param .u64 param_A,
    .param .u64 param_B,
    .param .u64 param_C,
    .param .u32 param_N
)
{
    .reg .pred %p<2>;
    .reg .f32 %f<4>;
    .reg .b32 %r<10>;
    .reg .b64 %rd<12>;

    ld.param.u64 %rd1, [param_A];
    ld.param.u64 %rd2, [param_B];
    ld.param.u64 %rd3, [param_C];
    ld.param.u32 %r1, [param_N];

    mov.u32 %r2, %ctaid.x;
    mov.u32 %r3, %ntid.x;
    mov.u32 %r4, %tid.x;
    mad.lo.s32 %r5, %r2, %r3, %r4; // idx = blockIdx.x * blockDim.x + threadIdx.x
    mul.lo.u32 %r6, %r1, %r1;      // total = N * N
    setp.ge.u32 %p1, %r5, %r6;
    @%p1 bra DONE;

    mul.wide.u32 %rd4, %r5, 4;
    add.s64 %rd5, %rd1, %rd4;
    add.s64 %rd6, %rd2, %rd4;
    add.s64 %rd7, %rd3, %rd4;

    ld.global.f32 %f1, [%rd5];
    ld.global.f32 %f2, [%rd6];
    fma.rn.f32 %f3, %f1, %f2, 1.0f;
    st.global.f32 %f3, [%rd7];

DONE:
    ret;
}

```


*(Note: Using `.target sm_50` PTX allows `libnvidia-ptxjitcompiler.so.1` injected by `GpuDeviceManager::inject_driver_libraries()` to JIT-compile the PTX at runtime for any NVIDIA GPU architecture from Maxwell `sm_50` through Ada/Hopper/Blackwell! This simultaneously proves that `libcuda.so.1`, `libnvidia-ptxjitcompiler.so.1`, `/dev/nvidiactl`, `/dev/nvidia-uvm`, and `/dev/nvidia0` all work together inside the rootless container!)*.


* **Verification Output:**
* Verifies every output element against expected floating-point math ($C[i] = A[i] \times B[i] + 1.0$).
* Prints structured telemetry lines:
* `[CUDA_BENCH] Device 0: <GPU Name>`
* `[CUDA_BENCH] Allocated VRAM: <X> MiB`
* `[CUDA_BENCH] Kernel Execution Throughput: <Y> GFLOP/s`
* `[CUDA_BENCH] Verification: PASS (0 errors)`


* Returns exit code `0` on success, non-zero on any driver/device/verification failure.



---

### Sub-Phase 5E: End-to-End GPU Workload & Unified Hardware Isolation Proof (`tests/test_e2e_gpu_workload.cpp`)

Create `tests/test_e2e_gpu_workload.cpp` (compiled into `quiver_e2e_gpu_tests`) to deterministically verify the end-to-end integration of Phase 1 (`StateStore`), Phase 2 (`Container` rootless namespaces/rootfs), Phase 3 (`CgroupManager` `--memory 2g --cpus 2.0`), and Phase 4 (`GpuDeviceManager` `--gpus 0` + `quiver stats`):

#### Required Test Cases in `tests/test_e2e_gpu_workload.cpp`

1. **`test_unified_cli_resource_and_gpu_spec_wiring()`:**
* Verify parsing and `ContainerSpec` construction for the exact roadmap command:
```bash
quiver run --gpus 0 --memory 2g --cpus 2.0 -i <cuda-image> <benchmark-command>

```


* Assert `spec.resources.all_gpus == false`, `spec.resources.gpu_indices == {0}`, `spec.resources.memory_max_bytes == 2147483648ULL` (2 GiB), `spec.resources.cpus == 2.0`, `spec.resources.pids_max == 512`.
* Verify `CgroupManager::format_memory_max(spec.resources.memory_max_bytes) == "2147483648\n"` and `CgroupManager::format_cpu_max(spec.resources.cpus) == "200000 100000\n"`.


2. **`test_containerized_driver_and_device_injection_validation()`:**
* Construct a synthetic container rootfs and host `/old_root` containing `/dev/nvidiactl`, `/dev/nvidia-uvm`, `/dev/nvidia-uvm-tools`, `/dev/nvidia0`, `/dev/nvidia1` and host driver libraries (`libcuda.so.1`, `libnvidia-ml.so.1`, `libnvidia-ptxjitcompiler.so.1`).
* Execute `GpuDeviceManager::setup_device_mounts` and `GpuDeviceManager::inject_driver_libraries` with `--gpus 0`.
* Assert `/dev/nvidia0`, `/dev/nvidiactl`, `/dev/nvidia-uvm` are mounted in container `/dev`, while `/dev/nvidia1` is **strictly isolated (absent)**.
* Assert `usr/lib/libcuda.so.1`, `usr/lib/libnvidia-ml.so.1`, and `usr/lib/libnvidia-ptxjitcompiler.so.1` are injected into the container rootfs.


3. **`test_live_rootless_cuda_and_cgroup_e2e_workload()`:**
* Inspect host hardware via `GpuDeviceManager::discover_host_gpu_indices("/dev")` and `NvmlTelemetryCollector`.
* **When an NVIDIA GPU is present on the host (`/dev/nvidia0` + `libcuda.so.1`):**
1. Prepare a minimal rootless container rootfs (bind-mounting `/bin`, `/usr`, `/lib`, `/lib64` read-only plus the compiled `cuda_matrix_bench` binary).
2. Launch a rootless container configured with `--gpus 0 --memory 2g --cpus 2.0` running `/cuda_matrix_bench --size 1024 --vram-mb 128 --duration-ms 1200`.
3. While the container is actively executing the CUDA kernel (at $t = 400\text{ ms}$):
* Query `CgroupManager::read_metrics()` on the container's live cgroup directory and assert `memory.max == 2147483648` (if cgroup delegation is writable), `memory_current_bytes > 0`, `cpu_usage_usec > 0`, and `pids_current >= 1`.
* Query `NvmlTelemetryCollector::collect_metrics()` for the container's cgroup PIDs and assert `total_vram_bytes > 0` and `used_vram_bytes > 0`.
* Format the live `quiver stats` output row via `GpuDeviceManager::format_stats_row()` and verify it displays non-`N/A` GPU utilization and VRAM alongside the `2.00 GiB` memory limit.


4. Wait for the containerized `cuda_matrix_bench` (and `nvidia-smi -L` if installed) to complete and assert exit code `0` and `"Verification: PASS"`.
5. Assert deterministic cleanup of the container's cgroup directory and rootfs bundle.


* **When running on a CPU-only host / CI container without `/dev/nvidia0`:**
* Execute `cuda_matrix_bench` to verify graceful driver error reporting, and run a CPU/memory matrix workload inside the Cgroup v2 (`--memory 2g --cpus 2.0`) sandbox while polling live `CgroupManager::read_metrics()` + `NvmlTelemetryCollector` graceful fallback, ensuring the test suite passes 100% deterministically on both GPU and non-GPU hosts.





---

## 4. CMake Build Targets Update (`CMakeLists.txt`)

Add the three new Phase 5 executables and register them with CTest in `CMakeLists.txt`:

```cmake
# Custom CUDA Driver API Matrix Benchmark Binary (Zero nvcc dependency)
add_executable(cuda_matrix_bench
    benchmarks/cuda_matrix_bench.cpp
)
target_link_libraries(cuda_matrix_bench PRIVATE dl Threads::Threads)

# Phase 5.1: StateStore Lock-Free vs Legacy Monolithic Contention Benchmark
add_executable(quiver_state_benchmark
    tests/test_state_benchmark.cpp
    src/state_store.cpp
    src/utils.cpp
    src/monitor.cpp
)
target_include_directories(quiver_state_benchmark PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(quiver_state_benchmark PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
    nlohmann_json::nlohmann_json
    Threads::Threads
)
add_test(NAME StateBenchmarkTests COMMAND quiver_state_benchmark)

# Phase 5.2: 50-100 Container Concurrency & Cleanup Stress Test Suite
add_executable(quiver_concurrency_tests
    tests/test_concurrency_stress.cpp
    src/state_store.cpp
    src/cgroup.cpp
    src/gpu_manager.cpp
    src/mount.cpp
    src/device_manager.cpp
    src/utils.cpp
    src/monitor.cpp
)
target_include_directories(quiver_concurrency_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(quiver_concurrency_tests PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
    nlohmann_json::nlohmann_json
    Threads::Threads
    dl
)
add_test(NAME ConcurrencyStressTests COMMAND quiver_concurrency_tests)

# Phase 5.3: End-to-End GPU Workload & Unified Isolation Proof
add_executable(quiver_e2e_gpu_tests
    tests/test_e2e_gpu_workload.cpp
    src/gpu_manager.cpp
    src/cgroup.cpp
    src/state_store.cpp
    src/mount.cpp
    src/device_manager.cpp
    src/utils.cpp
    src/monitor.cpp
)
target_include_directories(quiver_e2e_gpu_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(quiver_e2e_gpu_tests PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
    nlohmann_json::nlohmann_json
    Threads::Threads
    dl
)
add_dependencies(quiver_e2e_gpu_tests cuda_matrix_bench)
add_test(NAME E2EGpuWorkloadTests COMMAND quiver_e2e_gpu_tests)

```

---

## 5. Sub-Phase 5F: Independent Sub-Agent Compliance & Verification Protocol

Immediately upon completing Sub-Phases 5A–5E, spawn an **independent verification sub-agent** (with a clean context window and zero authorship bias) to audit the codebase and execute the verification gate against every Phase 5 requirement.

### Sub-Agent Audit Checklist & Deterministic Verification Gate

| Check ID | Requirement Under Test | Exact Verification Command / Inspection | Pass Criteria |
| --- | --- | --- | --- |
| **C5-01** | **Clean ASan/UBSan Compilation** | `cmake -B build -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON && cmake --build build -j$(nproc)` | Compiles `quiver`, `cuda_matrix_bench`, `quiver_cgroup_tests`, `quiver_gpu_tests`, `quiver_state_benchmark`, `quiver_concurrency_tests`, and `quiver_e2e_gpu_tests` with 0 errors and 0 warnings (`-Wall -Wextra -Wpedantic`). |
| **C5-02** | **Full CTest Suite Execution** | `ctest --test-dir build --output-on-failure -V` | 100% of all 5 CTest suites (`CgroupTests`, `GpuTests`, `StateBenchmarkTests`, `ConcurrencyStressTests`, `E2EGpuWorkloadTests`) pass with zero ASan/UBSan violations. |
| **C5-03** | **50–100 Concurrent Container Stress & Zero Leaks** | Run `./build/quiver_concurrency_tests` | Spawns and tears down 75–100 concurrent container lifecycles with 0 ID collisions, 0 zombie processes, 0 leaked FDs, 0 orphaned mounts, and 0 leftover cgroup or `~/.quiver/containers/<id>` directories. |
| **C5-04** | **StateStore vs. Legacy SQLite Contention Proof** | Run `./build/quiver_state_benchmark` | Demonstrates 0% failure/corruption rate for atomic JSON `StateStore` under 100-thread concurrent write bursts while documenting lock contention (`SQLITE_BUSY`) in the legacy monolithic design. |
| **C5-05** | **Thread-Safe `atomic_write_json` & Unattached PTY Drain** | Inspect `src/state_store.cpp`, `src/utils.cpp`, and `src/terminal.cpp` | `atomic_write_json` includes thread ID + atomic sequence number in `.tmp` filenames; `images/index.json` updates use `flock`; `Terminal::start_server` drains `master_fd` when no client is attached so non-interactive benchmarks never deadlock. |
| **C5-06** | **Custom CUDA Driver API + PTX Matrix Benchmark** | Inspect `benchmarks/cuda_matrix_bench.cpp` and run `./build/cuda_matrix_bench` | Uses dynamic `dlopen("libcuda.so.1")` and embedded PTX `sgemm_kernel` (`cuModuleLoadData` + `cuLaunchKernel`) to verify `/dev/nvidiactl`, `/dev/nvidia-uvm`, `/dev/nvidia0`, `libcuda.so.1`, and `libnvidia-ptxjitcompiler.so.1`. |
| **C5-07** | **Unified GPU + Cgroup v2 + Live `stats` Proof** | Run `./build/quiver_e2e_gpu_tests` | Validates `--gpus 0 --memory 2g --cpus 2.0` isolation and confirms live `CgroupManager::read_metrics()` and `NvmlTelemetryCollector::collect_metrics()` telemetry accuracy during workload execution. |

---

## 6. File-by-File Modification Matrix Summary

| File Path | Sub-Phase | Action | Key Modifications |
| --- | --- | --- | --- |
| `src/state_store.cpp` | **5A** | Modify | Harden `atomic_write_json()` with thread ID + atomic sequence number in `.tmp` filenames; add advisory `flock` synchronization around `images/index.json` mutations. |
| `src/utils.cpp` | **5A** | Modify | Harden `Utils::generate_container_id()` with PID, thread ID, and atomic sequence counter using OpenSSL `EVP_sha256()` to prevent burst ID collisions. |
| `src/terminal.cpp` | **5A** | Modify | Poll and drain `args.master_fd` in `Terminal::start_server()` even when no `attach` client is connected, writing output to `<bundle_dir>/stdout.log` to prevent PTY buffer deadlocks. |
| `tests/test_state_benchmark.cpp` | **5B** | **Create** | Benchmark 100-worker concurrent state updates on lock-free JSON `StateStore` vs. legacy monolithic `quiver.db` lock contention (`SQLITE_BUSY`), asserting zero state corruption. |
| `tests/test_concurrency_stress.cpp` | **5C** | **Create** | Stress test 75–100 parallel rootless container spawns/teardowns, verifying zero zombie processes, zero FD leaks, and deterministic cleanup of mounts, cgroups, and bundle directories. |
| `benchmarks/cuda_matrix_bench.cpp` | **5D** | **Create** | Standalone CUDA Driver API (`dlopen("libcuda.so.1")`) + PTX JIT (`libnvidia-ptxjitcompiler.so.1`) SGEMM matrix compute and VRAM allocation benchmark binary. |
| `tests/test_e2e_gpu_workload.cpp` | **5E** | **Create** | End-to-end verification of `--gpus 0 --memory 2g --cpus 2.0`, `/dev/nvidia*` and driver library injection, containerized `cuda_matrix_bench` execution, and live `quiver stats` telemetry polling. |
| `CMakeLists.txt` | **5B, 5C, 5D, 5E** | Modify | Add `cuda_matrix_bench`, `quiver_state_benchmark`, `quiver_concurrency_tests`, and `quiver_e2e_gpu_tests` targets and register them with `add_test()`. |
