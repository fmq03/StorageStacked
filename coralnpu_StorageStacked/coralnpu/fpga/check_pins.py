#!/usr/bin/env python3
# Copyright 2026 Google LLC

import argparse
import sys
import re


def parse_verilog_ports(sv_file):
    ports = {}
    # Regex to match input/output/inout
    # Group 1: direction
    # Group 2: array range (optional)
    # Group 3: name
    regex = re.compile(
        r'^\s*(input|output|inout)\s+(?:logic|reg|wire)?\s*(?:\[([^\]]+)\])?\s*([a-zA-Z0-9_]+)\s*,?\s*'
    )

    with open(sv_file, 'r') as f:
        for line in f:
            # Strip comments
            line = line.split('//')[0]
            match = regex.match(line)
            if match:
                direction = match.group(1)
                width_range = match.group(2)
                name = match.group(3)

                # Parse width
                width = 1
                if width_range:
                    parts = width_range.split(':')
                    if len(parts) == 2:
                        try:
                            h = int(parts[0].strip())
                            l = int(parts[1].strip())
                            width = abs(h - l) + 1
                        except ValueError:
                            # Parametric range (e.g. PARAM-1:0)
                            # We reset to 1 to trigger the fallback logic in main()
                            width = 1

                ports[name] = {
                    'direction': direction,
                    'width': width,
                    'range': width_range
                }
    return ports


def parse_xdc_mappings(xdc_file):
    mappings = set()
    # Updated regex to support:
    # - Optional spaces after [
    # - Comma or space separated multiple ports
    regex = re.compile(
        r'\[\s*get_ports\s+[\{\"]?\s*([a-zA-Z0-9_\[\],\s]+)\s*[\}\"]?\s*\]'
    )

    with open(xdc_file, 'r') as f:
        for line in f:
            if '#' in line:
                line = line.split('#')[0]
            matches = regex.findall(line)
            for m in matches:
                # Split by space or comma if multiple ports listed
                for p in re.split(r'[\s,]+', m):
                    if p:
                        mappings.add(p.strip())
    return mappings


def main():
    parser = argparse.ArgumentParser(
        description="Verify top-level SystemVerilog ports are mapped in XDC."
    )
    parser.add_argument(
        "--xdc",
        action="append",
        required=True,
        help="Path to XDC file(s). Can use multiple times."
    )
    parser.add_argument(
        "--sv", required=True, help="Path to top-level SystemVerilog file."
    )

    args = parser.parse_args()

    sv_file = args.sv
    xdc_files = args.xdc

    ports = parse_verilog_ports(sv_file)

    mappings = set()
    for xdc in xdc_files:
        mappings.update(parse_xdc_mappings(xdc))

    # Auto-detect if DDR mappings were provided in any XDC
    has_ddr_mappings = any(m.startswith("c0_ddr4_") for m in mappings)
    should_ignore_ddr = not has_ddr_mappings

    if should_ignore_ddr:
        print(
            "===================================================================================================="
        )
        print(
            "PIN CHECKER: DDR pin check is DISABLED (no DDR4 mappings found in XDC or forced ignore)"
        )
        print(
            "===================================================================================================="
        )
    else:
        print(
            "===================================================================================================="
        )
        print(
            "PIN CHECKER: DDR pin check is ENABLED (DDR4 mappings found in XDC)"
        )
        print(
            "===================================================================================================="
        )

    missing = []

    for name, info in ports.items():
        if should_ignore_ddr and (name.startswith("c0_ddr4_") or name
                                  == "c0_sys_clk_n" or name == "c0_sys_clk_p"):
            continue

        if info['width'] > 1:
            if info['range']:
                parts = info['range'].split(':')
                if len(parts) == 2:
                    try:
                        h = int(parts[0].strip())
                        l = int(parts[1].strip())
                        step = 1 if h >= l else -1
                        for i in range(l, h + step, step):
                            bit_name = f"{name}[{i}]"
                            if bit_name not in mappings:
                                missing.append(bit_name)
                    except ValueError:
                        print(
                            f"Warning: Parametric range for vector {name}: {info['range']}. Doing fallback prefix check."
                        )
                        found_base = False
                        for m in mappings:
                            if m.startswith(name):
                                found_base = True
                                break
                        if not found_base:
                            missing.append(name)
        else:
            # Fallback for vectors without a parsed range (assume it might be a vector and check if XDC has any mapping starting with name)
            if name not in mappings:
                found_base = False
                for m in mappings:
                    if m.startswith(name):
                        found_base = True
                        break
                if not found_base:
                    missing.append(name)

    if missing:
        print("ERROR: Missing pin mappings for some top-level ports:")
        for m in missing:
            print(f"  - {m}")
        sys.exit(1)
    else:
        print("All top-level ports are mapped to pins!")
        sys.exit(0)


if __name__ == "__main__":
    main()
