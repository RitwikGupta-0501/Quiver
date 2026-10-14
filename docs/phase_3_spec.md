### Phase 3 Engineering Specification: Cgroup v2 Hardware Resource Throttling

#### 1. Architectural Overview & Current Baseline Gaps

Inspecting the post-Phase 2 codebase reveals that the data schemas for hardware resource throttling are already present in `include/state_store.hpp` (`ResourceLimits` containing `memory_max_bytes`, `cpus`, `pids_max`) and plumbed into `ContainerSpec` (`include/container.hpp`) and `ContainerState` (`src/container.cpp`, `src/command_line_handler.cpp`). However, the enforcement engine is completely unimplemented:

1. **Missing `CgroupManager` Subsystem:** Neither `include/cgroup.hpp` nor `src/cgroup.cpp` exists, and `CMakeLists.txt` does not compile any cgroup target.


2. **Missing CLI Resource Flags:** `CommandLineHandler::run()` in `src/command_line_handler.cpp` parses `-v`, `-p`, `-n`, `--vfs`, `--no-remove`, and `-i`, but rejects `--memory`, `--cpus`, and `--pids-limit` as `"Unknown argument"`. `Utils::print_usage()` in `src/utils.cpp` also omits these flags.


3. **Linux Cgroup v2 "No Internal Process Constraint" & Rootless Delegation:**
* On modern Linux systems using the unified Cgroup v2 hierarchy (`/sys/fs/cgroup`), unprivileged users cannot arbitrary `mkdir` in `/sys/fs/cgroup/` unless running under root or a delegated user slice (e.g., `/sys/fs/cgroup/user.slice/user-<uid>.slice/user@<uid>.service/`).
* Furthermore, the Linux kernel enforces the **No Internal Process Constraint**: a non-root cgroup node cannot have domain controllers (`+memory +cpu +pids`) enabled in its `cgroup.subtree_control` while it simultaneously holds live processes in its own `cgroup.procs` (writing `+memory` fails with `EBUSY`).
* If `quiver` is launched from an unprivileged shell whose PID resides in a leaf cgroup (like `app.slice/.../vte-spawn-*.scope`), creating a child cgroup directly inside that scope and enabling `cgroup.subtree_control` will fail with `EBUSY` unless `CgroupManager` resolves a writable parent delegation root or evacuates/places processes in leaf nodes properly.


4. **Synchronization Ordering in `Container::manage_container()`:**
* In `src/container.cpp`, `m_child_pid` currently wakes up from `parent_to_child_pipe[0]` immediately after `setup_user_namespace(m_child_pid, host_uid, host_gid)` and forks `container_init` *before* `m_network.setup()` even runs!


* If `CgroupManager::assign_pid(m_child_pid)` is called *after* `go_signal` is written to `parent_to_child_pipe[1]`, `m_child_pid` can race ahead, fork `container_init`, and `execvp()` unconstrained before the cgroup limits are applied. `CgroupManager` setup and PID enrollment **must** occur while `m_child_pid` is blocked on `parent_to_child_pipe[0]`.




5. **No Test Suite Infrastructure:** `CMakeLists.txt` only defines the `quiver` executable and has no `enable_testing()` or unit/integration test targets.



---

#### 2. Sub-Phase Execution Plan

Because `CgroupManager` is a net-new kernel subsystem with strict Linux cgroup v2 hierarchy invariants, implement Phase 3 across **five sequential sub-phases**:

* **Sub-Phase 3A: Human-Readable Resource Parsers & CLI Flag Wiring (`utils`, `command_line_handler`)**
* **Sub-Phase 3B: `CgroupManager` Core Architecture — Rootless Discovery, Subtree Control & Limit Writers (`cgroup.hpp`, `cgroup.cpp`)**
* **Sub-Phase 3C: Container Lifecycle Integration, Synchronization & Automatic RAII Cleanup (`container.hpp`, `container.cpp`, `CMakeLists.txt`)**
* **Sub-Phase 3D: Unit & Kernel Integration Test Suite (`tests/test_cgroup.cpp`, `CMakeLists.txt`)**
* **Sub-Phase 3E: Independent Sub-Agent Compliance & Verification Audit**

