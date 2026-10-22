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

"""Rules for generating linker scripts."""

def _generate_linker_script_impl(ctx):
    """Implementation for the generate_linker_script rule."""
    output_script = ctx.outputs.out
    template = ctx.file.src

    # Default ITCM size is 8KB, DTCM is 32KB.
    # From //hdl/chisel/src/coralnpu:Parameters.scala
    itcm_size_kbytes_default = 8
    dtcm_size_kbytes_default = 32
    dtcm_origin_default = "0x00010000"
    dtcm_origin_highmem = "0x00100000"
    dtcm_origin = dtcm_origin_default
    if ctx.attr.itcm_size_kbytes != itcm_size_kbytes_default or ctx.attr.dtcm_size_kbytes != dtcm_size_kbytes_default:
        dtcm_origin = dtcm_origin_highmem

    stack_size_bytes_default = 128
    stack_size = ctx.attr.stack_size_bytes if ctx.attr.stack_size_bytes else stack_size_bytes_default

    heap_location = ctx.attr.heap_location
    heap_size = ctx.attr.heap_size

    # Logic to determine heap and stack sizing.
    # If heap is in DTCM and no size is specified, use the "remainder" logic.
    if heap_location == "DTCM" and (not heap_size or heap_size == "MAX"):
        heap_size_spec = ". = ORIGIN(DTCM) + LENGTH(DTCM) - STACK_SIZE;"
        stack_start_spec = ""  # Just follows heap
    else:
        heap_size_spec = ". += {};".format(heap_size if heap_size else "1K")

        # Stack stays in DTCM, force it to the end to maximize space.
        stack_start_spec = ". = ORIGIN(DTCM) + LENGTH(DTCM) - STACK_SIZE;"

    substitutions = {
        "@@ITCM_LENGTH@@": str(ctx.attr.itcm_size_kbytes),
        "@@DTCM_LENGTH@@": str(ctx.attr.dtcm_size_kbytes),
        "@@DTCM_ORIGIN@@": dtcm_origin,
        "@@STACK_SIZE@@": str(stack_size),
        "@@HEAP_LOCATION@@": heap_location,
        "@@HEAP_SIZE_SPEC@@": heap_size_spec,
        "@@STACK_START_SPEC@@": stack_start_spec,
    }

    ctx.actions.expand_template(
        template = template,
        output = output_script,
        substitutions = substitutions,
    )

generate_linker_script = rule(
    implementation = _generate_linker_script_impl,
    attrs = {
        "src": attr.label(mandatory = True, allow_single_file = True),
        "out": attr.output(mandatory = True),
        "itcm_size_kbytes": attr.int(mandatory = True),
        "dtcm_size_kbytes": attr.int(mandatory = True),
        "stack_size_bytes": attr.int(default = 128),
        "heap_size": attr.string(default = ""),
        "heap_location": attr.string(default = "DTCM"),
    },
)
