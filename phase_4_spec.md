# Phase 4 Engineering Specification: Native Rootless GPU Passthrough & NVML Telemetry (`docs/phase_4_spec.md`)

## 1. Architectural Overview & Current Baseline Gaps

Inspecting the post-Phase 3 codebase reveals that while `ResourceLimits` (`include/state_store.hpp`) already defines `std::vector<int> gpu_indices{}` and `bool all_gpus{false}`, the actual GPU device passthrough, user-space driver injection, and NVML telemetry subsystems are completely absent:

1. **Missing `GpuDeviceManager` Subsystem (`include/gpu_manager.hpp`, `src/gpu_manager.cpp`):**
* Neither header nor implementation exists, and `CMakeLists.txt` does not compile any GPU management module.


* Currently, `DeviceManager::create_terminal_devices()` (`src/device_manager.cpp`) only bind-mounts virtual character devices (`null`, `zero`, `full`, `random`, `urandom`, `tty`, `console`) from `/old_root/dev/` into `/dev/`. Containerized CUDA binaries fail immediately with `cudaErrorNoDevice` or `cudaErrorInsufficientDriver`.




2. **Missing CLI `--gpus` Flag & `create` / `stats` Subcommand Wiring:**
* `CommandLineHandler::run()` (`src/command_line_handler.cpp`) rejects `--gpus` as `"Unknown argument"`.


* `CommandLineHandler::create()` currently only handles `quiver create volume <cid> <host:cont>` and does not support `quiver create [options] -i <image> [cmd]` (or `quiver create gpu <cid> <all|0,1,...>`), nor is there a `stats` command in `src/main.cpp` or `include/command_line_handler.hpp`.


* `Utils::print_usage()` (`src/utils.cpp`) omits both `--gpus` and `quiver stats`.




3. **Unprivileged User Namespace Mount Ordering & Read-Only Driver Injection Constraints:**
* In Linux unprivileged user namespaces (`CLONE_NEWUSER | CLONE_NEWNS`), `mknod(2)` on character devices is blocked by the kernel (`EPERM`). Passing through `/dev/nvidia*` requires creating empty regular file mount targets inside the new `/dev` (`tmpfs`) and bind-mounting (`MS_BIND`) the character devices from `/old_root/dev/` **before** `umount2("/old_root", MNT_DETACH)` executes in `Container::run_container()` (`src/container.cpp`).


* Similarly, host NVIDIA user-space driver libraries (`libcuda.so.1`, `libnvidia-ml.so.1`, `libnvidia-ptxjitcompiler.so.1`, etc.) reside on the host filesystem (accessible at `/old_root/usr/lib/...` or `/old_root/lib/...` after `pivot_root`). In Linux user namespaces, remounting a bind mount as read-only requires preserving existing mount flags (`MS_BIND | MS_REMOUNT | MS_RDONLY | MS_NOSUID | MS_NODEV`) via `statvfs(2)` so the kernel does not reject the remount with `EPERM`.




4. **Missing Cgroup v2 Live Telemetry Readers & Dynamic NVML Loader (`dlopen`):**
* `CgroupManager` (`include/cgroup.hpp`, `src/cgroup.cpp`) currently implements limit *writers* (`memory.max`, `cpu.max`, `pids.max`), but has no *readers* for live runtime telemetry (`memory.current`, `cpu.stat`, `pids.current`, `cgroup.procs`).


* `quiver` already links against `dl` in `CMakeLists.txt`, but lacks a zero-header dynamic `dlopen("libnvidia-ml.so.1", RTLD_LAZY)` wrapper to query GPU VRAM, SM utilization, temperature, and per-process GPU memory without requiring the NVIDIA CUDA Toolkit at compile time.





---

## 2. Sub-Phase Execution Plan

To keep implementation modular and verifiable, execute Phase 4 across **six sequential sub-phases**:

* **Sub-Phase 4A: GPU String Parser, CLI Flag Wiring (`--gpus`, `create`, `stats`), and Environment Propagation (`utils`, `command_line_handler`, `main`)**

* **Sub-Phase 4B: `GpuDeviceManager` — Rootless Character Device Resolution & Bind-Mounting (`gpu_manager.hpp`, `gpu_manager.cpp`, `container.cpp`)**

* **Sub-Phase 4C: Host Driver User-Space Library Discovery & Read-Only Rootfs Injection (`gpu_manager.cpp`, `container.cpp`)**

* **Sub-Phase 4D: Cgroup v2 Telemetry Readers, Dynamic NVML Loader (`dlopen`), and `quiver stats` Dashboard (`cgroup`, `gpu_manager`, `command_line_handler`)**

* **Sub-Phase 4E: Unit & Mock Integration Test Suite (`tests/test_gpu.cpp`, `CMakeLists.txt`)**

* **Sub-Phase 4F: Independent Sub-Agent Compliance & Verification Audit**

