// 单层字符 Transformer；全部计算由 CoralNPU 执行。
// 权重、KV cache、token 和观测值通过外部 AXI / UCIe / MEMSIM 链路访问。
#include "tinyllm.h"
#include "project_config.h"
#include <cmath>
#include <cstdint>
#include <riscv_vector.h>

static volatile float *const weights = reinterpret_cast<volatile float *>(0x90000000u);
static volatile float *const keys = reinterpret_cast<volatile float *>(0x90010000u);
static volatile float *const values = reinterpret_cast<volatile float *>(0x90020000u);
static volatile float *const trace = reinterpret_cast<volatile float *>(0x90030000u);
static volatile uint32_t *const tokens = reinterpret_cast<volatile uint32_t *>(0x90040000u);
static volatile uint32_t *const report = reinterpret_cast<volatile uint32_t *>(0x90050000u);
static uint32_t errors = 0, calls = 0;
static uint32_t rvv_linear_calls = 0;

// 保存中间结果，供主机上的独立参考实现核对。
static void dump(unsigned pos, unsigned offset, const float *data, unsigned count) {
    for (unsigned i = 0; i < count; ++i)
        trace[pos * LLM_TRACE_STRIDE + offset + i] = data[i];
}
// LayerNorm：求均值和方差，再应用缩放与偏置。
static void norm(const float *x, float *y, unsigned scale, unsigned bias) {
    float mean = 0.0f, variance = 0.0f;
    for (unsigned i = 0; i < LLM_DIM; ++i)
        mean += x[i];
    mean /= LLM_DIM;
    for (unsigned i = 0; i < LLM_DIM; ++i) {
        float z = x[i] - mean;
        variance += z * z;
    }
    const float inv = 1.0f / sqrtf(variance / LLM_DIM + 1e-5f);
    for (unsigned i = 0; i < LLM_DIM; ++i)
        y[i] = (x[i] - mean) * inv * weights[scale + i] + weights[bias + i];
}
// RVV FP32 线性层：每个 lane 计算一个输出列，按输入行顺序累加。
// 乘法和加法分开执行，保留独立参考的 FP32 舍入顺序。
static void linear(const float *x, float *y, unsigned rows, unsigned cols, unsigned matrix,
                   int bias = -1) {
    ++rvv_linear_calls;
    for (unsigned j = 0; j < cols;) {
        const size_t vl = __riscv_vsetvl_e32m1(cols - j);
        auto sum = __riscv_vfmv_v_f_f32m1(0.0f, vl);
        for (unsigned i = 0; i < rows; ++i) {
            const auto row = __riscv_vle32_v_f32m1(
                const_cast<const float *>(weights + matrix + i * cols + j), vl);
            const auto product = __riscv_vfmul_vf_f32m1(row, x[i], vl);
            sum = __riscv_vfadd_vv_f32m1(sum, product, vl);
        }
        if (bias >= 0) {
            const auto offset = __riscv_vle32_v_f32m1(
                const_cast<const float *>(weights + bias + j), vl);
            sum = __riscv_vfadd_vv_f32m1(sum, offset, vl);
        }
        __riscv_vse32_v_f32m1(y + j, sum, vl);
        j += vl;
    }
}
// 处理一个 token 位置：Embedding → 注意力 → FFN → 输出分数。
static unsigned forward(unsigned pos) {
    ++calls;
    float x[LLM_DIM], n1[LLM_DIM], q[LLM_DIM], k[LLM_DIM], v[LLM_DIM];
    float mixed[LLM_DIM], projected[LLM_DIM], residual[LLM_DIM], n2[LLM_DIM];
    float ff[LLM_HIDDEN], hidden[LLM_DIM], nf[LLM_DIM], logits[LLM_VOCAB];
    float probs[LLM_HEADS * LLM_CONTEXT] = {};
    const unsigned id = tokens[pos];
    if (id >= LLM_VOCAB) {
        ++errors;
        return 0;
    }
    for (unsigned j = 0; j < LLM_DIM; ++j)
        x[j] = weights[W_EMBEDDING + id * LLM_DIM + j] + weights[W_POSITION + pos * LLM_DIM + j];
    dump(pos, T_EMBEDDING, x, LLM_DIM);
    norm(x, n1, W_NORM1_WEIGHT, W_NORM1_BIAS);
    dump(pos, T_NORM1, n1, LLM_DIM);
    linear(n1, q, LLM_DIM, LLM_DIM, W_Q);
    linear(n1, k, LLM_DIM, LLM_DIM, W_K);
    linear(n1, v, LLM_DIM, LLM_DIM, W_V);
    dump(pos, T_Q, q, LLM_DIM);
    dump(pos, T_K, k, LLM_DIM);
    dump(pos, T_V, v, LLM_DIM);
    for (unsigned j = 0; j < LLM_DIM; ++j) {
        keys[pos * LLM_DIM + j] = k[j];
        values[pos * LLM_DIM + j] = v[j];
    }
    // 因果注意力只访问当前位置及之前的 K/V，不读取未来 token。
    constexpr unsigned head_dim = LLM_DIM / LLM_HEADS;
    for (unsigned h = 0; h < LLM_HEADS; ++h) {
        float peak = -1e30f, total = 0.0f;
        for (unsigned t = 0; t <= pos; ++t) {
            float score = 0.0f;
            for (unsigned j = 0; j < head_dim; ++j)
                score += q[h * head_dim + j] * keys[t * LLM_DIM + h * head_dim + j];
            score /= sqrtf(float(head_dim));
            probs[h * LLM_CONTEXT + t] = score;
            if (score > peak)
                peak = score;
        }
        for (unsigned t = 0; t <= pos; ++t) {
            float p = expf(probs[h * LLM_CONTEXT + t] - peak);
            probs[h * LLM_CONTEXT + t] = p;
            total += p;
        }
        float check = 0.0f;
        for (unsigned t = 0; t <= pos; ++t) {
            probs[h * LLM_CONTEXT + t] /= total;
            check += probs[h * LLM_CONTEXT + t];
        }
        if (!(check > 0.999f && check < 1.001f))
            ++errors;
        for (unsigned j = 0; j < head_dim; ++j) {
            float sum = 0.0f;
            for (unsigned t = 0; t <= pos; ++t)
                sum += probs[h * LLM_CONTEXT + t] * values[t * LLM_DIM + h * head_dim + j];
            mixed[h * head_dim + j] = sum;
        }
    }
    dump(pos, T_ATTENTION, probs, LLM_HEADS * LLM_CONTEXT);
    linear(mixed, projected, LLM_DIM, LLM_DIM, W_O);
    dump(pos, T_ATTENTION_OUTPUT, projected, LLM_DIM);
    for (unsigned j = 0; j < LLM_DIM; ++j)
        residual[j] = x[j] + projected[j];
    dump(pos, T_RESIDUAL, residual, LLM_DIM);
    norm(residual, n2, W_NORM2_WEIGHT, W_NORM2_BIAS);
    dump(pos, T_NORM2, n2, LLM_DIM);
    linear(n2, ff, LLM_DIM, LLM_HIDDEN, W_FF1, W_FF1_BIAS);
    for (unsigned j = 0; j < LLM_HIDDEN; ++j)
        if (ff[j] < 0.0f)
            ff[j] = 0.0f;
    dump(pos, T_FF, ff, LLM_HIDDEN);
    linear(ff, hidden, LLM_HIDDEN, LLM_DIM, W_FF2, W_FF2_BIAS);
    for (unsigned j = 0; j < LLM_DIM; ++j)
        hidden[j] += residual[j];
    dump(pos, T_HIDDEN, hidden, LLM_DIM);
    norm(hidden, nf, W_NORM_FINAL_WEIGHT, W_NORM_FINAL_BIAS);
    dump(pos, T_NORM_FINAL, nf, LLM_DIM);
    linear(nf, logits, LLM_DIM, LLM_VOCAB, W_LM_HEAD, W_LM_BIAS);
    dump(pos, T_LOGITS, logits, LLM_VOCAB);
    // 贪心选择：返回分数最大的字符编号。
    unsigned best = 0;
    for (unsigned j = 0; j < LLM_VOCAB; ++j) {
        if (!(logits[j] > -1e10f && logits[j] < 1e10f))
            ++errors;
        if (logits[j] > logits[best])
            best = j;
    }
    return best;
}
int main() {
    // 1. 将模型与输入写入外部内存，并初始化 KV cache。
    report[0] = 0x4c4c4d31u;
    for (unsigned i = 0; i < LLM_WEIGHT_WORDS; ++i)
        weights[i] = model_image[i];
    for (unsigned i = 0; i < LLM_CONTEXT * LLM_DIM; ++i) {
        keys[i] = 0.0f;
        values[i] = 0.0f;
    }
    for (unsigned i = 0; i < LLM_PROMPT_LENGTH; ++i)
        tokens[i] = prompt_image[i];
    report[1] = 1;
    // 2. 第一轮处理整个 prompt（prefill），后续按 KV cache 开关选择计算范围。
    for (unsigned step = 0; step < LLM_GENERATE; ++step) {
        report[32 + step] = 0x70000000u + step;
        const unsigned end = LLM_PROMPT_LENGTH + step;
        unsigned best = 0;
        const unsigned begin = LLM_KV_CACHE && step ? end - 1 : 0;
        for (unsigned pos = begin; pos < end; ++pos)
            best = forward(pos);
        report[16 + step] = best;
        const unsigned returned = report[16 + step];
        if (returned != best)
            ++errors;
        if (step + 1 < LLM_GENERATE)
            tokens[end] = returned;
        report[96 + step] = calls;
        report[64 + step] = 0x71000000u + step;
    }
    // 3. 写入完成状态；结果和完整链路仍由独立校验器验收。
    report[8] = errors;
    // 记录实际执行的 RVV 线性层次数，供独立校验器核对。
    report[9] = rvv_linear_calls;
    report[2] = 2;
    *reinterpret_cast<volatile uint32_t *>(0xc0000000u) = errors ? 0xbad00000u : 0x600d0000u;
    asm volatile("wfi");
    return 0;
}
