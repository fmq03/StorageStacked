#pragma once
#include "tinyllm.h"

struct LlmScratch {
    float x[LLM_DIM], n1[LLM_DIM], q[LLM_DIM], k[LLM_DIM], v[LLM_DIM];
    float mixed[LLM_DIM], projected[LLM_DIM], residual[LLM_DIM], n2[LLM_DIM];
    float ff[LLM_HIDDEN], hidden[LLM_DIM], nf[LLM_DIM], logits[LLM_VOCAB];
    float probs[LLM_HEADS * LLM_CONTEXT];
    unsigned errors, calls, best, linear_calls;
    unsigned column_counts[LLM_DIM];
};
