# Phase 2 Engineering Specification: Core Container Lifecycle, RAII Refactoring & OCI Image Spec v1.1.1 Compliance

## 1. Architectural Overview & Current Baseline Gaps

Based on the post-Phase 1 codebase, `StateStore` (`src/state_store.cpp`, `include/state_store.hpp`) and `CMakeLists.txt` are operational, and `database_manager` / `package_manager` have been removed. However, inspecting the remaining runtime modules reveals critical structural defects that Phase 2 resolves:

1. **Pervasive `static` and File-Scope Global State:**
* `include/container.hpp` declares 14 `static` member variables (`m_store`, `m_container_id`, `m_child_pid`, `m_volumes`, `m_term`, etc.).


* `include/terminal.hpp` declares 6 `static` member variables (`m_sfd`, `m_container_id`, `m_orig_term`, `m_container_pid`, `m_running`, `m_raw_mode_enabled`).


* `include/network.hpp` declares `static pid_t m_net_pid` and `static std::map<pid_t, NetworkConfig> m_container_networks`.


* `src/command_line_handler.cpp` defines 11 file-scope globals (`volumes`, `commands`, `forward_ports`, `image_name`, `container_id`, `root_fs`, etc.).




2. **Broken Rootless Namespace & OverlayFS Primitives:**
* `Container::setup_user_namespace()` in `src/container.cpp` hardcodes `"0 1000 1\n"` and writes to `/proc/self/uid_map` from the **parent** process after `fork()`, rather than `/proc/<m_child_pid>/uid_map` using host `getuid()` / `getgid()`.


* `Container::run_container()` mounts `overlay` directly without `,userxattr`, causing rootless OverlayFS failures on Linux 5.11+ kernels, and uses `system("cp -a ...")` for VFS mode.


* `Utils::extract_tarball()` in `src/utils.cpp` hardcodes `-xzf` (failing on uncompressed `.tar` or `.tar.zst` blobs) and lacks `--no-same-owner`, causing `tar` `EPERM` errors inside rootless environments.




3. **Incomplete OCI Image Spec (v1.1.1) Compliance & Command Execution:**
* `ImageManager::get_manifest()` in `src/image_manager.cpp` only sends `Accept: application/vnd.docker.distribution.manifest.v2+json, application/vnd.oci.image.index.v1+json` (missing `application/vnd.oci.image.manifest.v1+json`).


* Multi-arch resolution hardcodes `entry["platform"]["architecture"] == "amd64"`.


* `ImageManager` never downloads or parses the `config` blob (`application/vnd.oci.image.config.v1+json`), never verifies blob SHA-256 digests or byte sizes, and extracts layers directly with `tar` without processing OCI whiteout files (`.wh.<filename>` and `.wh..wh..opq`).


* `CommandLineHandler::run()` rejects positional command arguments after `-i <image>` as `"Unknown argument"` and hardcodes `container.exec("/bin/bash", commands)`, while `Container::run_container()` ignores `args.commands` and hardcodes `execl(shell, shell, (char*)0)`.




4. **Disconnected `Monitor` Logging & Fork-Safety Hazards:**
* `Monitor` (`src/monitor.cpp`) is never invoked. Furthermore, `Utils::write_file()` opens files with `std::ios::app`—which breaks `/proc/<pid>/uid_map` writes if any error occurs or if reused, and `Monitor` lacks a formatted `log_fmt` or `std::string_view` interface (currently taking `std::string&` non-const reference).





---

## 2. Sub-Phase Execution Plan

To keep changes modular, testable, and reviewable, execute Phase 2 across **four sequential sub-phases**:

* **Sub-Phase 2A: Utilities, `Monitor` Logger & File I/O Hardening (`utils`, `monitor`)**
* **Sub-Phase 2B: OCI Image Spec v1.1.1 Compliance (`state_store`, `image_manager`)**
* **Sub-Phase 2C: Rootless Namespaces, OverlayFS (`userxattr`), and Arbitrary Command Execution (`mount`, `container`)**
* **Sub-Phase 2D: Full RAII De-Staticization (`ContainerSpec`, `Container`, `Terminal`, `Network`, `CommandLineHandler`)**
* **Sub-Phase 2E: Independent Sub-Agent Compliance & Verification Audit**

---

## 3. Detailed Sub-Phase Specifications

### Sub-Phase 2A: Utilities, `Monitor` Logger & File I/O Hardening