---

## 3. Detailed Sub-Phase Specifications

### Sub-Phase 4A: GPU String Parser, CLI Flag Wiring & Environment Propagation

#### 1. Add `Utils::parse_gpus_string` (`include/utils.hpp`, `src/utils.cpp`)

Add a deterministic parser for the `--gpus <all | 0,1,...>` CLI argument:

```cpp
// Inside include/utils.hpp (namespace Utils):
struct GpuRequest {
    bool all_gpus{false};
    std::vector<int> gpu_indices{};
};

auto parse_gpus_string(std::string_view str) -> GpuRequest;

```

* **`Utils::parse_gpus_string(std::string_view str)` Contract:**
* Trim leading and trailing whitespace. If empty, throw `std::invalid_argument("Empty --gpus specification")`.
* If case-insensitive match for `"all"`: return `GpuRequest{.all_gpus = true, .gpu_indices = {}}`.
* Otherwise, split by comma (`,`). Reject empty tokens (e.g., `"0,,1"`, `",0"`, `"0,"`).
* For each token, trim whitespace, reject negative signs (`-`) or non-digit characters, and parse as non-negative `int` via `std::stoi`.
* Deduplicate and sort `gpu_indices` in ascending order (e.g., `"1,0,1"` $\rightarrow$ `{0, 1}`).
* Return `GpuRequest{.all_gpus = false, .gpu_indices = ...}`.



#### 2. Wire `--gpus` and `stats` into CLI (`src/main.cpp`, `include/command_line_handler.hpp`, `src/command_line_handler.cpp`, `src/utils.cpp`)

* **`src/main.cpp`:** Register `commands["stats"] = [&handler](const std::vector<std::string>& args) { handler.stats(args); };`.


* **`include/command_line_handler.hpp`:** Add `void stats(const std::vector<std::string>& args);` to `CommandLineHandler`.


* **`CommandLineHandler::run()` (`src/command_line_handler.cpp`):** Add `--gpus` flag handling inside the option loop:


```cpp
} else if (arg == "--gpus") {
    if (i + 1 < cmds.size()) {
        try {
            auto req = Utils::parse_gpus_string(cmds[++i]);
            spec.resources.all_gpus = req.all_gpus;
            spec.resources.gpu_indices = std::move(req.gpu_indices);
        } catch (const std::exception& e) {
            std::cerr << "Error: Invalid --gpus value: " << e.what() << '\n';
            return;
        }
    } else {
        std::cerr << "Error: --gpus requires an argument (e.g., all or 0,1)\n";
        return;
    }

```


* **`CommandLineHandler::create()` (`src/command_line_handler.cpp`):**
Support both the existing `quiver create volume <cid> <host:cont>` **and** container creation / GPU attachment so `--gpus` works with `create` as required by the roadmap (`Add --gpus <all | 0,1,...> flag to run and create`):


1. If `cmds[0] == "volume"`: preserve existing volume creation behavior.


2. If `cmds[0] == "gpu"` (e.g., `quiver create gpu <container_id> <all|0,1,...>`): parse `cmds[2]` via `Utils::parse_gpus_string()`, update `cs.resources.all_gpus` and `cs.resources.gpu_indices` in `StateStore`, and save.
3. Otherwise (when flags like `-i <image>` and `--gpus <...>` are passed to `quiver create [options] -i <image> [cmd]`): parse the exact same container flags as `run` (`-i`, `-n`, `-p`, `-v`, `-m`, `--cpus`, `--pids-limit`, `--gpus`, `--vfs`, `--no-remove`), pull the image if needed, persist `ContainerState` with `status = "created"` in `StateStore`, and print the created container ID without starting it (so `quiver start <id>` can launch it later).




* **`Utils::print_usage()` (`src/utils.cpp`):** Document `--gpus <all|0,1,...>` under `run` and `create`, and document `stats [--no-stream] [container_id]` in the main command list.



---

### Sub-Phase 4B: `GpuDeviceManager` — Rootless Character Device Passthrough

#### 1. Class & Data Structures (`include/gpu_manager.hpp`)

Create `include/gpu_manager.hpp` to encapsulate rootless device node discovery, character device bind-mounting, user-space library discovery/injection, and dynamic NVML telemetry:

```cpp
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>
#include <sys/types.h>
#include "state_store.hpp"

namespace fs = std::filesystem;

struct GpuProcessSample {
    unsigned int pid{0};
    std::uint64_t used_vram_bytes{0};
};

struct GpuDeviceTelemetry {
    int index{0};
    std::string name{};
    std::uint64_t total_vram_bytes{0};
    std::uint64_t used_vram_bytes{0};
    unsigned int gpu_utilization_pct{0};
    unsigned int mem_utilization_pct{0};
    unsigned int temperature_c{0};
    std::vector<GpuProcessSample> processes{};
};

class GpuDeviceManager {
public:
    static constexpr const char* CONTROL_DEVICES[] = {
        "nvidiactl",
        "nvidia-uvm",
        "nvidia-uvm-tools",
        "nvidia-modeset"
    };

    static constexpr const char* ESSENTIAL_DRIVER_LIBS[] = {
        "libcuda.so.1",
        "libnvidia-ml.so.1",
        "libnvidia-ptxjitcompiler.so.1",
        "libnvidia-fatbinaryloader.so",
        "libcudadebugger.so.1"
    };

    // 1. Check if GPU passthrough is requested in ResourceLimits
    [[nodiscard]] static bool is_gpu_requested(const ResourceLimits& limits) noexcept;

    // 2. Discover available /dev/nvidia[0-9]+ indices under dev_root (e.g., "/old_root/dev" or "/dev")
    [[nodiscard]] static std::vector<int> discover_host_gpu_indices(
        const fs::path& dev_root = "/dev"
    );

    // 3. Resolve exact device relative paths (e.g., "nvidiactl", "nvidia0") for a given ResourceLimits
    [[nodiscard]] static bool resolve_device_nodes(
        const ResourceLimits& limits,
        const fs::path& dev_root,
        std::vector<std::string>& out_device_names,
        std::string& out_error
    );

    // 4. Bind-mount resolved character devices from old_dev_root into new_dev_root
    static bool setup_device_mounts(
        const ResourceLimits& limits,
        const fs::path& old_dev_root,
        const fs::path& new_dev_root,
        std::string& out_error,
        bool dry_run = false
    );

    // 5. Discover essential host driver libraries under host_root
    [[nodiscard]] static std::vector<fs::path> discover_driver_libraries(
        const fs::path& host_root = "/old_root"
    );

    // 6. Read-only bind-mount discovered libraries into container rootfs (/usr/lib64 or /usr/lib)
    static bool inject_driver_libraries(
        const fs::path& host_root,
        const fs::path& container_root,
        std::string& out_error,
        bool dry_run = false
    );
};

```

#### 2. Device Discovery & Resolution Logic (`src/gpu_manager.cpp`)

* **`is_gpu_requested(const ResourceLimits& limits)`:**
* Returns `limits.all_gpus || !limits.gpu_indices.empty()`.


* **`discover_host_gpu_indices(const fs::path& dev_root)`:**
* Iterate over `dev_root` (`fs::directory_iterator(dev_root, ec)`).
* Match entries whose filename starts with `"nvidia"` followed strictly by one or more ASCII digits (`0`–`9`), e.g., `nvidia0`, `nvidia1` (ignoring `nvidiactl`, `nvidia-uvm`, `nvidia-caps`, etc.).
* Parse the integer suffix, sort ascending, and return the vector of discovered indices.


* **`resolve_device_nodes(const ResourceLimits& limits, const fs::path& dev_root, std::vector<std::string>& out_device_names, std::string& out_error)`:**
* If `!is_gpu_requested(limits)`, return `true` with `out_device_names` empty.
* Determine target GPU indices:
* If `limits.all_gpus`: call `discover_host_gpu_indices(dev_root)`. If empty, set `out_error = "No NVIDIA GPU devices (/dev/nvidia[0-9]+) found in " + dev_root.string()` and return `false`.
* Else: use `limits.gpu_indices`. Verify each requested `/dev/nvidia<idx>` exists in `dev_root` (`fs::exists(dev_root / ("nvidia" + std::to_string(idx)), ec)`) and is readable (`::access(..., R_OK) == 0`). If missing or inaccessible, populate a descriptive `out_error` and return `false`.


* Verify required control nodes:
* `/dev/nvidiactl` is **mandatory** when GPUs are requested. If `!fs::exists(dev_root / "nvidiactl")`, set `out_error = "Mandatory control device /dev/nvidiactl not found"` and return `false`.
* Check optional/companion control nodes (`nvidia-uvm`, `nvidia-uvm-tools`, `nvidia-modeset`); include every node that exists in `dev_root`.


* Populate `out_device_names` with the existing control nodes followed by `"nvidia" + std::to_string(idx)` for each target index.



#### 3. Rootless Character Device Bind-Mounting (`setup_device_mounts`)

* For each device name `dev_name` returned by `resolve_device_nodes`:
* `fs::path src = old_dev_root / dev_name;` (e.g., `/old_root/dev/nvidia0`)
* `fs::path dst = new_dev_root / dev_name;` (e.g., `/dev/nvidia0`)
* Create the mount target file in `new_dev_root`:
```cpp
int fd = ::open(dst.c_str(), O_CREAT | O_RDONLY, 0666);
if (fd < 0) {
    out_error = std::format("Failed to create device mount target {}: {}", dst.string(), std::strerror(errno));
    return false;
}
::close(fd);

```