---

#### 3. Detailed Sub-Phase Specifications

##### Sub-Phase 3A: Human-Readable Resource Parsers & CLI Flag Wiring

###### 1. Add Resource Parsing Helpers to `Utils` (`include/utils.hpp`, `src/utils.cpp`)

Users pass human-readable strings on the CLI (e.g., `--memory 512m`, `--memory 2g`, `--cpus 1.5`, `--pids-limit 128`). Add deterministic, strict parsers to `Utils`:

```cpp
// include/utils.hpp additions inside namespace Utils:
auto parse_memory_string(std::string_view str) -> std::size_t;
auto parse_cpus_string(std::string_view str) -> double;
auto parse_pids_limit(std::string_view str) -> int;

```

* **`Utils::parse_memory_string(std::string_view str)` Contract:**
* Trim leading/trailing whitespace. Reject empty strings or negative signs (`-`) with `std::invalid_argument`.
* Parse numeric prefix (`std::uint64_t` or `double` if fractional like `1.5g`, or integer digits) and optional case-insensitive unit suffix:
* No suffix or `b` / `B`: multiplier $= 1$
* `k` / `K` / `kb` / `kib`: multiplier $= 1024ULL$
* `m` / `M` / `mb` / `mib`: multiplier $= 1024ULL \times 1024ULL$
* `g` / `G` / `gb` / `gib`: multiplier $= 1024ULL \times 1024ULL \times 1024ULL$


* Throw `std::invalid_argument` on unknown suffixes, trailing garbage, or zero bytes (`0`).


* **`Utils::parse_cpus_string(std::string_view str)` Contract:**
* Parse via `std::stod` (ensuring the entire string is consumed).
* Validate `cpus > 0.0` and `!std::isnan(cpus)` and `!std::isinf(cpus)`. Throw `std::invalid_argument` otherwise.


* **`Utils::parse_pids_limit(std::string_view str)` Contract:**
* Accept `"max"` or `"-1"` as unlimited (`0` or `-1` convention; note in `include/state_store.hpp`, `pids_max` defaults to `512`; use `pids_max <= 0` to mean `"max"` / unlimited, and `pids_max > 0` for a strict limit).


* Otherwise parse positive integer via `std::stoi` (verifying full string consumption and value $\ge 1$). Throw `std::invalid_argument` on invalid input.



###### 2. Wire `--memory`, `--cpus`, and `--pids-limit` in `CommandLineHandler` (`src/command_line_handler.cpp`, `src/utils.cpp`)

In `CommandLineHandler::run()` (`src/command_line_handler.cpp`), extend the option parsing loop before the unknown argument handler:

```cpp
} else if (arg == "-m" || arg == "--memory") {
    if (i + 1 < cmds.size()) {
        try {
            spec.resources.memory_max_bytes = Utils::parse_memory_string(cmds[++i]);
        } catch (const std::exception& e) {
            std::cerr << "Error: Invalid --memory value: " << e.what() << '\n';
            return;
        }
    } else {
        std::cerr << "Error: " << arg << " requires an argument\n";
        return;
    }
} else if (arg == "--cpus") {
    if (i + 1 < cmds.size()) {
        try {
            spec.resources.cpus = Utils::parse_cpus_string(cmds[++i]);
        } catch (const std::exception& e) {
            std::cerr << "Error: Invalid --cpus value: " << e.what() << '\n';
            return;
        }
    } else {
        std::cerr << "Error: --cpus requires an argument\n";
        return;
    }
} else if (arg == "--pids-limit") {
    if (i + 1 < cmds.size()) {
        try {
            spec.resources.pids_limit = Utils::parse_pids_limit(cmds[++i]); // Note: field name in ResourceLimits is pids_max
        } catch (const std::exception& e) {
            std::cerr << "Error: Invalid --pids-limit value: " << e.what() << '\n';
            return;
        }
    } else {
        std::cerr << "Error: --pids-limit requires an argument\n";
        return;
    }
}

```