#### 1. Fix `Utils::write_file` vs `Utils::append_file` (`include/utils.hpp`, `src/utils.cpp`)

Currently, `Utils::write_file` opens the target path with `std::ios::app`. Kernel pseudo-files in `/proc/<pid>/uid_map`, `gid_map`, and `setgroups` require a single atomic `write(2)` at offset 0 (`O_WRONLY | O_TRUNC`).

* **Change in `include/utils.hpp` & `src/utils.cpp`:**
* Modify `Utils::write_file(const fs::path& path, std::string_view buffer)` to open with `std::ios::out | std::ios::trunc` (or raw POSIX `::open(path.c_str(), O_WRONLY | O_TRUNC | O_CREAT, 0644)` + `::write()`).
* Add `Utils::append_file(const fs::path& path, std::string_view buffer)` using `std::ios::out | std::ios::app` specifically for `Monitor` log appending.
* Update `Utils::get_logs_path(std::string_view container_id)` to take `std::string_view container_id` instead of `const pid_t& pid`, returning `~/.quiver/logs/<container_id>.log` (and ensuring `~/.quiver/logs` exists).


* Update `Utils::get_sock_path` to support container bundle paths (`~/.quiver/containers/<container_id>/attach.sock`) alongside PID lookup, or pass `container_id` consistently.



#### 2. Upgrade `Utils::extract_tarball` for Multi-Compression & Rootless Safety (`include/utils.hpp`, `src/utils.cpp`)

* **Signature Update:**
```cpp
enum class ArchiveCompression {
    Auto,
    Uncompressed, // application/vnd.oci.image.layer.v1.tar
    Gzip,         // application/vnd.oci.image.layer.v1.tar+gzip or docker .tar.gzip
    Zstd          // application/vnd.oci.image.layer.v1.tar+zstd
};

auto extract_tarball(
    const fs::path& tarball_path,
    const fs::path& destination_path,
    ArchiveCompression compression = ArchiveCompression::Auto
) -> bool;

```


* **Implementation Details:**
* Construct the `tar` argument vector for `execvp("tar", ...)`:
* Base flags: `"tar"`, `"--no-same-owner"`, `"--no-same-permissions"`, `"-x"`
* Compression flag based on `ArchiveCompression`:
* `Gzip`: `"-z"`
* `Zstd`: `"--zstd"`
* `Uncompressed`: *(no compression flag)*
* `Auto`: `"-a"`


* File & directory flags: `"-f"`, `tarball_path.c_str()`, `"-C"`, `destination_path.c_str()`, `nullptr`.





#### 3. Add SHA-256 File Verification & Host Architecture Detection to `Utils`

* Add to `include/utils.hpp` and `src/utils.cpp`:
```cpp
auto verify_sha256_and_size(const fs::path& file_path, std::string_view expected_digest, std::uintmax_t expected_size) -> bool;
auto get_host_oci_arch() -> std::string;

```


* **`verify_sha256_and_size` Implementation:**
* Check `fs::file_size(file_path) == expected_size`. If mismatch, return `false`.
* Strip `"sha256:"` prefix from `expected_digest` if present.
* Stream `file_path` in 64 KB chunks through OpenSSL `EVP_DigestInit_ex` / `EVP_DigestUpdate` / `EVP_DigestFinal_ex` (`EVP_sha256()`), avoiding deprecated `SHA256()` calls where possible, convert to lowercase hex, and compare against `expected_digest`.


* **`get_host_oci_arch` Implementation:**
* Call `uname(&uts)` from `<sys/utsname.h>`:
* `"x86_64"` $\rightarrow$ `"amd64"`
* `"aarch64"` or `"arm64"` $\rightarrow$ `"arm64"`
* `"armv7l"` $\rightarrow$ `"arm"`
* `"riscv64"` $\rightarrow$ `"riscv64"`
* Fallback: `uts.machine`.





#### 4. Refactor `Monitor` for Fork-Safe Async Lifecycle Logging (`include/monitor.hpp`, `src/monitor.cpp`)

* **Fork-Safety Warning:** In C++, if `Monitor::get_instance()` spawns its `std::thread m_worker` **before** `fork()`, the child process inherits a locked/broken mutex and dead background thread.
* **Design Requirements:**
* Ensure `m_running` is initialized to `true` (`std::atomic<bool> m_running{true};` — note that in the current `include/monitor.hpp`, `m_running{}` initializes to `false`!).