* If `!dry_run`: call `::mount(src.c_str(), dst.c_str(), nullptr, MS_BIND, nullptr)`. If `mount` returns `-1`, set `out_error` with `std::strerror(errno)` and return `false`.


* Also check if `old_dev_root / "nvidia-caps"` directory exists; if so, `Utils::ensure_dirs(new_dev_root / "nvidia-caps")` and bind-mount any existing capability nodes (`MS_BIND | MS_REC`).

---

### Sub-Phase 4C: User-Space Driver Library Discovery & Read-Only Injection

#### 1. Host Driver Library Discovery (`discover_driver_libraries`)

Containerized CUDA applications link against the CUDA Runtime (`libcudart.so`), which dynamically `dlopen`s the user-space driver (`libcuda.so.1`) and PTX JIT compiler (`libnvidia-ptxjitcompiler.so.1`). Because these user-space libraries communicate via `ioctl` with the host's `nvidia.ko` kernel module, they must match the exact host driver version.

* Search the standard multi-arch and distribution library directories under `host_root` (`/old_root`):
* `/usr/lib/x86_64-linux-gnu`
* `/usr/lib/aarch64-linux-gnu`
* `/usr/lib64`
* `/usr/lib`
* `/lib/x86_64-linux-gnu`
* `/lib/aarch64-linux-gnu`
* `/lib64`
* `/lib`
* `/usr/lib/wsl/lib` *(for WSL2 NVIDIA GPU passthrough compatibility)*


* In each existing directory, search for files matching any prefix in `ESSENTIAL_DRIVER_LIBS` (`libcuda.so.1`, `libnvidia-ml.so.1`, `libnvidia-ptxjitcompiler.so.1`, `libnvidia-fatbinaryloader.so`, `libcudadebugger.so.1`).
* Resolve symlinks within `host_root` carefully:
* **Crucial `pivot_root` Symlink Rule:** If `/old_root/usr/lib/x86_64-linux-gnu/libcuda.so.1` is a relative symlink (e.g., `libcuda.so.550.54.14`), `fs::canonical` on the host might resolve outside `/old_root` if the target is an absolute symlink starting with `/`. Therefore, resolve relative symlinks relative to `entry.path().parent_path()`, and if a symlink target is absolute (`target.is_absolute()`), prefix it with `host_root` (`host_root / target.relative_path()`) so the source path remains strictly inside `/old_root`!


* Deduplicate by library filename (`libcuda.so.1`, `libnvidia-ml.so.1`, `libnvidia-ptxjitcompiler.so.1`, etc.) so each essential library is mounted once from the first matching host directory.

#### 2. Read-Only Bind-Mounting & Symlink Creation (`inject_driver_libraries`)

* Target directory inside container rootfs: `container_root / "usr/lib"` (or `/usr/lib/x86_64-linux-gnu` if present, plus `/usr/lib` to ensure standard dynamic linker visibility; specifically, mounting into `/usr/lib` inside the container and adding `/usr/lib` to `LD_LIBRARY_PATH` guarantees every distro—Ubuntu, Debian, Alpine/glibc, Rocky—finds the injected libraries immediately).
* For each discovered library `src_path` (e.g., `/old_root/usr/lib/x86_64-linux-gnu/libcuda.so.550.54.14` found via `libcuda.so.1`):
1. Let `soname` be the canonical SONAME (e.g., `libcuda.so.1`, `libnvidia-ml.so.1`, `libnvidia-ptxjitcompiler.so.1`).
2. Create destination file `dst_path = container_root / "usr/lib" / soname` (`::open(dst_path.c_str(), O_CREAT | O_WRONLY, 0755)`).
3. If `!dry_run`:
* Perform initial bind mount: `::mount(src_path.c_str(), dst_path.c_str(), nullptr, MS_BIND, nullptr)`.
* **Remount Read-Only safely in Rootless User Namespace:**
Query existing mount flags on `dst_path` using `struct statvfs vfs{}; ::statvfs(dst_path.c_str(), &vfs);` and convert `ST_NOSUID`, `ST_NODEV`, `ST_NOEXEC`, `ST_NOATIME`, `ST_RELATIME` into `MS_*` flags, then call:
```cpp
::mount(nullptr, dst_path.c_str(), nullptr, MS_BIND | MS_REMOUNT | MS_RDONLY | existing_ms_flags, nullptr);

```




4. Create convenience linker symlinks in `container_root / "usr/lib"`:
* If `soname == "libcuda.so.1"`, create symlink `libcuda.so -> libcuda.so.1` (if not already present).
* If `soname == "libnvidia-ml.so.1"`, create symlink `libnvidia-ml.so -> libnvidia-ml.so.1` (if not already present).





#### 3. Hook into `Container::run_container()` (`src/container.cpp`)

In `Container::run_container()` (`src/container.cpp`), right after `DeviceManager::create_terminal_devices();` and **strictly before** `umount2("/old_root", MNT_DETACH);`:

