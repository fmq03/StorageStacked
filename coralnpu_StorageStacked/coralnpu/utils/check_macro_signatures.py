#!/usr/bin/env python3
# Copyright 2026 Google LLC
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
"""Checks that vcs_cocotb_test and vcs_simulation_split_test signatures match."""

import ast
import os
import sys


def get_function_node(tree, name):
    for node in ast.walk(tree):
        if isinstance(node, ast.FunctionDef) and node.name == name:
            return node
    return None


def analyze_function(node):
    if not node:
        return None

    # Extract formal arguments and defaults
    args = [arg.arg for arg in node.args.args]
    defaults = []
    diff_len = len(args) - len(node.args.defaults)
    for default in node.args.defaults:
        defaults.append(ast.unparse(default))

    formal_args = {}
    for i in range(len(args)):
        if i >= diff_len:
            formal_args[args[i]] = defaults[i - diff_len]
        else:
            formal_args[args[i]] = None

    # Extract arguments popped or checked in kwargs
    popped_keys = set()
    got_keys = set()
    for child in ast.walk(node):
        if isinstance(child, ast.Call):
            if isinstance(child.func, ast.Attribute):
                if (isinstance(child.func.value, ast.Name)
                        and child.func.value.id == "kwargs"):
                    method = child.func.attr
                    if method in ("pop", "get"):
                        if child.args and isinstance(child.args[0],
                                                     ast.Constant):
                            key = child.args[0].value
                            if method == "pop":
                                popped_keys.add(key)
                            elif method == "get":
                                got_keys.add(key)

    return {
        "formal": formal_args,
        "popped": popped_keys,
        "got": got_keys,
    }


def main():
    script_dir = os.path.dirname(os.path.realpath(__file__))
    bzl_path = os.path.abspath(
        os.path.join(script_dir, "../rules/coco_tb.bzl")
    )

    if not os.path.exists(bzl_path):
        print(f"Error: coco_tb.bzl not found at {bzl_path}")
        sys.exit(1)

    with open(bzl_path, "r") as f:
        content = f.read()

    tree = ast.parse(content, filename=bzl_path)

    split_info = analyze_function(
        get_function_node(tree, "vcs_simulation_split_test")
    )
    reg_info = analyze_function(get_function_node(tree, "vcs_cocotb_test"))

    if not split_info or not reg_info:
        print("Error: Could not find function definitions in coco_tb.bzl")
        sys.exit(1)

    split_all = (
        set(split_info["formal"].keys())
        | split_info["popped"]
        | split_info["got"]
    )
    reg_all = (
        set(reg_info["formal"].keys()) | reg_info["popped"] | reg_info["got"]
    )

    # Remove standard kwargs and standard Bazel attributes
    split_all.discard("kwargs")
    reg_all.discard("kwargs")
    standard_attributes = {
        "visibility", "deprecation", "testonly", "tags", "licenses",
        "features", "timeout", "flaky", "size", "shard_count", "local", "args"
    }
    for attr in standard_attributes:
        split_all.discard(attr)
        reg_all.discard(attr)

    # Allowed exceptions (arguments unique to split flow by design)
    allowed_split_only = {"waves", "seed", "testcase", "test_args", "defines"}
    # Allowed exceptions (arguments unique to regular flow by design).
    # Currently empty as all compilation/synthesis attributes are popped in both macros.
    allowed_reg_only = set()

    actual_split_only = split_all - reg_all
    actual_reg_only = reg_all - split_all

    unexpected_split_only = actual_split_only - allowed_split_only
    unexpected_reg_only = actual_reg_only - allowed_reg_only

    errors = []
    if unexpected_split_only:
        errors.append(
            f"Arguments only in split flow (not allowed): {unexpected_split_only}"
        )
    if unexpected_reg_only:
        errors.append(
            f"Arguments only in regular flow (not allowed): {unexpected_reg_only}"
        )

    if errors:
        print("FAIL: Macro signature divergence detected!")
        for err in errors:
            print("  - " + err)
        print(
            "\nIf these differences are intentional, update the allowed lists in check_macro_signatures.py."
        )
        sys.exit(1)

    print("PASS: Macro signatures are in sync.")
    sys.exit(0)


if __name__ == "__main__":
    main()