* Change `auto log(std::string& buffer) -> void` to `auto log(std::string_view level, std::string_view component, std::string_view message) -> void` and convenience helper `auto log(std::string message) -> void` (by value/move or `std::string_view`).


* Format each log line with an ISO-8601 UTC timestamp and PID:
`[2026-10-14T15:15:00Z] [PID 12345] [INFO] [Container] Starting container...`
* Inside `Monitor` worker loop, call `Utils::append_file(current_path, current_buffer)`.
* In `Container::manage_container()`, call `Monitor::get_instance().set_log_path(Utils::get_logs_path(m_spec.id))` **after** the manager daemon `fork()` and `setsid()` so the background logging thread is created cleanly inside the long-lived container manager process.



---

### Sub-Phase 2B: OCI Image Spec v1.1.1 Compliance

#### 1. Expand `ImageMetadata` Schema (`include/state_store.hpp`)

Update `ImageMetadata` in `include/state_store.hpp` to persist the full OCI execution context:

```cpp
struct ImageMetadata {
    std::string name{};
    std::string tag{"latest"};
    std::string rootfs_path{};
    std::string created_at{};
    std::vector<std::string> default_env{};
    std::vector<std::string> default_entrypoint{};
    std::vector<std::string> default_cmd{"/bin/sh"};
    std::string working_dir{"/"};

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
        ImageMetadata, name, tag, rootfs_path, created_at,
        default_env, default_entrypoint, default_cmd, working_dir
    )
};

```

*(Using `NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT` ensures backward compatibility with any existing `index.json` entries missing the new fields).*

#### 2. Manifest Negotiation & Platform Selection (`src/image_manager.cpp`)

* **`ImageManager::get_manifest`:**
Update the `Accept` header to include all four standard OCI v1.1.1 and Docker v2 media types:


```cpp
{"Accept",
 "application/vnd.oci.image.manifest.v1+json, "
 "application/vnd.oci.image.index.v1+json, "
 "application/vnd.docker.distribution.manifest.v2+json, "
 "application/vnd.docker.distribution.manifest.list.v2+json"}

```


* **Dynamic Platform Resolution in `ImageManager::pull_image_from_registry`:**
Replace the hardcoded `"amd64"` check:


```cpp
const std::string host_arch = Utils::get_host_oci_arch();
if (manifest.contains("manifests") && manifest["manifests"].is_array()) {
    std::string arch_digest;
    for (const auto& entry : manifest["manifests"]) {
        if (entry.contains("platform")) {
            const auto& plat = entry["platform"];
            std::string arch = plat.value("architecture", "");
            std::string os   = plat.value("os", "");
            if (arch == host_arch && os == "linux") {
                arch_digest = entry.value("digest", "");
                break;
            }
        }
    }
    if (arch_digest.empty()) {
        error = "No matching linux/" + host_arch + " manifest found in image index.";
        return false;
    }
    std::string arch_image_name = repo + "@" + arch_digest;
    if (!get_manifest(arch_image_name, token, manifest, error)) return false;
}

```



#### 3. Fetch, Verify & Parse OCI Image Configuration (`application/vnd.oci.image.config.v1+json`)

* Add helper `bool ImageManager::fetch_and_parse_config(const json& manifest, const std::string& repo, const std::string& token, ImageMetadata& out_meta, std::string& error)`:
1. Extract `manifest["config"]["digest"]` and `manifest["config"]["size"]`.
2. Download `[https://registry-1.docker.io/v2/](https://registry-1.docker.io/v2/)<repo>/blobs/<config_digest>` to a temporary file `<image_path>/config.json.tmp`.
3. Verify the downloaded config blob using `Utils::verify_sha256_and_size(tmp_config_path, config_digest, config_size)`.
4. Parse JSON and extract `config` object (`json cfg = config_json.value("config", json::object());`):
* `Env`: If `cfg.contains("Env") && cfg["Env"].is_array()`, populate `out_meta.default_env`.
* `Entrypoint`: If `cfg.contains("Entrypoint") && cfg["Entrypoint"].is_array()`, populate `out_meta.default_entrypoint`.
* `Cmd`: If `cfg.contains("Cmd") && cfg["Cmd"].is_array()`, populate `out_meta.default_cmd`.
* `WorkingDir`: If `cfg.contains("WorkingDir") && cfg["WorkingDir"].is_string()` and non-empty, set `out_meta.working_dir`.


5. Remove `<image_path>/config.json.tmp` after parsing.



