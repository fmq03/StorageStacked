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

import sys


def main():
    width = 320
    height = 240
    filename = "test_pattern_320x240.raw"

    print(f"Generating {filename} ({width}x{height})")

    with open(filename, 'wb') as f:
        bar_width = width // 8

        for y in range(height):
            row_data = bytearray()
            for x in range(width):
                bar_idx = x // bar_width
                val = bar_idx * 36  # 0, 36, 72, ... 252
                row_data.append(val)
            f.write(row_data)


if __name__ == "__main__":
    main()
