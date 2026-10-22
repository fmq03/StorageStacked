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

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "crt",
    srcs = [
        "coralnpu_exceptions.cc",
        "coralnpu_gloss.cc",
        "coralnpu_start.S",
        "crt.S",
        "cxx_guards.cc",
    ],
    copts = ["-DSKIP_HTIF_SYMBOLS"],
    target_compatible_with = [
        "//platforms/cpu:coralnpu_v2",
    ],
    alwayslink = True,
)

cc_library(
    name = "crt_semihosting",
    srcs = [
        "coralnpu_exceptions.cc",
        "coralnpu_htif_gloss.cc",
        "coralnpu_start.S",
        "crt.S",
    ],
    target_compatible_with = [
        "//platforms/cpu:coralnpu_v2",
    ],
    alwayslink = True,
)