#### 4. Layer Digest Verification, MediaType Dispatch & OCI Whiteout Processing

* **Layer Verification:**
For every layer in `manifest["layers"]`:
* Record `digest = layer["digest"].get<std::string>()`, `size = layer["size"].get<std::uintmax_t>()`, and `media_type = layer.value("mediaType", "")`.
* After `download_futures[i].get()` completes, run `Utils::verify_sha256_and_size(tarball_paths[i], digest, size)`. Abort pull and clean up if digest or size verification fails.


* **MediaType Compression Mapping:**
* If `media_type.ends_with("+gzip")` or `media_type.ends_with(".gzip")`: `ArchiveCompression::Gzip`
* If `media_type.ends_with("+zstd")` or `media_type.ends_with(".zstd")`: `ArchiveCompression::Zstd`
* If `media_type.ends_with(".tar")`: `ArchiveCompression::Uncompressed`
* Otherwise: `ArchiveCompression::Auto`


* **OCI Layer Whiteout Semantics (`.wh.<file>` and `.wh..wh..opq`):**
Per OCI Image Spec v1.1.1 §5.2 (Layer Changesets), if layer $N$ deletes a file or directory created in layer $0 \dots N-1$, simply running `tar -x -C <rootfs>` directly into the target rootfs will write literal `.wh.<filename>` marker files instead of deleting the lower layer's files.
* **Implementation (`ImageManager::apply_layer_with_whiteouts`):**
1. Extract layer $i$ into a staging directory `<destination_path>/.layer_staging_<i>`.
2. Walk `<destination_path>/.layer_staging_<i>` recursively:
* **First Pass (Opaque Whiteouts):** Check if directory `D` in staging contains `.wh..wh..opq`. If so, remove all existing entries inside the corresponding `<destination_path>/<rel_D>` directory on the target rootfs, and delete `.wh..wh..opq` from staging.
* **Second Pass (Explicit Whiteouts):** For any file whose filename starts with `.wh.` (and is not `.wh..wh..opq`), let `target_name = filename.substr(4)`. Remove `<destination_path>/<rel_parent>/<target_name>` recursively from the target rootfs, and delete the `.wh.<target_name>` marker from staging.
* **Third Pass (Merge):** Move/copy remaining files, symlinks, and directories from `<destination_path>/.layer_staging_<i>` into `<destination_path>`, overwriting existing target files.


3. Remove `<destination_path>/.layer_staging_<i>`.





---

### Sub-Phase 2C: Rootless Namespaces, OverlayFS (`userxattr`), and Arbitrary Command Execution

#### 1. Fix `Container::setup_user_namespace` (`include/container.hpp`, `src/container.cpp`)

Currently, `setup_user_namespace()` writes hardcoded `"0 1000 1\n"` to `/proc/self/uid_map`. Because `setup_user_namespace()` is called by the **manager parent** while `m_child_pid` blocks on `parent_to_child_pipe` (or if `unshare(CLONE_NEWUSER)` is moved into the child or target PID is specified), targeting `/proc/<target_pid>/...` with dynamic host credentials fixes non-1000 UIDs and guarantees the mapping applies to the target namespace:

```cpp
void Container::setup_user_namespace(pid_t target_pid, uid_t host_uid, gid_t host_gid) {
    std::string pid_str = std::to_string(target_pid);
    std::string uid_map = std::format("0 {} 1\n", host_uid);
    std::string gid_map = std::format("0 {} 1\n", host_gid);

    Utils::write_file("/proc/" + pid_str + "/setgroups", "deny\n");
    Utils::write_file("/proc/" + pid_str + "/gid_map", gid_map);
    Utils::write_file("/proc/" + pid_str + "/uid_map", uid_map);
}

```

* **Important Namespace Ordering Note:** In `src/container.cpp`, `manage_container()` currently calls `unshare(CLONE_NEWUSER)` at the start of `manage_container()` before forking `m_child_pid`, and `m_child_pid` then calls `unshare(CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNET | CLONE_NEWNS)`.


* If `unshare(CLONE_NEWUSER)` occurs in `manage_container()`, `m_child_pid` cannot call `unshare(CLONE_NEWNS | CLONE_NEWNET)` until `uid_map` and `gid_map` are written!
* **Fix:** Capture `uid_t host_uid = ::getuid(); gid_t host_gid = ::getgid();` **before** `unshare(CLONE_NEWUSER)` (because after `unshare(CLONE_NEWUSER)`, `getuid()` returns `65534` `nobody`!). Better yet, have `m_child_pid` perform `unshare(CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNET | CLONE_NEWNS)` in a single atomic syscall, signal the parent via `child_to_parent_pipe`, have the parent (still in the host user namespace with real `getuid()` / `getgid()`) call `setup_user_namespace(m_child_pid, ::getuid(), ::getgid())`, and then signal `parent_to_child_pipe`! This also allows `slirp4netns` in the parent to attach cleanly to `m_child_pid`.