(Important: Assign to `spec.resources.pids_max`, matching `struct ResourceLimits` in `include/state_store.hpp`).

Update `Utils::print_usage()` in `src/utils.cpp` under `run [options]` to document:

```text
      -m, --memory <limit>         Memory limit (e.g., 512m, 2g)
      --cpus <quota>               Number of CPUs (e.g., 0.5, 2.0)
      --pids-limit <max>           Tune container pids limit (default: 512)

```

---

##### Sub-Phase 3B: `CgroupManager` Core Architecture (`include/cgroup.hpp`, `src/cgroup.cpp`)

###### 1. Class Interface (`include/cgroup.hpp`)

Design `CgroupManager` as a move-only RAII class that also supports dependency injection of a custom `cgroup_root` path so unit tests can validate file formatting on a mock `tmpfs` directory without requiring root or kernel cgroupfs privileges.

```cpp
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <sys/types.h>
#include "state_store.hpp"

namespace fs = std::filesystem;

class CgroupManager {
public:
    static constexpr std::uint64_t DEFAULT_CPU_PERIOD_US = 100'000; // 100ms standard CFS period

    explicit CgroupManager(
        std::string container_id,
        ResourceLimits limits,
        std::optional<fs::path> custom_root = std::nullopt
    );
    ~CgroupManager();

    CgroupManager(const CgroupManager&) = delete;
    CgroupManager& operator=(const CgroupManager&) = delete;
    CgroupManager(CgroupManager&& other) noexcept;
    CgroupManager& operator=(CgroupManager&& other) noexcept;

    // 1. Discover writable cgroup v2 parent, create quiver-<id> leaf dir, enable controllers, write limits
    bool setup( std::string& out_error );

    // 2. Write target PID into <cgroup_dir>/cgroup.procs
    bool assign_pid( pid_t pid, std::string& out_error ) const;

    // 3. Remove the cgroup leaf directory (retries on EBUSY while processes drain)
    bool cleanup() noexcept;

    // Static formatting helpers (exposed for deterministic unit testing)
    [[nodiscard]] static std::string format_memory_max(std::size_t memory_max_bytes);
    [[nodiscard]] static std::string format_cpu_max(double cpus, std::uint64_t period_us = DEFAULT_CPU_PERIOD_US);
    [[nodiscard]] static std::string format_pids_max(int pids_max);

    // Inspection helpers
    [[nodiscard]] const fs::path& cgroup_path() const noexcept { return m_cgroup_dir; }
    [[nodiscard]] bool is_active() const noexcept { return m_active; }
    [[nodiscard]] static bool is_cgroup_v2_available(const fs::path& root = "/sys/fs/cgroup") noexcept;

private:
    std::optional<fs::path> resolve_delegated_parent(std::string& out_error) const;
    static bool enable_subtree_controllers(const fs::path& parent_dir, const std::vector<std::string>& controllers);
    static bool write_cgroup_file(const fs::path& file_path, std::string_view content, std::string& out_error);

    std::string m_container_id{};
    ResourceLimits m_limits{};
    std::optional<fs::path> m_custom_root{};
    fs::path m_cgroup_dir{};
    bool m_active{false};
};

```

###### 2. Cgroup v2 Detection & Rootless Hierarchy Resolution (`resolve_delegated_parent`)

In `src/cgroup.cpp`, implement a multi-tier discovery strategy that works across:

* **Tier 0 (Test Override):** If `m_custom_root.has_value()`, use `*m_custom_root` directly as the parent directory.
* **Tier 1 (Root / Direct `/sys/fs/cgroup` Write Access):** If `::geteuid() == 0` or `/sys/fs/cgroup` is directly writable (`::access("/sys/fs/cgroup", W_OK) == 0`), use `/sys/fs/cgroup/quiver` as the parent container slice (ensuring `/sys/fs/cgroup/quiver` exists and enabling controllers on `/sys/fs/cgroup` and `/sys/fs/cgroup/quiver`).
* **Tier 2 (Rootless Systemd User Slice Delegation):**
1. Read `/proc/self/cgroup` and locate the unified v2 entry (`0::<relative_path>`).
2. Construct `current_cg = "/sys/fs/cgroup" / relative_path`.
3. **Handling the "No Internal Process Constraint" (`EBUSY`):** Because the current `quiver` CLI/manager process lives inside `current_cg/cgroup.procs`, the kernel will forbid enabling `+memory +cpu +pids` in `current_cg/cgroup.subtree_control`!
4. Therefore, walk up or check candidate delegated directories owned by `::getuid()`:
* **Candidate A:** `current_cg.parent_path()` (e.g., `/sys/fs/cgroup/user.slice/user-1000.slice/user@1000.service/app.slice` or `.../user@1000.service`) if `::access(candidate.c_str(), W_OK) == 0`.
* **Candidate B:** `/sys/fs/cgroup/user.slice/user-<uid>.slice/user@<uid>.service/quiver.slice` (creating `quiver.slice` inside `user@<uid>.service` if `user@<uid>.service` is writable by the user).
* **Candidate C:** If `current_cg` is writable and has no other processes (or if `current_cg` is the only writable node), create a sibling or two-level leaf (`current_cg / "supervisor"` for the manager PID and `current_cg / ("quiver-" + short_id)` for the container) so the parent node holds zero internal PIDs before writing to `cgroup.subtree_control`.


5. Once the writable delegated parent directory `parent_dir` is selected, create the container leaf directory:
`m_cgroup_dir = parent_dir / ("quiver-" + m_container_id.substr(0, 12));`



###### 3. Enabling Subtree Controllers (`enable_subtree_controllers`)

Before writing `memory.max`, `cpu.max`, or `pids.max` in `m_cgroup_dir`, inspect `parent_dir / "cgroup.controllers"` to see which controllers are available in this slice, and write each required controller (`+memory`, `+cpu`, `+pids`) individually to `parent_dir / "cgroup.subtree_control"`:

* Write controllers one token at a time (e.g., `"+memory"`, `"+cpu"`, `"+pids"`) using raw POSIX `::open(..., O_WRONLY)` + `::write()` (never `O_CREAT` or `O_TRUNC` on `cgroup.subtree_control` when running unprivileged, or standard `O_WRONLY`, as some kernel versions reject `O_CREAT` on cgroup pseudo-files if permissions are tight).
* Writing one controller at a time ensures that if a host kernel lacks `memory` delegation on a rootless user slice (e.g., only `cpu` and `pids` are delegated by systemd), `+cpu` and `+pids` still succeed rather than failing an all-or-nothing `"+memory +cpu +pids"` write.

###### 4. Exact Limit Formatting & Control File Writes

Use a dedicated `CgroupManager::write_cgroup_file(const fs::path& file_path, std::string_view content, std::string& out_error)` helper that opens existing cgroup control files with `::open(file_path.c_str(), O_WRONLY | O_TRUNC)` (falling back to `O_WRONLY | O_CREAT | O_TRUNC, 0644` only when `m_custom_root.has_value()` for unit tests!):

1. **Memory & Swap Throttling (`--memory <limit>`):**
* `format_memory_max(memory_max_bytes)`:
* If `memory_max_bytes == 0`, return `"max\n"`.
* Otherwise return `std::format("{}\n", memory_max_bytes)`.


* When `m_limits.memory_max_bytes > 0`:
* Write `format_memory_max(m_limits.memory_max_bytes)` to `m_cgroup_dir / "memory.max"`.
* Write `"0\n"` to `m_cgroup_dir / "memory.swap.max"` (strictly disabling swap spillover so the container cannot bypass its RAM ceiling via swap, or if `memory.swap.max` does not exist because the host kernel has swap accounting disabled, log a `WARN` to `Monitor` and do not fail).




