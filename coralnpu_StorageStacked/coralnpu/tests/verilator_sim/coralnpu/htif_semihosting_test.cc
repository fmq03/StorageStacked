// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

int main(int argc, char** argv) {
    printf("Starting HTIF Semihosting Test...\n");
    printf("==================================\n");

    const char* filename = "/tmp/htif_test.txt";
    const char* msg = "Hello from CoralNPU V2 via HTIF semihosting!\n";
    char buf[128];

    // These calls use the overrides in toolchain/crt/coralnpu_htif_gloss.cc
    printf("Opening %s for writing...\n", filename);
    int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        printf("Failed to open file for writing: %d\n", fd);
        return 1;
    }

    printf("Writing message...\n");
    write(fd, msg, strlen(msg));
    close(fd);

    printf("Opening %s for reading...\n", filename);
    fd = open(filename, O_RDONLY, 0);
    if (fd < 0) {
        printf("Failed to open file for reading: %d\n", fd);
        return 1;
    }

    memset(buf, 0, sizeof(buf));
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n > 0) {
        printf("Read from file: %s", buf);
    } else {
        printf("Failed to read from file: %ld\n", (long)n);
    }
    close(fd);

    printf("HTIF Semihosting Test Complete.\n");
    return 0;
}