```cpp
DeviceManager::create_terminal_devices();

if (GpuDeviceManager::is_gpu_requested(m_spec.resources)) {
    std::string gpu_err;
    if (!GpuDeviceManager::setup_device_mounts(m_spec.resources, "/old_root/dev", "/dev", gpu_err)) {
        Utils::handle_error("GPU device passthrough failed: " + gpu_err);
    }
    if (!GpuDeviceManager::inject_driver_libraries("/old_root", "/", gpu_err)) {
        std::cerr << "WARNING: GPU driver library injection incomplete: " << gpu_err << '\n';
    }
}

umount2("/old_root", MNT_DETACH);
rmdir("/old_root");

```

And in the environment setup block of `Container::run_container()`, when `GpuDeviceManager::is_gpu_requested(m_spec.resources)` is `true`:

* Set `NVIDIA_VISIBLE_DEVICES` (`"all"` or `"0,1,..."`) and `NVIDIA_DRIVER_CAPABILITIES="compute,utility"`.
* Ensure `LD_LIBRARY_PATH` includes `/usr/lib:/usr/lib64:/usr/local/nvidia/lib:/usr/local/nvidia/lib64` (appending to any existing `LD_LIBRARY_PATH` from `m_spec.env` so image CUDA toolkit paths like `/usr/local/cuda/lib64` are preserved!).

---

### Sub-Phase 4D: Dynamic NVML Loader (`dlopen`), Cgroup v2 Telemetry & `quiver stats`

#### 1. Add Cgroup v2 Live Telemetry Readers (`include/cgroup.hpp`, `src/cgroup.cpp`)

Extend `CgroupManager` in `include/cgroup.hpp` and `src/cgroup.cpp` with static telemetry readers that work on both real `/sys/fs/cgroup` paths and mock test directories:

```cpp
// Add to include/cgroup.hpp:
struct CgroupMetrics {
    std::uint64_t memory_current_bytes{0};
    std::uint64_t memory_max_bytes{0}; // 0 = unlimited ("max")
    std::uint64_t cpu_usage_usec{0};
    std::uint64_t cpu_throttled_usec{0};
    std::uint64_t pids_current{0};
    std::uint64_t pids_max{0};         // 0 = unlimited ("max")
    std::vector<pid_t> pids{};
};

// Static telemetry helpers inside CgroupManager:
[[nodiscard]] static std::optional<fs::path> find_container_cgroup_dir(
    std::string_view container_id,
    pid_t container_pid = -1,
    const fs::path& cgroup_root = "/sys/fs/cgroup"
);

[[nodiscard]] static bool read_metrics(
    const fs::path& cgroup_dir,
    CgroupMetrics& out_metrics
);

```

* **`find_container_cgroup_dir` Contract:**
1. If `container_pid > 0` and `/proc/<container_pid>/cgroup` exists, read the `0::<rel_path>` line and check if `cgroup_root / rel_path` exists.
2. Otherwise, search `cgroup_root` (e.g., `cgroup_root / ("quiver-" + short_id)`, `cgroup_root / "quiver" / ("quiver-" + short_id)`, or recursive search under `/sys/fs/cgroup/user.slice` for directory named `"quiver-" + container_id.substr(0, 12)`).


* **`read_metrics(const fs::path& cgroup_dir, CgroupMetrics& out_metrics)` Contract:**
* Read `memory.current` $\rightarrow$ `out_metrics.memory_current_bytes`.
* Read `memory.max` $\rightarrow$ if `"max"`, `0`; else parse integer bytes.
* Read `cpu.stat` line-by-line $\rightarrow$ extract key-value pairs `usage_usec <val>` and `throttled_usec <val>`.
* Read `pids.current` and `pids.max` (`"max"` $\rightarrow$ `0`).
* Read `cgroup.procs` $\rightarrow$ collect all active host PIDs inside the container's cgroup into `out_metrics.pids` (used both for PID count verification and for cross-referencing against NVML GPU compute processes!).



#### 2. Dynamic NVML Wrapper via `dlopen` (`include/gpu_manager.hpp`, `src/gpu_manager.cpp`)

To guarantee `quiver` builds on any Linux machine without requiring `<nvml.h>` or `-lnvidia-ml` at build time, define minimal C ABI types and function pointer signatures inside `GpuDeviceManager` / `NvmlLoader`:

```cpp
// Minimal ABI-compatible NVML types (from NVIDIA NVML C ABI):
using nvmlReturn_t = int;
constexpr nvmlReturn_t NVML_SUCCESS = 0;
constexpr nvmlReturn_t NVML_ERROR_INSUFFICIENT_SIZE = 7;

using nvmlDevice_t = void*;
constexpr int NVML_TEMPERATURE_GPU = 0;

struct nvmlMemory_t {
    unsigned long long total;
    unsigned long long free;
    unsigned long long used;
};

struct nvmlUtilization_t {
    unsigned int gpu;
    unsigned int memory;
};

struct nvmlProcessInfo_v1_t {
    unsigned int pid;
    unsigned long long usedGpuMemory;
};

struct nvmlProcessInfo_v2_t {
    unsigned int pid;
    unsigned long long usedGpuMemory;
    unsigned int gpuInstanceId;
    unsigned int computeInstanceId;
};

```

