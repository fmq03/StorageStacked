// 单层字符 Transformer；全部计算由 Vortex 执行。
// 权重、KV cache、token 和观测值通过外部 AXI / UCIe / MEMSIM 链路访问。
#include "tinyllm.h"
#include "project_config.h"
#include "llm_parallel.h"
#include <device_math.h>
#include <vx_spawn2.h>
#include <cstdint>

static volatile float *const weights = reinterpret_cast<volatile float *>(0x90000000u);
static volatile float *const keys = reinterpret_cast<volatile float *>(0x90010000u);
static volatile float *const values = reinterpret_cast<volatile float *>(0x90020000u);
static volatile float *const trace = reinterpret_cast<volatile float *>(0x90030000u);
static volatile uint32_t *const tokens = reinterpret_cast<volatile uint32_t *>(0x90040000u);
static volatile uint32_t *const report = reinterpret_cast<volatile uint32_t *>(0x90050000u);

// 显式控制线程分歧；串行阶段结束后恢复工作组的活动线程。
template <class Function> static inline void on_leader(bool leader, Function work) {
    const int mask = vx_split(leader);
    if (leader)
        work();
    vx_join(mask);
}

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
    const float inv = 1.0f / device_sqrt(variance / LLM_DIM + 1e-5f);
    for (unsigned i = 0; i < LLM_DIM; ++i)
        y[i] = (x[i] - mean) * inv * weights[scale + i] + weights[bias + i];
}
// 每个线程独立计算输出列，保持每列的 FP32 累加顺序。
static void linear(LlmScratch &s, const float *x, float *y, unsigned rows, unsigned cols,
                   unsigned matrix, int bias = -1) {
    const unsigned tid = unsigned(threadIdx.x);
    on_leader(tid == 0, [&] { ++s.linear_calls; });
    for (unsigned base = 0; base < cols; base += unsigned(blockDim.x)) {
        const unsigned j = base + tid;
        const bool valid = j < cols;
        const int mask = vx_split(valid);
        if (valid) {
            float sum = 0.0f;
            for (unsigned i = 0; i < rows; ++i)
                sum += x[i] * weights[matrix + i * cols + j];
            y[j] = sum + (bias >= 0 ? weights[bias + j] : 0.0f);
            ++s.column_counts[tid];
        }
        vx_join(mask);
    }
    __syncthreads();
}
// 处理一个 token 位置：Embedding → 注意力 → FFN → 输出分数。
static unsigned forward(LlmScratch &s, unsigned pos) {
    const bool leader = unsigned(threadIdx.x) == 0;
    auto &x = s.x;
    auto &n1 = s.n1;
    auto &q = s.q;
    auto &k = s.k;
    auto &v = s.v;
    auto &mixed = s.mixed;
    auto &projected = s.projected;
    auto &residual = s.residual;
    auto &n2 = s.n2;
    auto &ff = s.ff;
    auto &hidden = s.hidden;
    auto &nf = s.nf;
    auto &logits = s.logits;
    auto &probs = s.probs;
    const unsigned id = tokens[pos];
    if (id >= LLM_VOCAB) {
        on_leader(leader, [&] { ++s.errors; });
        return 0;
    }
    on_leader(leader, [&] {
        ++s.calls;
        for (unsigned i = 0; i < LLM_HEADS * LLM_CONTEXT; ++i)
            probs[i] = 0.0f;
        for (unsigned j = 0; j < LLM_DIM; ++j)
            x[j] =
                weights[W_EMBEDDING + id * LLM_DIM + j] + weights[W_POSITION + pos * LLM_DIM + j];
        dump(pos, T_EMBEDDING, x, LLM_DIM);
        norm(x, n1, W_NORM1_WEIGHT, W_NORM1_BIAS);
        dump(pos, T_NORM1, n1, LLM_DIM);
    });
    __syncthreads();
    linear(s, n1, q, LLM_DIM, LLM_DIM, W_Q);
    linear(s, n1, k, LLM_DIM, LLM_DIM, W_K);
    linear(s, n1, v, LLM_DIM, LLM_DIM, W_V);
    on_leader(leader, [&] {
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
                score /= device_sqrt(float(head_dim));
                probs[h * LLM_CONTEXT + t] = score;
                if (score > peak)
                    peak = score;
            }
            for (unsigned t = 0; t <= pos; ++t) {
                float p = device_exp(probs[h * LLM_CONTEXT + t] - peak);
                probs[h * LLM_CONTEXT + t] = p;
                total += p;
            }
            float check = 0.0f;
            for (unsigned t = 0; t <= pos; ++t) {
                probs[h * LLM_CONTEXT + t] /= total;
                check += probs[h * LLM_CONTEXT + t];
            }
            if (!(check > 0.999f && check < 1.001f))
                ++s.errors;
            for (unsigned j = 0; j < head_dim; ++j) {
                float sum = 0.0f;
                for (unsigned t = 0; t <= pos; ++t)
                    sum += probs[h * LLM_CONTEXT + t] * values[t * LLM_DIM + h * head_dim + j];
                mixed[h * head_dim + j] = sum;
            }
        }
        dump(pos, T_ATTENTION, probs, LLM_HEADS * LLM_CONTEXT);
    });
    __syncthreads();
    linear(s, mixed, projected, LLM_DIM, LLM_DIM, W_O);
    on_leader(leader, [&] {
        dump(pos, T_ATTENTION_OUTPUT, projected, LLM_DIM);
        for (unsigned j = 0; j < LLM_DIM; ++j)
            residual[j] = x[j] + projected[j];
        dump(pos, T_RESIDUAL, residual, LLM_DIM);
        norm(residual, n2, W_NORM2_WEIGHT, W_NORM2_BIAS);
        dump(pos, T_NORM2, n2, LLM_DIM);
    });
    __syncthreads();
    linear(s, n2, ff, LLM_DIM, LLM_HIDDEN, W_FF1, W_FF1_BIAS);
    on_leader(leader, [&] {
        for (unsigned j = 0; j < LLM_HIDDEN; ++j)
            if (ff[j] < 0.0f)
                ff[j] = 0.0f;
        dump(pos, T_FF, ff, LLM_HIDDEN);
    });
    __syncthreads();
    linear(s, ff, hidden, LLM_HIDDEN, LLM_DIM, W_FF2, W_FF2_BIAS);
    on_leader(leader, [&] {
        for (unsigned j = 0; j < LLM_DIM; ++j)
            hidden[j] += residual[j];
        dump(pos, T_HIDDEN, hidden, LLM_DIM);
        norm(hidden, nf, W_NORM_FINAL_WEIGHT, W_NORM_FINAL_BIAS);
        dump(pos, T_NORM_FINAL, nf, LLM_DIM);
    });
    __syncthreads();
    linear(s, nf, logits, LLM_DIM, LLM_VOCAB, W_LM_HEAD, W_LM_BIAS);
    on_leader(leader, [&] {
        dump(pos, T_LOGITS, logits, LLM_VOCAB);
        // 贪心选择：返回分数最大的字符编号。
        unsigned best = 0;
        for (unsigned j = 0; j < LLM_VOCAB; ++j) {
            if (!(logits[j] > -1e10f && logits[j] < 1e10f))
                ++s.errors;
            if (logits[j] > logits[best])
                best = j;
        }
        s.best = best;
    });
    __syncthreads();
    return s.best;
}
__kernel void kernel_main() {
    auto &s = *reinterpret_cast<LlmScratch *>(__local_mem());
    const unsigned tid = unsigned(threadIdx.x);
    const bool leader = tid == 0;
    s.column_counts[tid] = 0;
    on_leader(leader, [&] {
        s.errors = s.calls = s.linear_calls = s.best = 0;
        report[0] = 0x4c4c4d31u;
        for (unsigned i = 0; i < LLM_CONTEXT * LLM_DIM; ++i) {
            keys[i] = 0.0f;
            values[i] = 0.0f;
        }
        report[1] = 1;
    });
    __syncthreads();
    for (unsigned step = 0; step < LLM_GENERATE; ++step) {
        on_leader(leader, [&] { report[32 + step] = 0x70000000u + step; });
        const unsigned end = LLM_PROMPT_LENGTH + step;
        unsigned best = 0;
        const unsigned begin = LLM_KV_CACHE && step ? end - 1 : 0;
        for (unsigned pos = begin; pos < end; ++pos)
            best = forward(s, pos);
        on_leader(leader, [&] {
            report[16 + step] = best;
            const unsigned returned = report[16 + step];
            if (returned != best)
                ++s.errors;
            if (step + 1 < LLM_GENERATE)
                tokens[end] = returned;
            report[96 + step] = s.calls;
            report[64 + step] = 0x71000000u + step;
        });
        __syncthreads();
    }
    report[128 + tid] = s.column_counts[tid];
    __syncthreads();
    on_leader(leader, [&] {
        unsigned active = 0;
        for (unsigned i = 0; i < unsigned(blockDim.x); ++i)
            active += s.column_counts[i] > 0;
        report[8] = s.errors;
        report[9] = s.linear_calls;
        report[10] = blockDim.x;
        report[11] = active;
        report[12] = get_num_sub_groups();
        report[13] = vx_core_id();
        report[2] = 2;
        report[3] = s.errors ? 0xbad00000u : 0x600d0000u;
    });
}