2. **CPU Bandwidth Quota (`--cpus <quota>`):**
* `format_cpu_max(cpus, period_us)`:
* Cgroup v2 `cpu.max` expects `"$MAX $PERIOD\n"` in microseconds.
* If `cpus <= 0.0`, return `std::format("max {}\n", period_us)`.
* Otherwise compute `std::uint64_t quota_us = static_cast<std::uint64_t>(std::llround(cpus * static_cast<double>(period_us)));` (clamping minimum `quota_us` to `1000` us if `cpus > 0.0` so rounding never produces `0`).
* Return `std::format("{} {}\n", quota_us, period_us)`.


* When `m_limits.cpus > 0.0`:
* Write `format_cpu_max(m_limits.cpus)` to `m_cgroup_dir / "cpu.max"`.




3. **PIDs Limit / Fork-Bomb Protection (`--pids-limit <max>`):**
* `format_pids_max(pids_max)`:
* If `pids_max <= 0`, return `"max\n"`.
* Otherwise return `std::format("{}\n", pids_max)`.


* Write `format_pids_max(m_limits.pids_max)` to `m_cgroup_dir / "pids.max"` (note: since `pids_max` defaults to `512` in `ResourceLimits`, every container automatically gets `pids.max = 512` fork-bomb protection by default unless overridden!).





###### 5. Process Enrollment (`assign_pid`) & Automatic Teardown (`cleanup`)

* **`assign_pid(pid_t pid, std::string& out_error)`:**
* Writes `std::format("{}\n", pid)` to `m_cgroup_dir / "cgroup.procs"`.


* **`cleanup() noexcept`:**
* Invoked automatically by `~CgroupManager()` and explicitly in `Container::manage_container()` after `waitpid(m_child_pid, ...)` completes.
* On real cgroupfs, a cgroup directory can only be removed with `::rmdir(m_cgroup_dir.c_str())` (never `unlink` pseudo-files inside `/sys/fs/cgroup`, as the kernel forbids `unlink` on cgroup files!).
* If `m_custom_root.has_value()` (unit test mock filesystem), use `fs::remove_all(m_cgroup_dir, ec)`.
* On real cgroupfs (`!m_custom_root.has_value()`):
1. Attempt `::rmdir(m_cgroup_dir.c_str())`.
2. If `errno == EBUSY` (e.g., an orphaned child process inside the container is still exiting), read `m_cgroup_dir / "cgroup.kill"` (if present on Linux 5.14+, write `"1"` to `cgroup.kill` to atomically `SIGKILL` all lingering tasks in the cgroup) or read `cgroup.procs` and send `SIGKILL` to each PID.
3. Retry `::rmdir(m_cgroup_dir.c_str())` up to 10 times with a 20ms backoff (`usleep(20000)`).
4. Set `m_active = false`.





---

##### Sub-Phase 3C: Container Lifecycle Integration & Synchronization (`src/container.cpp`, `CMakeLists.txt`)

###### 1. Fix Pipe Synchronization & Integrate `CgroupManager` in `Container::manage_container()` (`src/container.cpp`)

Look at lines 124–140 of `src/container.cpp` in the current codebase:

```cpp
setup_user_namespace(m_child_pid, host_uid, host_gid);

char go_signal = '1';
if (write(parent_to_child_pipe[1], &go_signal, 1) != 1) {
    Utils::handle_error("Failed to write sync to child");
}
close(parent_to_child_pipe[1]);

if (m_network.setup(m_child_pid, m_spec.ports) != 0) { ... }

```

Notice that `go_signal` is currently written *before* `m_network.setup()`. We must initialize `CgroupManager`, apply limits, enroll `m_child_pid`, and set up networking **before** writing `go_signal` to `parent_to_child_pipe[1]`:

```cpp
// Inside Container::manage_container() parent branch (m_child_pid > 0):
close(parent_to_child_pipe[0]);
close(child_to_parent_pipe[1]);

char ready_signal = 0;
if (read(child_to_parent_pipe[0], &ready_signal, 1) != 1) {
    Utils::handle_error("could not read signal from child");
}
close(child_to_parent_pipe[0]);

setup_user_namespace(m_child_pid, host_uid, host_gid);

// Phase 3: Setup Cgroup v2 limits and enroll m_child_pid BEFORE waking child
CgroupManager cgroup(m_spec.id, m_spec.resources);
std::string cg_err;
if (cgroup.setup(cg_err)) {
    if (!cgroup.assign_pid(m_child_pid, cg_err)) {
        Monitor::get_instance().log("WARN", "Cgroup", "Failed to assign PID " + std::to_string(m_child_pid) + " to cgroup: " + cg_err);
    } else {
        Monitor::get_instance().log("INFO", "Cgroup", "Applied cgroup v2 limits at " + cgroup.cgroup_path().string());
    }
} else {
    Monitor::get_instance().log("WARN", "Cgroup", "Cgroup v2 delegation unavailable (continuing unthrottled): " + cg_err);
}

if (m_network.setup(m_child_pid, m_spec.ports) != 0) {
    Monitor::get_instance().log("ERROR", "Network", "Failed to setup networking for PID " + std::to_string(m_child_pid));
}

// Now release m_child_pid to fork container_init inside the configured cgroup & network namespace
char go_signal = '1';
if (write(parent_to_child_pipe[1], &go_signal, 1) != 1) {
    Utils::handle_error("Failed to write sync to child");
}
close(parent_to_child_pipe[1]);

```

* **Graceful Degradation Policy:** On environments where systemd cgroup v2 delegation is restricted (such as unprivileged Docker-in-Docker CI runners or WSL2 without cgroup v2 user delegation), `cgroup.setup(cg_err)` logs a structured `WARN` via `Monitor` and allows the container to boot if no explicit `--memory` or `--cpus` flags were forced, OR if you want strict enforcement when flags are passed, log the warning clearly. (For maximum testability across rootless hosts, logging to `Monitor` + `std::cerr` warning when unprivileged delegation is missing while strictly enforcing whenever writable cgroup v2 is available ensures both safety and portability).
* **Exit Cleanup:** Right after `waitpid(m_child_pid, &status, 0);` in `Container::manage_container()`:


```cpp
m_store.update_container_status(m_spec.id, "exited");
m_network.teardown();
cgroup.cleanup();

```


Because `cgroup.cleanup()` is called explicitly before `exit(EXIT_SUCCESS)` (note that calling `std::exit()` skips local stack destructors in C++!), calling `cgroup.cleanup()` explicitly before `exit(EXIT_SUCCESS)` (or replacing `exit(EXIT_SUCCESS)` with `_exit(EXIT_SUCCESS)` after explicit cleanup) **guarantees** the cgroup directory is removed on container exit.

###### 2. Update `CMakeLists.txt` to Compile `src/cgroup.cpp`

Add `src/cgroup.cpp` to `SOURCES` in `CMakeLists.txt`.

---

##### Sub-Phase 3D: Unit & Kernel Integration Test Suite (`tests/test_cgroup.cpp`, `CMakeLists.txt`)

###### 1. Add CTest Target to `CMakeLists.txt`

Extend `CMakeLists.txt` to build a standalone test executable `quiver_cgroup_tests` using standard C++20 `<cassert>` (zero external test framework downloads needed) registered with `enable_testing()` and `add_test()`:

```cmake
enable_testing()

add_executable(quiver_cgroup_tests
    tests/test_cgroup.cpp
    src/cgroup.cpp
    src/state_store.cpp
    src/utils.cpp
    src/monitor.cpp
)
target_include_directories(quiver_cgroup_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(quiver_cgroup_tests PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
    nlohmann_json::nlohmann_json
    Threads::Threads
)
add_test(NAME CgroupTests COMMAND quiver_cgroup_tests)

```

###### 2. Required Test Cases in `tests/test_cgroup.cpp`

`tests/test_cgroup.cpp` must execute and pass the following 6 deterministic test suites:

1. **`test_memory_parser()`:**
* Verify `Utils::parse_memory_string("512m") == 536870912ULL`.
* Verify `Utils::parse_memory_string("2G") == 2147483648ULL`.
* Verify `Utils::parse_memory_string("1024k") == 1048576ULL`.
* Verify `Utils::parse_memory_string("4096") == 4096ULL`.
* Verify malformed inputs (`""`, `"-512m"`, `"0"`, `"128x"`, `"abc"`) throw `std::invalid_argument`.


