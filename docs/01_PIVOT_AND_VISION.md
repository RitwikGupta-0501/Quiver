# Architectural Pivot: From Generic Container Prototype to Rootless GPU/HPC Runtime

## 1. Project Lineage & Fork Statement
This repository began as **Quiver**, a collaborative systems programming prototype exploring rootless Linux containers using C++20, Linux namespaces, OverlayFS, and `slirp4netns`[cite: 1]. 

Following the initial prototype phase, this fork diverged as an independent engineering effort with two primary objectives:
1. **Architectural Re-engineering:** Eliminate the concurrency bottlenecks, monolithic SQLite state locks, and static global states present in the original prototype[cite: 1].
2. **Domain Specialization (GPU & HPC Compute Isolation):** Pivot from a general-purpose Docker CLI clone into a **Rootless GPU-Aware Container Runtime** tailored for High-Performance Computing (HPC), CUDA workloads, and multi-tenant AI inference/training environments.

---

## 2. Why Pivot to GPU & HPC Containers?
Standard container runtimes isolate processes from host hardware by default, populating a minimal `/dev` (`null`, `zero`, `random`, `urandom`, `tty`)[cite: 1]. Consequently, compute-intensive CUDA or PyTorch workloads executed inside a standard rootless container cannot access the host's physical GPUs or Unified Virtual Memory (UVM).

Conversely, running AI and HPC workloads directly on a bare-metal host introduces severe operational problems:
* **CUDA & Driver Dependency Conflicts:** Concurrent workloads frequently require incompatible CUDA toolkits, cuDNN versions, or Python environments on the same physical node.
* **Multi-GPU "Noisy Neighbor" Contention:** Without device-level isolation, a process on a multi-GPU node can inadvertently allocate VRAM or compute contexts across all physical GPUs, crashing co-located jobs.
* **Unprivileged Multi-Tenancy:** Shared research clusters and HPC nodes cannot grant root (`sudo`) privileges to users, disqualifying traditional root-daemon container engines.

---

## 3. Core Design Pillars of the New Architecture
1. **Native Rootless GPU Passthrough (`--gpus`):** Selectively expose host NVIDIA character devices (`/dev/nvidiactl`, `/dev/nvidia-uvm`, `/dev/nvidia-uvm-tools`, and specific `/dev/nvidia[0-N]` indices) and read-only user-space driver libraries (`libcuda.so`, `libnvidia-ml.so`) directly into the container's mount namespace without requiring external OCI hook toolkits.
2. **Hardware Telemetry via NVML:** Integrate the NVIDIA Management Library (NVML) alongside Linux Cgroup v2 metrics to provide unified real-time telemetry (CPU, Host RAM, GPU VRAM, and SM utilization) per container.
3. **Lock-Free OCI-Style State Management:** Replace the centralized SQLite database (`quiver.db`)[cite: 1] with atomic, per-container JSON state directories (`~/.quiver/containers/<id>/state.json`), enabling high-concurrency container launches without database lock contention.
4. **Strict Cgroup v2 Resource Throttling:** Enforce deterministic bounds on CPU bandwidth (`cpu.max`), memory ceilings (`memory.max`), and process limits (`pids.max`) via the unified Linux Cgroup v2 hierarchy.
