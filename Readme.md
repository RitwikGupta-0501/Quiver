<table>
  <tr>
    <td align="center" width="300px">
      <img src="./logo.jpg" alt="Quiver Logo" width="100%"/>
    </td>
    <td align="left">
      <h1>Quiver</h1>
      <p>High-Performance, Rootless Linux Container Runtime with Native GPU Passthrough & Hardware Throttling</p>
    </td>
  </tr>
</table>

## Overview

**Quiver** is a modern, high-performance, rootless container runtime written in C++20 for Linux systems. Designed with zero daemon overhead, Quiver leverages Linux user namespaces (`CLONE_NEWUSER`), Cgroup v2 unified hierarchy throttling, and dynamic host NVIDIA driver injection to deliver rootless GPU container isolation without requiring system privileges, Docker, or privileged daemon services.

---

## Key Features & Architecture (v2.0)

- **Rootless GPU Passthrough (`--gpus <all|0,1,...>`)**:
  Zero-daemon `/dev/nvidia*` character device node discovery and unprivileged bind-mounting alongside read-only host driver library injection (`libcuda.so.1`, `libnvidia-ml.so.1`, `libnvidia-ptxjitcompiler.so.1`). Preserves existing filesystem mount flags (`statvfs`) to ensure seamless execution in rootless namespaces.

- **Cgroup v2 Hardware Throttling (`--memory`, `--cpus`, `--pids-limit`)**:
  Native rootless systemd slice delegation and unified hierarchy enforcement (`memory.max`, `cpu.max`, `pids.max`). Throttles CPU quota, memory ceilings, and process counts with zero root escalation.

- **Live Hardware & GPU Telemetry (`quiver stats`)**:
  Real-time streaming dashboard querying dynamic `dlopen("libnvidia-ml.so.1")` NVML metrics (GPU streaming multiprocessor utilization, temperature, and container VRAM tracking) aggregated alongside Cgroup v2 memory and CPU metrics.

- **Lock-Free Atomic JSON `StateStore`**:
  High-concurrency container and image metadata engine utilizing lock-free atomic write-and-rename semantics (`state.json`), benchmarked at 100 concurrent workers with 0% lock contention.

- **OCI Image Spec v1.1.1 Engine**:
  Compliant image distribution engine supporting multi-architecture manifest negotiation, SHA-256 blob integrity verification, image configuration parsing (`Env`, `Entrypoint`, `Cmd`, `WorkingDir`), and 3-pass layer whiteout extraction (`.wh.<file>` explicit whiteouts and `.wh..wh..opq` opaque whiteouts).

- **Rootless User-Mode Networking**:
  Zero-privilege container networking and TCP port forwarding via `slirp4netns` integration.

- **Robust Terminal Proxy**:
  Non-blocking PTY master/slave proxy with terminal raw mode restoration, automatic resize signal handling (`SIGWINCH`), and log draining.

---

## Build Prerequisites

To build and run Quiver, you need a modern Linux distribution (or WSL2) supporting C++20:

### Required Toolchain & Libraries

- **C++ Compiler**: GCC (g++ >= 11) or Clang (clang++ >= 14) with full C++20 support
- **Build System**: CMake >= 3.20 and Ninja or Make
- **Libraries**:
  - `libcurl` (`libcurl4-openssl-dev`)
  - `OpenSSL` (`libssl-dev`, `libcrypto`)
  - `libutil`
  - `libcpr` (automatically fetched via CMake FetchContent if not installed on system)
  - `nlohmann/json` (automatically fetched via CMake FetchContent if not installed on system)
  - `libdl` (standard system library)

### Runtime Dependencies

- **slirp4netns**: Required for rootless container networking (`sudo apt install slirp4netns`)

### Installing Dependencies (Ubuntu / Debian)

```bash
sudo apt update
sudo apt install -y build-essential cmake pkg-config libssl-dev libcurl4-openssl-dev slirp4netns
```

---

## Compilation & Installation

Quiver uses standard CMake for configuration and building.

### 1. Release Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

The compiled `quiver` binary will be located at `build/quiver`.

### 2. Debug Build with AddressSanitizer & UndefinedBehaviorSanitizer

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON
cmake --build build -j$(nproc)
```

---

## Running the Test Suite

Execute the comprehensive test and verification suite via CTest:

```bash
ctest --test-dir build --output-on-failure -V
```

This verifies all 5 core test suites:
- `CgroupTests`: Cgroup v2 limit enforcement and live telemetry
- `GpuTests`: Rootless GPU character device and driver injection resolution
- `StateBenchmarkTests`: Atomic StateStore contention and throughput benchmarks
- `ConcurrencyStressTests`: 50–100 container concurrent lifecycle operations
- `E2EGpuWorkloadTests`: End-to-end GPU passthrough and telemetry formatting

---

## Command Reference & Usage

### 1. Pulling an Image

```bash
# Pull an OCI image from Docker Hub (fetches linux/amd64 or host arch)
quiver pull ubuntu:latest
quiver pull alpine:latest
```

### 2. Running Containers

```bash
# Interactive container with bash
quiver run -i ubuntu:latest -n dev-shell /bin/bash

# Run with Cgroup v2 hardware resource constraints
quiver run -i ubuntu:latest --memory 2g --cpus 2.0 --pids-limit 256 /bin/bash

# Run with NVIDIA GPU passthrough
quiver run -i nvidia/cuda:12.2.0-base-ubuntu22.04 --gpus all nvidia-smi

# Run with read-only volume mounts and port forwarding
quiver run -i nginx:latest -p 8080:80 -v /host/data:/container/data:ro
```

### 3. Container Lifecycle Management

```bash
# List active running containers
quiver ps

# List all containers (including stopped and created)
quiver ps -a

# Stop a running container
quiver stop <container_id>

# Remove an exited container
quiver rm <container_id>

# Attach to a running container's terminal
quiver attach <container_id>
```

### 4. Live Telemetry Dashboard (`quiver stats`)

Monitor real-time CPU, memory, PIDs, GPU utilization, and VRAM across containers:

```bash
# Live stream stats dashboard (refreshes every second)
quiver stats

# Single-shot telemetry snapshot
quiver stats --no-stream
```

### 5. Managing Images, Volumes, and Network Ports

```bash
# List local cached images
quiver image ls

# Remove cached image
quiver image rm <image_name:tag>

# List container volume mounts
quiver volume ls

# Add volume mount to container
quiver create volume <container_id> /host/dir:/container/dir:ro

# Port forwarding management
quiver network ls
quiver network add <container_id> 8080:80
quiver network rm <host_port>
```

---

## License

This project is licensed under the GNU General Public License v3.0. See the [LICENSE](LICENSE) file for details.
