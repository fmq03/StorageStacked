#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>

#define SYS_openat 56
#define SYS_close 57
#define SYS_lseek 62
#define SYS_read 63
#define SYS_write 64
#define SYS_fstat 80
#define SYS_exit 93
#define SYS_getmainvars 2011

#define AT_FDCWD -100

// HTIF symbols.
// Use 64-byte alignment to match common HTIF implementations and ensure
// they fall on separate cache lines/bursts if needed.
extern "C" {
volatile uint64_t tohost __attribute__((section(".htif"), aligned(64), used)) =
    0;
volatile uint64_t fromhost
    __attribute__((section(".htif"), aligned(64), used)) = 0;
volatile uint64_t tohost_ready
    __attribute__((section(".htif"), aligned(64), used)) = 0;
volatile uint64_t fromhost_ready
    __attribute__((section(".htif"), aligned(64), used)) = 0;

long htif_syscall_6args(long n, long a0, long a1, long a2, long a3, long a4,
                        long a5) {
  volatile uint64_t buf[8];
  buf[0] = n;
  buf[1] = a0;
  buf[2] = a1;
  buf[3] = a2;
  buf[4] = a3;
  buf[5] = a4;
  buf[6] = a5;
  buf[7] = 0;

  // HTIF protocol: write packet address to tohost.
  // For RV32, we write the 32-bit address.
  // The testbench should read it as a 64-bit value.

  // Memory fence to ensure buf is written before tohost.
  asm volatile("fence rw, w" ::: "memory");

  tohost = (uintptr_t)buf;

  // For compatibility with systems requiring a ready signal (like MPACT),
  // write to tohost_ready after setting the payload in tohost.
  asm volatile("fence rw, w" ::: "memory");
  tohost_ready = 1;

  // Poll for completion. We check both fromhost (standard Spike/HTIF)
  // and fromhost_ready (MPACT-style) for broad compatibility.
  while (fromhost == 0 && fromhost_ready == 0);
  fromhost = 0;
  fromhost_ready = 0;

  asm volatile("fence r, rw" ::: "memory");
  return buf[0];
}

__attribute__((weak)) int _open(const char* name, int flags, int mode) {
  uintptr_t name_addr = (uintptr_t)name;
  size_t name_len = strlen(name) + 1;

  // Translate newlib flags to Linux host flags.
  // This is required because HTIF (mpact-riscv) passes flags directly to the
  // host's openat().
  int host_flags =
      flags & 3;  // O_RDONLY, O_WRONLY, O_RDWR are the same (0, 1, 2)
  if (flags & 0x0008) host_flags |= 0x400;  // newlib O_APPEND -> linux O_APPEND
  if (flags & 0x0200) host_flags |= 0x40;   // newlib O_CREAT  -> linux O_CREAT
  if (flags & 0x0400) host_flags |= 0x200;  // newlib O_TRUNC  -> linux O_TRUNC
  if (flags & 0x0800) host_flags |= 0x80;   // newlib O_EXCL   -> linux O_EXCL
  // Note: Other flags might need translation if used.

  return htif_syscall_6args(SYS_openat, AT_FDCWD, name_addr, name_len,
                            host_flags, mode, 0);
}

__attribute__((weak)) int _read(int file, char* ptr, int len) {
  return htif_syscall_6args(SYS_read, file, (uintptr_t)ptr, len, 0, 0, 0);
}

__attribute__((weak)) int _write(int file, char* ptr, int len) {
  return htif_syscall_6args(SYS_write, file, (uintptr_t)ptr, len, 0, 0, 0);
}

__attribute__((weak)) int _close(int file) {
  return htif_syscall_6args(SYS_close, file, 0, 0, 0, 0, 0);
}

__attribute__((weak)) int _lseek(int file, int ptr, int dir) {
  return htif_syscall_6args(SYS_lseek, file, ptr, dir, 0, 0, 0);
}

__attribute__((weak)) int _fstat(int file, struct stat* st) {
  // For now, return a dummy fstat or implement it if needed.
  // Spike's HTIF doesn't have a direct fstat, it uses stat/lstat.
  // But we can implement a basic one if the test uses it.
  memset(st, 0, sizeof(*st));
  st->st_mode = S_IFREG;
  return 0;
}

__attribute__((weak)) int _isatty(int file) { return (file >= 0 && file <= 2); }

__attribute__((weak)) void _exit(int status) {
  uint64_t tohost_val = ((uint64_t)status << 1) | 1;
  asm volatile("fence rw, w" ::: "memory");
  tohost = tohost_val;

  // Signal ready for systems that require it.
  asm volatile("fence rw, w" ::: "memory");
  tohost_ready = 1;

  while (1) {
    asm volatile("wfi");
  }
}

int _kill(int pid, int sig) {
  _exit(128 + sig);
  return -1;
}

int _getpid(void) { return 1; }

__attribute__((weak)) void* _sbrk(int bytes) {
  extern char __heap_start__, __heap_end__;
  static char* _heap_ptr = &__heap_start__;
  char* prev_heap_end;
  if ((bytes < 0) || (_heap_ptr + bytes > &__heap_end__)) {
    errno = ENOMEM;
    return reinterpret_cast<void*>(-1);
  }

  prev_heap_end = _heap_ptr;
  _heap_ptr += bytes;

  return reinterpret_cast<void*>(prev_heap_end);
}
}
