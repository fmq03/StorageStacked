// gem5 x86 CPU 程序：CPU 多线程准备数据，Vortex 执行 kernel.cpp。
#include "project_config.h"
#include <host_io.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <cstdlib>
#include <thread>

// 主线程也参加：共 HOST_CPU_COUNT 个线程，分片处理真实输入/输出。
// 在 GPU 队列创建之前、销毁之后执行，避免与运行库线程争抢 CPU 上下文。
template<class Function>
static void on_cpu_cores(const char* phase, unsigned count, Function work) {
    pthread_barrier_t start;
    if (pthread_barrier_init(&start, nullptr, HOST_CPU_COUNT)) std::abort();
    std::vector<long> tids(HOST_CPU_COUNT);
    auto run = [&](unsigned worker) {
        tids[worker] = syscall(SYS_gettid);
        pthread_barrier_wait(&start);
        for (unsigned i = count * worker / HOST_CPU_COUNT;
             i < count * (worker + 1) / HOST_CPU_COUNT; ++i) work(i);
    };
    std::vector<std::thread> threads;
    for (unsigned worker = 1; worker < HOST_CPU_COUNT; ++worker)
        threads.emplace_back(run, worker);
    run(0);
    for (auto& thread : threads) thread.join();
    pthread_barrier_destroy(&start);
    for (unsigned worker = 0; worker < HOST_CPU_COUNT; ++worker)
        std::printf("HOST_TASK phase=%s worker=%u tid=%ld begin=%u end=%u\n",
                    phase, worker, tids[worker], count * worker / HOST_CPU_COUNT,
                    count * (worker + 1) / HOST_CPU_COUNT);
}

int main(int argc, char** argv) {
    if (argc != 2) { std::fprintf(stderr, "usage: host.elf program.vxbin\n"); return 2; }
    std::vector<uint32_t> input(APP_WORKGROUPS), expected(APP_WORKGROUPS);
    on_cpu_cores("prepare", APP_WORKGROUPS, [&](unsigned i) {
        input[i] = SMOKE_INPUT;
        expected[i] = input[i] + 1u;
    });

    vx_device_h device = nullptr;
    CHECK(vx_device_open(0, &device));
    vx_queue_info_t info{sizeof(info), nullptr, VX_QUEUE_PRIORITY_NORMAL, 0};
    vx_queue_h queue = nullptr;
    CHECK(vx_queue_create(device, &info, &queue));
    vx_buffer_h buffer = nullptr;
    CHECK(vx_buffer_reserve(device, 0x90000000ull, 0x60000,
                            VX_MEM_READ_WRITE | VX_MEM_PHYS, &buffer));
    // CPU 把输入上传到 GPU 地址 0x90000000；kernel.cpp 从此处读取。
    vx_event_h event = nullptr;
    CHECK(vx_enqueue_write(queue, buffer, 0, input.data(),
                            input.size() * sizeof(uint32_t), 0, nullptr, &event));
    CHECK(wait_event(event));

    // 加载 RV32 设备程序，发射工作组，等待 Vortex 完成。
    vx_module_h module = nullptr;
    vx_kernel_h kernel = nullptr;
    CHECK(vx_module_load_file(device, argv[1], &module));
    CHECK(vx_module_get_kernel(module, "main", &kernel));
    vx_launch_info_t launch{};
    launch.struct_size = sizeof(launch);
    launch.kernel = kernel;
    launch.ndim = 1;
    launch.grid_dim[0] = APP_WORKGROUPS;
    launch.block_dim[0] = 1;
    CHECK(vx_enqueue_launch(queue, &launch, 0, nullptr, &event));
    CHECK(wait_event(event));

    // CPU 经 Vortex 运行库读取真实返回字节，再释放运行库线程。
    Readback returned;
    CHECK(readback_all(queue, buffer, returned));
    CHECK(vx_kernel_release(kernel));
    CHECK(vx_module_release(module));
    CHECK(vx_buffer_release(buffer));
    CHECK(vx_queue_release(queue));
    CHECK(vx_device_dump_perf(device, stdout));
    CHECK(vx_device_release(device));

    std::vector<unsigned> errors(APP_WORKGROUPS);
    on_cpu_cores("check", APP_WORKGROUPS, [&](unsigned i) {
        errors[i] = readback_word(returned.at(0), i) != input[i] ||
                    readback_word(returned.at(0x10000), i) != expected[i];
    });
    unsigned total_errors = 0;
    for (auto error : errors) total_errors += error;
    const auto status = readback_word(returned.at(0x5000c), 0);
    std::printf("HOST_CHECK_ERRORS %u\nDEVICE_STATUS %08x\n", total_errors, status);
    return total_errors == 0 && status == 0x600d0000u ? 0 : 1;
}