2. **`test_cpu_and_pids_parsers()`:**
* Verify `Utils::parse_cpus_string("0.5") == 0.5` and `Utils::parse_cpus_string("2.5") == 2.5`.
* Verify invalid CPU inputs (`"0"`, `"-1.0"`, `"abc"`) throw `std::invalid_argument`.
* Verify `Utils::parse_pids_limit("256") == 256` and `Utils::parse_pids_limit("max") <= 0`.
* Verify invalid PID inputs (`"0"`, `"-5"`, `"xyz"`) throw `std::invalid_argument`.


3. **`test_cgroup_formatting()`:**
* Verify `CgroupManager::format_memory_max(536870912) == "536870912\n"` and `format_memory_max(0) == "max\n"`.
* Verify `CgroupManager::format_cpu_max(2.0, 100000) == "200000 100000\n"`, `format_cpu_max(0.5, 100000) == "50000 100000\n"`, and `format_cpu_max(0.0, 100000) == "max 100000\n"`.
* Verify `CgroupManager::format_pids_max(512) == "512\n"` and `format_pids_max(0) == "max\n"`.


4. **`test_mock_cgroup_hierarchy_lifecycle()`:**
* Create a temporary directory `/tmp/quiver_cg_test_<pid>` simulating `/sys/fs/cgroup`.
* Pre-populate `cgroup.controllers` with `"cpu memory pids\n"` and `cgroup.subtree_control` with `""`.
* Instantiate `CgroupManager cg("abcdef1234567890", ResourceLimits{.memory_max_bytes = 268435456, .cpus = 1.5, .pids_max = 64}, mock_root)`.
* Call `cg.setup(err)` and assert it returns `true`.
* Assert directory `<mock_root>/quiver-abcdef123456` exists.
* Read back and assert exact file contents:
* `memory.max` == `"268435456\n"`
* `memory.swap.max` == `"0\n"`
* `cpu.max` == `"150000 100000\n"`
* `pids.max` == `"64\n"`


* Call `cg.assign_pid(4242, err)` and assert `cgroup.procs` == `"4242\n"`.
* Call `cg.cleanup()` and assert `<mock_root>/quiver-abcdef123456` no longer exists.


5. **`test_state_store_resource_persistence()`:**
* Create a `StateStore` in a temporary directory, save a `ContainerState` with `memory_max_bytes = 1073741824`, `cpus = 2.0`, `pids_max = 128`, reload via `get_container()`, and assert all three `ResourceLimits` fields round-trip identically through JSON.




6. **`test_live_kernel_cgroup_v2_delegation()` (Conditional on Host Support):**
* If `/sys/fs/cgroup/cgroup.controllers` exists and a delegated slice is writable by the current user, instantiate `CgroupManager` without a custom root, fork a sleeping child process (`usleep(200000)`), enroll its PID into the real kernel cgroup, verify `/sys/fs/cgroup/.../quiver-<id>/cgroup.procs` contains the child PID, wait for the child to exit, call `cleanup()`, and assert the kernel cgroup directory is removed.



---

#### 4. Sub-Phase 3E: Independent Sub-Agent Compliance & Verification Protocol

Immediately after completing Sub-Phases 3A–3D, spawn an **independent verification sub-agent** (with a clean context window and zero authorship bias) to audit the implementation against every Phase 3 requirement.

##### Sub-Agent Audit Checklist & Deterministic Verification Gate