#### 2. Add `Mount::overlay` with `userxattr` (`include/mount.hpp`, `src/mount.cpp`, `src/container.cpp`)

* Add `Mount::overlay` to `include/mount.hpp` and `src/mount.cpp`:
```cpp
void Mount::overlay(
    const fs::path& lower_dir,
    const fs::path& upper_dir,
    const fs::path& work_dir,
    const fs::path& merged_dir
) {
    std::string opts = std::format(
        "lowerdir={},upperdir={},workdir={},userxattr",
        lower_dir.string(), upper_dir.string(), work_dir.string()
    );
    if (::mount("overlay", merged_dir.c_str(), "overlay", MS_NODEV, opts.c_str()) == ERR) {
        Utils::handle_error("Cannot mount rootless overlayfs (" + opts + "): " + std::strerror(errno));
    }
}

```


* Replace the raw `mount("overlay", ...)` block in `Container::run_container()` with `Mount::overlay(...)`.


* Replace `system(copy_command.c_str())` in VFS fallback with `fs::copy(m_spec.rootfs_path, final_filesystem, fs::copy_options::recursive | fs::copy_options::copy_symlinks | fs::copy_options::overwrite_existing)`.


* **Bundle Directory Consolidation:** Store the container's OverlayFS `upper`, `work`, and `merged` directories directly inside `~/.quiver/containers/<container_id>/rootfs/` (via `m_store.container_bundle_dir(m_spec.id) / "rootfs"`) instead of scattering them by temporary PID in `~/.quiver/filesystems/<temp_pid>`.



#### 3. OCI Environment, WorkingDir & Arbitrary Command Execution (`src/container.cpp`)

In `Container::run_container()`, replace the hardcoded `clearenv()` + `execl(shell, ...)` tail with OCI-compliant process initialization:

1. **Working Directory:**
```cpp
std::string work_dir = m_spec.working_dir.empty() ? "/" : m_spec.working_dir;
Utils::ensure_dirs(work_dir);
if (::chdir(work_dir.c_str()) == ERR) {
    Utils::handle_error("Unable to chdir to WorkingDir: " + work_dir);
}

```


2. **Environment Variables:**
```cpp
::clearenv();
::setenv("TERM", "xterm-256color", 1);
::setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 1);
::setenv("HOME", "/root", 1);
for (const auto& env_kv : m_spec.env) {
    auto eq_pos = env_kv.find('=');
    if (eq_pos != std::string::npos) {
        std::string k = env_kv.substr(0, eq_pos);
        std::string v = env_kv.substr(eq_pos + 1);
        ::setenv(k.c_str(), v.c_str(), 1);
    }
}

```


3. **Command Resolution & `execvp()`:**
Build the final `argv` vector according to OCI runtime precedence:
* If user passed CLI command/args (`!m_spec.command.empty()`):
* If image has `default_entrypoint`, prepend `default_entrypoint` ONLY if user didn't override entrypoint, OR follow `quiver run -i <image> <cmd> [args...]` requirement: execute `m_spec.command` directly via `execvp()`.


* Else if image metadata has `default_entrypoint` or `default_cmd`:
* Concatenate `default_entrypoint` + `default_cmd`.


* Else fallback to interactive shell discovery (`/bin/bash`, `/bin/sh`, `/bin/ash`).




```cpp
std::vector<char*> c_argv;
c_argv.reserve(effective_cmd.size() + 1);
for (auto& arg : effective_cmd) {
    c_argv.push_back(arg.data());
}
c_argv.push_back(nullptr);

::execvp(c_argv[0], c_argv.data());
Utils::handle_error(std::format("execvp('{}') failed: {}", c_argv[0], std::strerror(errno)));

```



---

### Sub-Phase 2D: Full RAII De-Staticization (`ContainerSpec`, `Container`, `Terminal`, `Network`, `CommandLineHandler`)

#### 1. Introduce `ContainerSpec` (`include/container.hpp`)

