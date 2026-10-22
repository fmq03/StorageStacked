#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <unistd.h>
#include <vector>
#include <vortex2.h>
#include "common.h"

#define CHECK(expr) do { \
    vx_result_t result = (expr); \
    if (result != VX_SUCCESS) { \
        std::fprintf(stderr, "FAIL: %s: %s\n", #expr, vx_result_string(result)); \
        std::exit(1); \
    } \
} while (0)

int main(int argc, char** argv) {
    uint32_t size = 8;
    const char* kernel_file = "kernel.vxbin";
    int option;
    while ((option = getopt(argc, argv, "n:k:h")) != -1) {
        switch (option) {
        case 'n': size = std::atoi(optarg); break;
        case 'k': kernel_file = optarg; break;
        default:
            std::cout << "Usage: [-n square size] [-k kernel]\n";
            return option == 'h' ? 0 : 1;
        }
    }
    if (size == 0 || size > 16384) {
        std::cerr << "size must be in [1,16384]\n";
        return 1;
    }
    const uint64_t count = uint64_t(size) * size;
    const uint64_t bytes = count * sizeof(float);
    std::vector<float> input(count), output(count);
    for (uint32_t row = 0; row < size; ++row) {
        for (uint32_t col = 0; col < size; ++col) {
            float value;
            switch (row % 4) {
            case 0: value = -1000.0f; break;
            case 1: value = 1000.0f + float(col % 8); break;
            case 2: value = col == 0 ? 80.0f : -80.0f; break;
            default: value = float(int(col % 13) - 6) * 0.75f; break;
            }
            input[uint64_t(row) * size + col] = value;
        }
    }

    vx_device_h device = nullptr;
    CHECK(vx_device_open(0, &device));
    vx_queue_info_t queue_info = {sizeof(queue_info), nullptr, VX_QUEUE_PRIORITY_NORMAL, 0};
    vx_queue_h queue = nullptr;
    CHECK(vx_queue_create(device, &queue_info, &queue));
    vx_buffer_h source = nullptr, destination = nullptr;
    CHECK(vx_buffer_create(device, bytes, VX_MEM_READ_WRITE, &source));
    CHECK(vx_buffer_create(device, bytes, VX_MEM_WRITE, &destination));
    kernel_arg_t args{};
    args.num_rows = size;
    args.num_cols = size;
    CHECK(vx_buffer_address(source, &args.src0_addr));
    CHECK(vx_buffer_address(destination, &args.dst_addr));
    vx_module_h module = nullptr;
    vx_kernel_h kernel = nullptr;
    CHECK(vx_module_load_file(device, kernel_file, &module));
    CHECK(vx_module_get_kernel(module, "main", &kernel));
    CHECK(vx_enqueue_write(queue, source, 0, input.data(), bytes, 0, nullptr, nullptr));

    uint32_t grid[1], block[1];
    CHECK(vx_device_max_occupancy_grid(device, 1, &size, grid, block));
    vx_launch_info_t launch{};
    launch.struct_size = sizeof(launch);
    launch.kernel = kernel;
    launch.args_host = &args;
    launch.args_size = sizeof(args);
    launch.ndim = 1;
    launch.grid_dim[0] = grid[0];
    launch.block_dim[0] = block[0];
    vx_event_h launched = nullptr, returned = nullptr;
    CHECK(vx_enqueue_launch(queue, &launch, 0, nullptr, &launched));
    CHECK(vx_enqueue_read(queue, output.data(), destination, 0, bytes, 1, &launched, &returned));
    CHECK(vx_event_wait_value(returned, 1, VX_TIMEOUT_INFINITE));

    uint64_t errors = 0;
    double max_abs_error = 0, row_sum_error = 0;
    for (uint32_t row = 0; row < size; ++row) {
        const uint64_t offset = uint64_t(row) * size;
        const double maximum = *std::max_element(input.begin() + offset, input.begin() + offset + size);
        double denominator = 0;
        for (uint32_t col = 0; col < size; ++col)
            denominator += std::exp(double(input[offset + col]) - maximum);
        double actual_sum = 0;
        for (uint32_t col = 0; col < size; ++col) {
            const double reference = std::exp(double(input[offset + col]) - maximum) / denominator;
            const double actual = output[offset + col];
            const double error = std::abs(actual - reference);
            max_abs_error = std::max(max_abs_error, error);
            actual_sum += actual;
            if (!std::isfinite(actual) || actual < 0 || actual > 1 ||
                error > 2e-6 + 2e-5 * std::abs(reference)) {
                if (errors < 10)
                    std::printf("mismatch row=%u col=%u expected=%.9g actual=%.9g\n",
                                row, col, reference, actual);
                ++errors;
            }
        }
        const double sum_error = std::abs(actual_sum - 1.0);
        row_sum_error = std::max(row_sum_error, sum_error);
        if (!std::isfinite(actual_sum) || sum_error > 2e-5)
            ++errors;
    }
    std::printf("LLM_CHECK {\"operator\":\"softmax\",\"elements\":%u,"
                "\"checked_outputs\":%llu,\"max_abs_error\":%.9g,"
                "\"row_sum_max_error\":%.9g,\"passed\":%s}\n",
                size, static_cast<unsigned long long>(count),
                max_abs_error, row_sum_error, errors == 0 ? "true" : "false");
    vx_event_release(returned);
    vx_event_release(launched);
    vx_buffer_release(destination);
    vx_buffer_release(source);
    vx_kernel_release(kernel);
    vx_module_release(module);
    vx_queue_release(queue);
    vx_device_dump_perf(device, stdout);
    vx_device_release(device);
    std::cout << (errors == 0 ? "PASSED!\n" : "FAILED!\n");
    return errors == 0 ? 0 : 1;
}
