# Copyright 2024 Google LLC
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

"""Verilog packaging rules"""

load("@rules_hdl//verilog:providers.bzl", "VerilogInfo", "verilog_library")

def collect_verilog_files(targets, files = None):
    """Collects Verilog files transitively from targets and direct files.

    Args:
        targets: A target or list of targets.
        files: A list of Files.

    Returns:
        A depset of Files.
    """
    if files == None:
        files = []
    if type(targets) != "list":
        targets = [targets]

    transitive_dags = []
    raw_files_depsets = []
    for target in targets:
        if VerilogInfo in target:
            transitive_dags.append(target[VerilogInfo].dag)
        else:
            raw_files_depsets.append(target.files)

    transitive_srcs = depset([], transitive = transitive_dags)

    flat_srcs = []
    for verilog_info_struct in transitive_srcs.to_list():
        flat_srcs.extend(verilog_info_struct.srcs)

    if raw_files_depsets:
        flat_srcs.extend(depset(transitive = raw_files_depsets).to_list())

    flat_srcs.extend(files)

    return depset(flat_srcs)

def _verilog_zip_bundle_impl(ctx):
    # Gather all sources
    all_srcs = collect_verilog_files(ctx.attr.lib).to_list()

    # Build up zip command
    zipper_args = ["cf", ctx.outputs.zip.path]
    for f in all_srcs:
        zipper_args.append(f.path)

    # Run zip command.
    ctx.actions.run(
        inputs = all_srcs,
        outputs = [ctx.outputs.zip],
        executable = ctx.executable._zipper,
        arguments = zipper_args,
        progress_message = "Creating zip...",
        mnemonic = "zipper",
    )

verilog_zip_bundle = rule(
    implementation = _verilog_zip_bundle_impl,
    attrs = {
        "lib": attr.label(
            doc = "The verilog_library to bundle.",
            providers = [VerilogInfo],
        ),
        "_zipper": attr.label(
            default = Label("@bazel_tools//tools/zip:zipper"),
            cfg = "host",
            executable = True,
        ),
    },
    outputs = {
        "zip": "%{name}.zip",
    },
)
