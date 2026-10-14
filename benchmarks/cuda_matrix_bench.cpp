#include <iostream>
#include <vector>
#include <string>
#include <string_view>
#include <chrono>
#include <cmath>
#include <cstring>
#include <dlfcn.h>
#include <cstdint>

namespace {

// CUDA Driver API Types & Constants
using CUresult = int;
using CUdevice = int;
using CUcontext = void*;
using CUmodule = void*;
using CUfunction = void*;
using CUdeviceptr = unsigned long long;

constexpr CUresult CUDA_SUCCESS = 0;

// Embedded PTX Assembly for Element-wise SGEMM Kernel (sm_50+)
// C[i] = A[i] * B[i] + 1.0f
const char* PTX_SOURCE = R"ptx(
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
)ptx";

struct CudaDriverApi {
    void* handle{nullptr};

    CUresult (*cuInit)(unsigned int){nullptr};
    CUresult (*cuDeviceGetCount)(int*){nullptr};
    CUresult (*cuDeviceGet)(CUdevice*, int){nullptr};
    CUresult (*cuDeviceGetName)(char*, int, CUdevice){nullptr};
    CUresult (*cuCtxCreate_v2)(CUcontext*, unsigned int, CUdevice){nullptr};
    CUresult (*cuCtxDestroy_v2)(CUcontext){nullptr};
    CUresult (*cuMemAlloc_v2)(CUdeviceptr*, size_t){nullptr};
    CUresult (*cuMemFree_v2)(CUdeviceptr){nullptr};
    CUresult (*cuMemcpyHtoD_v2)(CUdeviceptr, const void*, size_t){nullptr};
    CUresult (*cuMemcpyDtoH_v2)(void*, CUdeviceptr, size_t){nullptr};
    CUresult (*cuModuleLoadData)(CUmodule*, const void*){nullptr};
    CUresult (*cuModuleUnload)(CUmodule){nullptr};
    CUresult (*cuModuleGetFunction)(CUfunction*, CUmodule, const char*){nullptr};
    CUresult (*cuLaunchKernel)(CUfunction, unsigned int, unsigned int, unsigned int,
                               unsigned int, unsigned int, unsigned int,
                               unsigned int, void*, void**, void**){nullptr};
    CUresult (*cuCtxSynchronize)(void){nullptr};
    CUresult (*cuGetErrorString)(CUresult, const char**){nullptr};

    bool load() {
        const char* candidate_libs[] = {
            "libcuda.so.1",
            "libcuda.so",
            "/usr/lib/x86_64-linux-gnu/libcuda.so.1",
            "/usr/lib64/libcuda.so.1",
            "/usr/lib/wsl/lib/libcuda.so.1"
        };
        for (const char* lib : candidate_libs) {
            handle = dlopen(lib, RTLD_LAZY);
            if (handle) break;
        }
        if (!handle) return false;

        #define LOAD_SYM(fn) fn = reinterpret_cast<decltype(fn)>(dlsym(handle, #fn)); if (!fn) return false;
        LOAD_SYM(cuInit);
        LOAD_SYM(cuDeviceGetCount);
        LOAD_SYM(cuDeviceGet);
        LOAD_SYM(cuDeviceGetName);
        LOAD_SYM(cuCtxCreate_v2);
        LOAD_SYM(cuCtxDestroy_v2);
        LOAD_SYM(cuMemAlloc_v2);
        LOAD_SYM(cuMemFree_v2);
        LOAD_SYM(cuMemcpyHtoD_v2);
        LOAD_SYM(cuMemcpyDtoH_v2);
        LOAD_SYM(cuModuleLoadData);
        LOAD_SYM(cuModuleUnload);
        LOAD_SYM(cuModuleGetFunction);
        LOAD_SYM(cuLaunchKernel);
        LOAD_SYM(cuCtxSynchronize);
        LOAD_SYM(cuGetErrorString);
        #undef LOAD_SYM

        return true;
    }

    ~CudaDriverApi() {
        if (handle) {
            dlclose(handle);
        }
    }
};