| Check ID | Requirement Under Test | Exact Verification Command / Inspection | Pass Criteria |
| --- | --- | --- | --- |
| **C3-01** | **Build & ASan Cleanliness** | `cmake -B build -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON && cmake --build build -j$(nproc)` | Compiles `quiver` and `quiver_cgroup_tests` with 0 errors and 0 warnings (`-Wall -Wextra -Wpedantic`). |
| **C3-02** | **Automated Unit & Integration Tests** | `ctest --test-dir build --output-on-failure` | 100% of test cases in `quiver_cgroup_tests` pass with zero AddressSanitizer or UndefinedBehaviorSanitizer violations. |
| **C3-03** | **Rootless Systemd Slice & Direct v2 Hierarchy** | Inspect `CgroupManager::resolve_delegated_parent` and `enable_subtree_controllers` in `src/cgroup.cpp` | Supports both direct `/sys/fs/cgroup/` hierarchy creation and rootless `/proc/self/cgroup` systemd user slice delegation while respecting the Cgroup v2 No Internal Process Constraint. |
| **C3-04** | **`--memory <limit>` Implementation** | Inspect `src/cgroup.cpp` and `src/command_line_handler.cpp` | CLI parses `-m` / `--memory` (with `k`, `m`, `g` suffixes) into `spec.resources.memory_max_bytes` and writes both `memory.max` and `memory.swap.max`. |
| **C3-05** | **`--cpus <quota>` Implementation** | Inspect `src/cgroup.cpp` and `src/command_line_handler.cpp` | CLI parses `--cpus <double>` into `spec.resources.cpus` and writes `<quota_us> 100000` to `cpu.max`. |
| **C3-06** | **`--pids-limit <max>` Implementation** | Inspect `src/cgroup.cpp` and `src/command_line_handler.cpp` | CLI parses `--pids-limit <int |
| **C3-07** | **Pre-Exec Pipe Synchronization** | Inspect `Container::manage_container` in `src/container.cpp` | `cgroup.setup()` and `cgroup.assign_pid(m_child_pid)` execute **before** writing `go_signal` to `parent_to_child_pipe[1]`, preventing unconstrained child execution races. |
| **C3-08** | **Automatic Cgroup Directory Cleanup on Exit** | Inspect `Container::manage_container` in `src/container.cpp` and `CgroupManager::cleanup` in `src/cgroup.cpp` | `cgroup.cleanup()` is called explicitly prior to `exit(EXIT_SUCCESS)` in `manage_container()` and in `~CgroupManager()`, using `rmdir()` with `cgroup.kill` / `SIGKILL` retry on `EBUSY`. |

---

#### 5. File-by-File Modification Matrix Summary

| File Path | Sub-Phase | Action | Key Modifications |
| --- | --- | --- | --- |
| `include/utils.hpp` & `src/utils.cpp` | **3A** | Modify | Add `parse_memory_string`, `parse_cpus_string`, `parse_pids_limit`; update `print_usage()` with `--memory`, `--cpus`, `--pids-limit`. |
| `src/command_line_handler.cpp` | **3A** | Modify | Parse `-m`/`--memory`, `--cpus`, and `--pids-limit` in `CommandLineHandler::run()` into `spec.resources`. |
| `include/cgroup.hpp` | **3B** | **Create** | Declare `CgroupManager` RAII class, formatting helpers, and rootless delegation discovery methods. |
| `src/cgroup.cpp` | **3B** | **Create** | Implement `CgroupManager`: v2 hierarchy discovery, `+memory +cpu +pids` subtree controller activation, `memory.max`/`memory.swap.max`, `cpu.max`, `pids.max` writers, `cgroup.procs` enrollment, and `rmdir` + `cgroup.kill` teardown. |
| `src/container.cpp` | **3C** | Modify | Instantiate `CgroupManager` in `manage_container()`, enroll `m_child_pid` and run `m_network.setup()` **before** `go_signal`, and call `cgroup.cleanup()` on container exit before `exit(EXIT_SUCCESS)`. |
| `CMakeLists.txt` | **3C, 3D** | Modify | Add `src/cgroup.cpp` to `SOURCES`, enable CTest (`enable_testing()`), and add `quiver_cgroup_tests` target. |
| `tests/test_cgroup.cpp` | **3D** | **Create** | Implement unit and kernel integration tests for resource string parsing, cgroup v2 control file formatting, mock hierarchy lifecycle, JSON state persistence, and live cgroup v2 delegation. |