* **`NvmlTelemetryCollector` (RAII `dlopen` Wrapper in `include/gpu_manager.hpp` & `src/gpu_manager.cpp`):**
* Constructor attempts `dlopen("libnvidia-ml.so.1", RTLD_LAZY)` (falling back to `"libnvidia-ml.so"` and `"/usr/lib/wsl/lib/libnvidia-ml.so.1"`).
* If `dlopen` returns `nullptr`, `is_available()` is `false` and `quiver stats` gracefully renders `"N/A (no driver)"` for GPU columns without failing.


* Resolves symbols via `dlsym`:
* `nvmlInit_v2` (fallback `nvmlInit`)
* `nvmlShutdown`
* `nvmlDeviceGetCount_v2` (fallback `nvmlDeviceGetCount`)
* `nvmlDeviceGetHandleByIndex_v2` (fallback `nvmlDeviceGetHandleByIndex`)
* `nvmlDeviceGetName`
* `nvmlDeviceGetMemoryInfo`
* `nvmlDeviceGetUtilizationRates`
* `nvmlDeviceGetTemperature`
* `nvmlDeviceGetComputeRunningProcesses` (or `_v2` / `_v3`)


* Destructor calls `nvmlShutdown()` and `dlclose(m_handle)`.
* **Per-Container GPU Attribution:**
* For a given container, `CgroupMetrics::pids` (plus `state.pid` and its descendants in `/proc`) provides the set of host PIDs belonging to the container.
* When querying each GPU assigned to the container (`limits.all_gpus` or `limits.gpu_indices`), sum `usedGpuMemory` for processes matching the container's PIDs (or report device-level VRAM & SM utilization alongside container-specific VRAM when active processes are matched).







#### 3. `quiver stats` Subcommand & Terminal Formatter (`src/command_line_handler.cpp`)

Implement `CommandLineHandler::stats(const std::vector<std::string>& cmds)`:

* **Supported Flags & Arguments:**
* `--no-stream` (or `--once`): Capture a single telemetry snapshot ( computing CPU % over a short 100ms sample interval) and exit immediately. *(Default if stdout is not a TTY or when `--no-stream` is passed; otherwise stream live updates every `1000ms` using ANSI clear-screen `\033[2J\033[H` until `SIGINT`/`Ctrl+C`)*.
* Optional `[container_id ...]`: Filter stats to specific running container(s) by ID or name. If omitted, display all running containers (`m_store.list_containers(true)`).


* **CPU % Calculation Across Two Samples ($t_0, t_1$):**
* Sample `usage_usec` at $t_0$ and $t_1$ ($\Delta t_{\text{wall}}$ in microseconds):

$$\text{CPU \%} = \frac{\text{usage\_usec}(t_1) - \text{usage\_usec}(t_0)}{\Delta t_{\text{wall}}} \times 100.0$$




* **Formatted Table Columns (Exposed via a static helper for unit testing):**
```text
CONTAINER ID   NAME             CPU %     MEM USAGE / LIMIT     PIDS        GPU UTIL   GPU VRAM
a1b2c3d4e5f6   quiver-a1b2c3d4  14.25%    42.10 MiB / 512.00 MiB 4 / 512     35% (48C)  1.20 GiB / 8.00 GiB

```


*(If a container was launched without `--gpus` or NVML is unavailable on the host, `GPU UTIL` and `GPU VRAM` display `N/A`)*.

---

### Sub-Phase 4E: Unit & Mock Integration Test Suite (`tests/test_gpu.cpp`, `CMakeLists.txt`)

#### 1. Add CTest Target `quiver_gpu_tests` to `CMakeLists.txt`

Register a dedicated test executable in `CMakeLists.txt`:

```cmake
add_executable(quiver_gpu_tests
    tests/test_gpu.cpp
    src/gpu_manager.cpp
    src/cgroup.cpp
    src/state_store.cpp
    src/utils.cpp
    src/monitor.cpp
)
target_include_directories(quiver_gpu_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(quiver_gpu_tests PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
    nlohmann_json::nlohmann_json
    Threads::Threads
    dl
)
add_test(NAME GpuTests COMMAND quiver_gpu_tests)

```

*(Also add `src/gpu_manager.cpp` to `SOURCES` for the main `quiver` executable in `CMakeLists.txt`!)*.

#### 2. Required Test Cases in `tests/test_gpu.cpp`

`tests/test_gpu.cpp` must execute and pass the following 6 deterministic test suites using `<cassert>`:

1. **`test_gpus_flag_parser()`:**
* Verify `Utils::parse_gpus_string("all")` sets `all_gpus == true` and `gpu_indices.empty()`.
* Verify `Utils::parse_gpus_string("ALL")` sets `all_gpus == true`.
* Verify `Utils::parse_gpus_string("0")` returns `all_gpus == false`, `gpu_indices == {0}`.
* Verify `Utils::parse_gpus_string("2,0,1,0")` deduplicates and sorts to `{0, 1, 2}`.
* Verify invalid strings (`""`, `"-1"`, `"0,,1"`, `"gpu0"`, `"abc"`, `"0,a"`) throw `std::invalid_argument`.


2. **`test_mock_gpu_device_discovery_and_resolution()`:**
* Create a temporary directory `/tmp/quiver_gpu_dev_<pid>` simulating `/old_root/dev`.
* Create dummy files `nvidiactl`, `nvidia-uvm`, `nvidia-uvm-tools`, `nvidia0`, `nvidia1`, and non-GPU devices `null`, `zero`.
* Verify `GpuDeviceManager::discover_host_gpu_indices(mock_dev)` returns `{0, 1}`.
* Verify `resolve_device_nodes` with `all_gpus = true` returns `{"nvidiactl", "nvidia-uvm", "nvidia-uvm-tools", "nvidia0", "nvidia1"}`.
* Verify `resolve_device_nodes` with `gpu_indices = {1}` returns `{"nvidiactl", "nvidia-uvm", "nvidia-uvm-tools", "nvidia1"}` (excluding `nvidia0`).
* Verify `resolve_device_nodes` with out-of-range `gpu_indices = {3}` returns `false` and populates `out_error`.
* Verify `setup_device_mounts(..., dry_run = true)` creates the target mount stub files inside a mock container `/dev` directory.


3. **`test_mock_driver_library_discovery_and_injection()`:**
* Create a mock `/old_root` containing `/usr/lib/x86_64-linux-gnu/libcuda.so.550.54`, a relative symlink `libcuda.so.1 -> libcuda.so.550.54`, and `libnvidia-ml.so.1`.
* Verify `GpuDeviceManager::discover_driver_libraries(mock_old_root)` locates both libraries and resolves the relative symlink inside `mock_old_root`.
* Create a mock container rootfs `/tmp/quiver_cont_root_<pid>` and run `GpuDeviceManager::inject_driver_libraries(mock_old_root, mock_cont_root, err, /*dry_run=*/true)`.
* Assert target files `usr/lib/libcuda.so.1`, `usr/lib/libnvidia-ml.so.1`, and symlinks `usr/lib/libcuda.so`, `usr/lib/libnvidia-ml.so` exist inside `mock_cont_root`.


4. **`test_cgroup_live_telemetry_reader()`:**
* Create a mock cgroup v2 directory containing:
* `memory.current`: `"104857600\n"` (100 MiB)
* `memory.max`: `"536870912\n"` (512 MiB)
* `cpu.stat`: `"usage_usec 250000\nuser_usec 150000\nsystem_usec 100000\nthrottled_usec 5000\n"`
* `pids.current`: `"7\n"`
* `pids.max`: `"512\n"`
* `cgroup.procs`: `"1001\n1002\n"`


* Call `CgroupManager::read_metrics(mock_cg_dir, metrics)` and assert exact field values (`memory_current_bytes == 104857600`, `cpu_usage_usec == 250000`, `cpu_throttled_usec == 5000`, `pids_current == 7`, `pids == {1001, 1002}`).


5. **`test_state_store_gpu_persistence()`:**
* Save a `ContainerState` with `resources.all_gpus = false` and `resources.gpu_indices = {0, 2}` to a temporary `StateStore`, reload via `get_container()`, and assert exact round-trip fidelity.




6. **`test_nvml_graceful_runtime_loader_and_telemetry_formatting()`:**
* Instantiate `NvmlTelemetryCollector`. Whether the host has an NVIDIA GPU + driver installed or not, assert initialization and `collect_metrics()` never crash or throw unhandled exceptions.


* Test the telemetry row formatter (`GpuDeviceManager::format_stats_row(...)`) with synthetic `CgroupMetrics` and `GpuDeviceTelemetry` to verify deterministic human-readable formatting (`MiB` / `GiB`, CPU %, PIDs, GPU %).



---

## 4. Sub-Phase 4F: Independent Sub-Agent Compliance & Verification Protocol

Immediately upon completing Sub-Phases 4A–4E, spawn an **independent verification sub-agent** (with a clean context window and zero authorship bias) to audit the codebase against every Phase 4 requirement.

### Sub-Agent Audit Checklist & Deterministic Verification Gate