Replace the 10-parameter `Container` constructor and all 14 `static` fields in `include/container.hpp` with a clean, value-semantic `ContainerSpec`:

```cpp
#pragma once
#include <string>
#include <vector>
#include <utility>
#include "state_store.hpp"
#include "terminal.hpp"
#include "network.hpp"

struct ContainerSpec {
    std::string id{};
    std::string name{};
    std::string image{};
    std::string hostname{};
    std::string rootfs_path{};
    std::string working_dir{"/"};
    std::vector<std::string> env{};
    std::vector<std::string> command{};
    std::vector<VolumeMount> volumes{};
    std::vector<PortMapping> ports{};
    ResourceLimits resources{};
    bool use_vfs{false};
    bool no_remove{false};
};

class Container {
public:
    explicit Container(ContainerSpec spec, StateStore& store);
    ~Container() = default;

    Container(const Container&) = delete;
    Container& operator=(const Container&) = delete;
    Container(Container&&) noexcept = default;
    Container& operator=(Container&&) noexcept = default;

    void start();
    static void stop(pid_t container_pid, pid_t net_pid);
    static void attach(pid_t container_pid);

private:
    void manage_container(const fs::path& filesystem_dir);
    void run_container(int slave_fd, const fs::path& filesystem_dir);
    static void setup_user_namespace(pid_t target_pid, uid_t host_uid, gid_t host_gid);
    static void secure_kill(pid_t pid);

    ContainerSpec m_spec;
    StateStore& m_store;
    pid_t m_child_pid{-1};
    Terminal m_term{};
    Terminal::PtyArgs m_pty_args{};
    Network m_network{};
};

```

#### 2. De-Static `Terminal` (`include/terminal.hpp`, `src/terminal.cpp`)

Remove `static` from all member variables in `include/terminal.hpp` and encapsulate FD & `termios` cleanup in the destructor:

```cpp
#pragma once
#include <string>
#include <termios.h>
#include <sys/ioctl.h>

inline constexpr std::size_t SLAVE_NAME_MAX = 128;

class Terminal {
public:
    struct PtyArgs {
        int master_fd{-1};
        int slave_fd{-1};
        char slave_name[SLAVE_NAME_MAX]{};
        winsize window_size{};
    };

    Terminal() = default;
    ~Terminal();

    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;
    Terminal(Terminal&& other) noexcept;
    Terminal& operator=(Terminal&& other) noexcept;

    void start_pty_session(PtyArgs& args);
    void redirect_io(int slave_fd);
    void start_server(const PtyArgs& args, const std::string& container_id, pid_t container_pid);
    static void connect_to_server(pid_t container_pid);
    void cleanup();
    [[nodiscard]] int get_sfd() const noexcept { return m_sfd; }

private:
    int m_sfd{-1};
    int m_master_fd{-1};
    std::string m_container_id{};
    std::string m_sock_path{};
    pid_t m_container_pid{-1};
    bool m_running{false};
};

```

* In `Terminal::~Terminal()`, invoke `cleanup()` which closes `m_sfd` and `m_master_fd` (if `>= 0`) and unlinks `m_sock_path` if owned by the server instance.

#### 3. De-Static `Network` (`include/network.hpp`, `src/network.cpp`)

Convert `Network` into an RAII session manager for the active container's `slirp4netns` child process, while keeping stateless socket RPC helpers (`add_port_forward`, `remove_port_forward`, `connect_namespaces`) available for CLI commands targeting running containers by PID:

```cpp
#pragma once
#include <sys/types.h>
#include <vector>
#include <utility>
#include <string>
#include "state_store.hpp"

class Network {
public:
    Network() = default;
    ~Network();

    Network(const Network&) = delete;
    Network& operator=(const Network&) = delete;
    Network(Network&& other) noexcept;
    Network& operator=(Network&& other) noexcept;

    int setup(pid_t container_pid, const std::vector<PortMapping>& ports);
    void teardown();

    [[nodiscard]] pid_t get_net_pid() const noexcept { return m_slirp_pid; }
    [[nodiscard]] const std::string& get_api_socket() const noexcept { return m_api_socket; }

    // Stateless RPC helpers for CLI commands (`quiver network add/rm`)
    static int add_port_forward(pid_t container_pid, int host_port, int container_port);
    static int remove_port_forward(pid_t container_pid, int host_port);
    static int connect_namespaces(pid_t target_pid, int host_port, int target_port);
    static std::string get_api_socket_path(pid_t container_pid);

private:
    static int send_api_command(pid_t container_pid, const std::string& json_command);
    static int wait_for_api_socket(const std::string& socket_path, int timeout_ms = 5000);

    pid_t m_container_pid{-1};
    pid_t m_slirp_pid{-1};
    std::string m_api_socket{};
};

```

