# Copyright 2025 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "memory_util",
    hdrs = ["memory_util.h"],
    target_compatible_with = ["//platforms/cpu:coralnpu_v2"],
    deps = [
        "@tflite_micro//tensorflow/lite/kernels/internal:compatibility",
    ],
)

cc_library(
    name = "accumulator_util",
    hdrs = ["accumulator_util.h"],
    target_compatible_with = ["//platforms/cpu:coralnpu_v2"],
    deps = [
        "@tflite_micro//tensorflow/lite/kernels/internal:compatibility",
    ],
)

cc_library(
    name = "depthwise_conv",
    srcs = [
        "depthwise_conv.cc",
    ],
    hdrs = ["depthwise_conv.h"],
    target_compatible_with = ["//platforms/cpu:coralnpu_v2"],
    deps = [
        ":accumulator_util",
        ":memory_util",
        "//sw/opt:rvv_opt",
        "@tflite_micro//tensorflow/lite/micro:op_resolvers",
    ],
)

cc_library(
    name = "conv",
    srcs = [
        "conv.cc",
    ],
    hdrs = ["conv.h"],
    target_compatible_with = ["//platforms/cpu:coralnpu_v2"],
    deps = [
        ":accumulator_util",
        ":memory_util",
        "//sw/opt:rvv_opt",
        "@tflite_micro//tensorflow/lite/kernels/internal:common",
        "@tflite_micro//tensorflow/lite/micro:op_resolvers",
    ],
)

cc_library(
    name = "pooling",
    srcs = ["pooling.cc"],
    hdrs = ["pooling.h"],
    target_compatible_with = ["//platforms/cpu:coralnpu_v2"],
    deps = [
        "@tflite_micro//tensorflow/lite/kernels/internal:reference_base",
        "@tflite_micro//tensorflow/lite/micro:op_resolvers",
        "@tflite_micro//tensorflow/lite/micro/kernels:micro_ops",
    ],
)

cc_library(
    name = "fully_connected",
    srcs = ["fully_connected.cc"],
    hdrs = ["fully_connected.h"],
    target_compatible_with = ["//platforms/cpu:coralnpu_v2"],
    deps = [
        ":accumulator_util",
        "@tflite_micro//tensorflow/lite/kernels/internal:reference_base",
        "@tflite_micro//tensorflow/lite/micro:op_resolvers",
        "@tflite_micro//tensorflow/lite/micro/kernels:micro_ops",
    ],
)

cc_library(
    name = "logistic",
    srcs = ["logistic.cc"],
    hdrs = ["logistic.h"],
    target_compatible_with = ["//platforms/cpu:coralnpu_v2"],
    deps = [
        "@tflite_micro//tensorflow/lite/kernels/internal:reference_base",
        "@tflite_micro//tensorflow/lite/micro:op_resolvers",
        "@tflite_micro//tensorflow/lite/micro/kernels:micro_ops",
    ],
)