void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " [options]\n"
              << "Options:\n"
              << "  --size <N>          Matrix dimension N x N (default: 1024)\n"
              << "  --vram-mb <MB>      Additional VRAM buffer to allocate in MiB (default: 256)\n"
              << "  --duration-ms <MS>  Run benchmark kernel in a loop for MS ms (default: 1500)\n"
              << "  --help              Show this help message\n";
}

} // namespace

int main(int argc, char* argv[]) {
    int size = 1024;
    int vram_mb = 256;
    int duration_ms = 1500;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--size" && i + 1 < argc) {
            size = std::stoi(argv[++i]);
        } else if (arg == "--vram-mb" && i + 1 < argc) {
            vram_mb = std::stoi(argv[++i]);
        } else if (arg == "--duration-ms" && i + 1 < argc) {
            duration_ms = std::stoi(argv[++i]);
        } else if (arg == "--help") {
            print_usage(argv[0]);
            return 0;
        }
    }

    std::cout << ">>> Running Custom CUDA Driver API Matrix Benchmark (SGEMM sm_50+ PTX) <<<\n";
    std::cout << "Configuration: Size=" << size << "x" << size
              << ", Hold VRAM=" << vram_mb << " MiB"
              << ", Duration=" << duration_ms << " ms\n";

    CudaDriverApi api;
    if (!api.load()) {
        std::cerr << "[CUDA_BENCH] Error: Failed to dynamically load libcuda.so.1: "
                  << dlerror() << '\n';
        return 1;
    }

    CUresult res = api.cuInit(0);
    if (res != CUDA_SUCCESS) {
        const char* err_str = "Unknown";
        api.cuGetErrorString(res, &err_str);
        std::cerr << "[CUDA_BENCH] Error: cuInit failed (" << res << "): " << err_str << '\n';
        return 1;
    }

    int dev_count = 0;
    res = api.cuDeviceGetCount(&dev_count);
    if (res != CUDA_SUCCESS || dev_count <= 0) {
        std::cerr << "[CUDA_BENCH] Error: No CUDA-capable devices found.\n";
        return 1;
    }

    CUdevice dev{};
    res = api.cuDeviceGet(&dev, 0);
    if (res != CUDA_SUCCESS) {
        std::cerr << "[CUDA_BENCH] Error: Failed to acquire device 0\n";
        return 1;
    }

    char dev_name[256]{};
    api.cuDeviceGetName(dev_name, sizeof(dev_name) - 1, dev);
    std::cout << "[CUDA_BENCH] Device 0: " << dev_name << "\n";

    CUcontext ctx{nullptr};
    res = api.cuCtxCreate_v2(&ctx, 0, dev);
    if (res != CUDA_SUCCESS || !ctx) {
        std::cerr << "[CUDA_BENCH] Error: cuCtxCreate failed (" << res << ")\n";
        return 1;
    }

    // Allocate additional VRAM buffer to observe distinct telemetry spike in NVML
    CUdeviceptr d_extra{0};
    size_t extra_bytes = static_cast<size_t>(vram_mb) * 1024ULL * 1024ULL;
    if (extra_bytes > 0) {
        res = api.cuMemAlloc_v2(&d_extra, extra_bytes);
        if (res != CUDA_SUCCESS) {
            std::cerr << "[CUDA_BENCH] Warning: Failed to allocate extra VRAM (" << vram_mb << " MiB)\n";
        }
    }

    size_t num_elements = static_cast<size_t>(size) * size;
    size_t matrix_bytes = num_elements * sizeof(float);

    std::vector<float> h_A(num_elements, 1.5f);
    std::vector<float> h_B(num_elements, 2.0f);
    std::vector<float> h_C(num_elements, 0.0f);

    CUdeviceptr d_A{0}, d_B{0}, d_C{0};
    api.cuMemAlloc_v2(&d_A, matrix_bytes);
    api.cuMemAlloc_v2(&d_B, matrix_bytes);
    api.cuMemAlloc_v2(&d_C, matrix_bytes);

    api.cuMemcpyHtoD_v2(d_A, h_A.data(), matrix_bytes);
    api.cuMemcpyHtoD_v2(d_B, h_B.data(), matrix_bytes);

    size_t total_allocated_bytes = extra_bytes + (matrix_bytes * 3);
    double total_vram_mib = static_cast<double>(total_allocated_bytes) / (1024.0 * 1024.0);
    std::cout << "[CUDA_BENCH] Allocated VRAM: " << total_vram_mib << " MiB\n";

    // JIT compile embedded PTX module
    CUmodule mod{nullptr};
    res = api.cuModuleLoadData(&mod, PTX_SOURCE);
    if (res != CUDA_SUCCESS || !mod) {
        const char* err_str = "PTX JIT load failed";
        api.cuGetErrorString(res, &err_str);
        std::cerr << "[CUDA_BENCH] Error: cuModuleLoadData failed (" << res << "): " << err_str << '\n';
        if (d_extra) api.cuMemFree_v2(d_extra);
        api.cuMemFree_v2(d_A); api.cuMemFree_v2(d_B); api.cuMemFree_v2(d_C);
        api.cuCtxDestroy_v2(ctx);
        return 1;
    }

    CUfunction sgemm_func{nullptr};
    res = api.cuModuleGetFunction(&sgemm_func, mod, "sgemm_kernel");
    if (res != CUDA_SUCCESS || !sgemm_func) {
        std::cerr << "[CUDA_BENCH] Error: cuModuleGetFunction failed\n";
        api.cuModuleUnload(mod);
        if (d_extra) api.cuMemFree_v2(d_extra);
        api.cuMemFree_v2(d_A); api.cuMemFree_v2(d_B); api.cuMemFree_v2(d_C);
        api.cuCtxDestroy_v2(ctx);
        return 1;
    }

    uint64_t param_A = static_cast<uint64_t>(d_A);
    uint64_t param_B = static_cast<uint64_t>(d_B);
    uint64_t param_C = static_cast<uint64_t>(d_C);
    uint32_t param_N = static_cast<uint32_t>(size);

    void* kernel_params[] = {
        &param_A,
        &param_B,
        &param_C,
        &param_N
    };

    unsigned int block_dim_x = 256;
    unsigned int grid_dim_x = (static_cast<unsigned int>(num_elements) + block_dim_x - 1) / block_dim_x;

    // Launch loop for requested duration
    auto bench_start = std::chrono::high_resolution_clock::now();
    uint64_t iterations = 0;

    while (true) {
        api.cuLaunchKernel(sgemm_func, grid_dim_x, 1, 1, block_dim_x, 1, 1, 0, nullptr, kernel_params, nullptr);
        api.cuCtxSynchronize();
        iterations++;

        auto now = std::chrono::high_resolution_clock::now();
        double elapsed_ms = std::chrono::duration<double, std::milli>(now - bench_start).count();
        if (elapsed_ms >= duration_ms) break;
    }

    auto bench_end = std::chrono::high_resolution_clock::now();
    double total_sec = std::chrono::duration<double>(bench_end - bench_start).count();

    // 2 FLOPs per element (fma: multiply + add)
    double total_flops = static_cast<double>(iterations) * static_cast<double>(num_elements) * 2.0;
    double gflops = (total_sec > 0.0) ? (total_flops / total_sec / 1e9) : 0.0;
    std::cout << "[CUDA_BENCH] Kernel Execution Throughput: " << gflops << " GFLOP/s ("
              << iterations << " iterations in " << total_sec << " s)\n";

    // Read back and verify output
    api.cuMemcpyDtoH_v2(h_C.data(), d_C, matrix_bytes);

    int errors = 0;
    float expected = 1.5f * 2.0f + 1.0f; // 4.0f
    for (size_t i = 0; i < num_elements; ++i) {
        if (std::abs(h_C[i] - expected) > 1e-3f) {
            if (errors < 5) {
                std::cerr << "[CUDA_BENCH] Verification error at index " << i
                          << ": expected " << expected << ", got " << h_C[i] << '\n';
            }
            errors++;
        }
    }

    std::cout << "[CUDA_BENCH] Verification: " << (errors == 0 ? "PASS (0 errors)" : "FAIL") << "\n";

    // Teardown
    api.cuModuleUnload(mod);
    if (d_extra) api.cuMemFree_v2(d_extra);
    api.cuMemFree_v2(d_A);
    api.cuMemFree_v2(d_B);
    api.cuMemFree_v2(d_C);
    api.cuCtxDestroy_v2(ctx);

    return (errors == 0) ? 0 : 2;
}