* **Unified `slirp4netns` Launch:** Always launch `slirp4netns` with `--api-socket <api_socket>` even when initial `ports` is empty! In v0.1, `setup_networking()` (when `m_forward_ports` was empty) omitted `--api-socket`, which caused `quiver network add <container_id> <host:cont>` on a running container to fail later because no API socket existed! Unifying `setup_networking` and `setup_networking_with_ports` into a single `Network::setup(pid_t container_pid, const std::vector<PortMapping>& ports)` fixes dynamic port addition on any running container.



#### 4. De-Static `CommandLineHandler` & Parse Positional Command Args (`include/command_line_handler.hpp`, `src/command_line_handler.cpp`, `src/main.cpp`)

* Delete all `extern` declarations from `include/command_line_handler.hpp` and all 11 file-scope global variables at the top of `src/command_line_handler.cpp`.


* Refactor `CommandLineHandler` to take `StateStore& store` and `ImageManager& img_manager` in its constructor:
```cpp
class CommandLineHandler {
public:
    explicit CommandLineHandler(StateStore& store, ImageManager& img_manager);
    ~CommandLineHandler() = default;

    void run(const std::vector<std::string>& args);
    void attach(const std::vector<std::string>& args);
    void ps(const std::vector<std::string>& args);
    void rm(const std::vector<std::string>& args);
    void image(const std::vector<std::string>& args);
    void volume(const std::vector<std::string>& args);
    void network(const std::vector<std::string>& args);
    void create(const std::vector<std::string>& args);
    void pull(const std::vector<std::string>& args);
    void start(const std::vector<std::string>& args);
    void stop(const std::vector<std::string>& args);
    void vfs(const std::vector<std::string>& args);

private:
    StateStore& m_store;
    ImageManager& m_img_manager;
};

```


* **Positional Command Parsing in `CommandLineHandler::run`:**
In the argument loop of `CommandLineHandler::run(const std::vector<std::string>& cmds)`:


* Once `-i` / `--image <image_name>` has been parsed, any non-flag token (or any token following `--`, or any trailing positional arguments once flags are consumed) is treated as the start of the container command vector:
```cpp
} else if (!arg.starts_with('-')) {
    // Everything from this positional argument onward is the container command + args
    for (size_t j = i; j < cmds.size(); ++j) {
        spec.command.push_back(cmds[j]);
    }
    break;
} else if (arg == "--") {
    for (size_t j = i + 1; j < cmds.size(); ++j) {
        spec.command.push_back(cmds[j]);
    }
    break;
}

```


* Query `m_store.get_image(name, tag)` after pulling to populate `spec.rootfs_path`, `spec.env = img_meta->default_env`, `spec.working_dir = img_meta->working_dir`, and if `spec.command.empty()`, resolve `img_meta->default_entrypoint` + `img_meta->default_cmd`.



---

## 4. Sub-Phase 2E: Independent Sub-Agent Compliance & Verification Protocol

Immediately upon completing Sub-Phases 2A–2D, spawn an **independent verification sub-agent** (with a clean context window and zero authorship bias) to audit the codebase against every Phase 2 requirement.

### Sub-Agent Audit Prompt & Automated Checklist

The compliance sub-agent must execute the following deterministic verification commands and fail the gate if any check does not pass:

| Check ID | Requirement Under Test | Exact Verification Command / Inspection | Pass Criteria |
| --- | --- | --- | --- |
| **C-01** | **Build & ASan Cleanliness** | `cmake -B build -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON && cmake --build build -j$(nproc)` | Compiles with 0 errors and 0 warnings (`-Wall -Wextra -Wpedantic`). |
| **C-02** | **Zero Static/Global Instance State** | `grep -rn "static " include/container.hpp include/terminal.hpp include/network.hpp` and inspect `include/command_line_handler.hpp` & `src/command_line_handler.cpp` | Zero `static` member variables in `Container`, `Terminal`, `Network`; zero `extern` or file-scope mutable globals in `command_line_handler`. |
| **C-03** | **Dynamic Rootless UID/GID Mapping** | Inspect `Container::setup_user_namespace` in `src/container.cpp` | Uses `getuid()` / `getgid()` captured prior to `CLONE_NEWUSER`, writes to `/proc/<target_pid>/setgroups`, `gid_map`, `uid_map` using truncating writes (no `1000` or `/proc/self/` in parent). |
| **C-04** | **Rootless OverlayFS `userxattr**` | Inspect `Mount::overlay` and `src/container.cpp` | Mounts `overlay` with `,userxattr` option; `system("cp -a ...")` is replaced with `std::filesystem::copy`. |
| **C-05** | **Tarball Extraction Fix** | Inspect `Utils::extract_tarball` in `src/utils.cpp` | Uses `-C`, `--no-same-owner`, and supports `.tar`, `.tar+gzip`, and `.tar+zstd` via `layer.mediaType`. |
| **C-06** | **OCI v1.1.1 Manifest, Config, Digest & Platform** | Inspect `src/image_manager.cpp` and `src/utils.cpp` | `Accept` header includes `application/vnd.oci.image.manifest.v1+json`; `uname` resolves `platform.architecture` + `platform.os == "linux"`; SHA-256 + size verified for config and layers; `Env`, `Cmd`, `Entrypoint`, `WorkingDir` parsed and stored in `index.json`. |
| **C-07** | **OCI Layer Whiteout Handling** | Inspect `src/image_manager.cpp` | Processes `.wh.<filename>` and `.wh..wh..opq` markers across layers before merging into rootfs. |
| **C-08** | **Arbitrary Command Execution (`execvp`)** | Run `./build/quiver run -i alpine:latest uname -a` and `./build/quiver run -i alpine:latest ls -la /` | Executes custom commands + arguments via `execvp()` instead of dropping into hardcoded `/bin/bash`. |
| **C-09** | **Async `Monitor` Lifecycle Logging** | Inspect `src/container.cpp`, `src/network.cpp`, `src/terminal.cpp`, `src/image_manager.cpp` and check `~/.quiver/logs/<container_id>.log` | `Monitor::get_instance().log(...)` is actively wired across container lifecycle modules; raw `std::cerr << "DEBUG: ..."` statements are removed; log file is populated at `~/.quiver/logs/<container_id>.log`. |

---

## 5. File-by-File Modification Matrix Summary

| File Path | Sub-Phase | Key Modifications |
| --- | --- | --- |
| `include/utils.hpp` & `src/utils.cpp` | **2A** | Fix `write_file` (`O_TRUNC`), add `append_file`, update `extract_tarball` (`--no-same-owner`, `-C`, gzip/zstd/uncompressed), add `verify_sha256_and_size` (OpenSSL EVP), add `get_host_oci_arch` (`uname`), update `get_logs_path(std::string_view)`. |
| `include/monitor.hpp` & `src/monitor.cpp` | **2A** | Fix `m_running{true}` initialization bug, accept `std::string_view` / value strings instead of `std::string&`, add timestamp/level formatting, use `Utils::append_file`. |
| `include/state_store.hpp` | **2B** | Add `default_entrypoint` and `working_dir` to `ImageMetadata`. |
| `include/image_manager.hpp` & `src/image_manager.cpp` | **2B** | Add OCI manifest `Accept` header, dynamic `linux/<arch>` selection, `fetch_and_parse_config`, SHA-256 & byte size verification on blobs, `layer.mediaType` compression detection, and `.wh.` / `.wh..wh..opq` whiteout application. |
| `include/mount.hpp` & `src/mount.cpp` | **2C** | Add `Mount::overlay()` with `,userxattr`. |
| `include/container.hpp` & `src/container.cpp` | **2C, 2D** | Define `ContainerSpec`, strip all `static` member state, fix `setup_user_namespace(target_pid, host_uid, host_gid)`, apply `Env` & `WorkingDir`, execute commands via `execvp()`, route lifecycle logs to `Monitor`. |
| `include/terminal.hpp` & `src/terminal.cpp` | **2D** | Strip all `static` fields, implement RAII socket/FD cleanup in `~Terminal()`. |
| `include/network.hpp` & `src/network.cpp` | **2D** | Strip `static` state, unify `slirp4netns` launch to always expose `--api-socket`, implement RAII process teardown in `~Network()`. |
| `include/command_line_handler.hpp`, `src/command_line_handler.cpp`, `src/main.cpp` | **2D** | Delete file-scope globals and `extern` declarations, instantiate `CommandLineHandler(store, img_manager)`, parse trailing positional command arguments in `run`. |