| Check ID | Requirement Under Test | Exact Verification Command / Inspection | Pass Criteria |
| --- | --- | --- | --- |
| **C4-01** | **Build & ASan Cleanliness** | `cmake -B build -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON && cmake --build build -j$(nproc)` | Compiles `quiver`, `quiver_cgroup_tests`, and `quiver_gpu_tests` with 0 errors and 0 warnings (`-Wall -Wextra -Wpedantic`). |
| **C4-02** | **Automated Test Suites (Cgroup + GPU)** | `ctest --test-dir build --output-on-failure` | 100% of test cases in both `CgroupTests` and `GpuTests` pass with zero ASan/UBSan violations. |
| **C4-03** | **Zero Hard Compile-Time CUDA/NVML Dependency** | `ldd ./build/quiver | grep -i nvidia` | Returns empty (`libnvidia-ml.so.1` is loaded strictly at runtime via `dlopen()`, not linked via `DT_NEEDED`). |
| **C4-04** | **CLI `--gpus` Wiring (`run` & `create`)** | Inspect `src/command_line_handler.cpp` and `src/utils.cpp` | Both `quiver run` and `quiver create` support `--gpus <all|0,1,...>`, persisting `all_gpus` and `gpu_indices` into `ContainerSpec` and `StateStore`. |
| **C4-05** | **Pre-`umount2` Device Node Bind-Mounting** | Inspect `src/container.cpp` and `src/gpu_manager.cpp` | Bind-mounts `/dev/nvidiactl`, `/dev/nvidia-uvm`, `/dev/nvidia-uvm-tools`, `/dev/nvidia-modeset`, and requested `/dev/nvidia[0-N]` from `/old_root/dev` into `/dev` **prior** to `umount2("/old_root", MNT_DETACH)`. |
| **C4-06** | **Read-Only Host Driver Library Injection** | Inspect `GpuDeviceManager::discover_driver_libraries` and `inject_driver_libraries` in `src/gpu_manager.cpp` | Discovers `libcuda.so.1`, `libnvidia-ml.so.1`, and `libnvidia-ptxjitcompiler.so.1` under `/old_root`, resolves symlinks safely inside `/old_root`, and bind-mounts them read-only (`MS_BIND | MS_REMOUNT | MS_RDONLY`) into the container rootfs. |
| **C4-07** | **Live Telemetry (`quiver stats`)** | Inspect `src/main.cpp`, `src/command_line_handler.cpp`, `src/cgroup.cpp`, and `src/gpu_manager.cpp`; run `./build/quiver stats --no-stream` | Reads Cgroup v2 `memory.current`, `memory.max`, `cpu.stat`, `pids.current`, and dynamically queries NVML via `dlopen("libnvidia-ml.so.1", RTLD_LAZY)`, degrading gracefully when no GPU is present. |

---

## 5. File-by-File Modification Matrix Summary

| File Path | Sub-Phase | Action | Key Modifications |
| --- | --- | --- | --- |
| `include/utils.hpp` & `src/utils.cpp` | **4A** | Modify | Add `GpuRequest` struct, `Utils::parse_gpus_string()`, and update `Utils::print_usage()` with `--gpus` and `stats`. |
| `include/gpu_manager.hpp` | **4B, 4C, 4D** | **Create** | Declare `GpuDeviceManager`, `NvmlTelemetryCollector`, `GpuDeviceTelemetry`, device passthrough methods, driver library injection methods, and stats formatting helpers. |
| `src/gpu_manager.cpp` | **4B, 4C, 4D** | **Create** | Implement `/dev/nvidia*` resolution & bind-mounting, host driver `.so` discovery & read-only remounting, and `dlopen("libnvidia-ml.so.1")` telemetry collection. |
| `include/cgroup.hpp` & `src/cgroup.cpp` | **4D** | Modify | Add `CgroupMetrics` struct, `CgroupManager::find_container_cgroup_dir()`, and `CgroupManager::read_metrics()` (`memory.current`, `cpu.stat`, `pids.current`, `cgroup.procs`). |
| `src/container.cpp` | **4B, 4C** | Modify | Call `GpuDeviceManager::setup_device_mounts()` and `inject_driver_libraries()` before `umount2("/old_root", MNT_DETACH)`; set `NVIDIA_VISIBLE_DEVICES` and `LD_LIBRARY_PATH`. |
| `include/command_line_handler.hpp`, `src/command_line_handler.cpp`, `src/main.cpp` | **4A, 4D** | Modify | Add `--gpus` to `run` and `create`, implement `CommandLineHandler::stats()`, and wire `"stats"` in `main.cpp`. |
| `CMakeLists.txt` | **4B, 4E** | Modify | Add `src/gpu_manager.cpp` to `SOURCES` and add `quiver_gpu_tests` CTest executable target. |
| `tests/test_gpu.cpp` | **4E** | **Create** | Implement unit and mock integration tests for `--gpus` parsing, `/dev/nvidia*` resolution, driver `.so` symlink resolution/injection, Cgroup v2 metric parsing, and NVML graceful fallback. |
