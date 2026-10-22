"""Keep Docker's seccomp policy and make clone3 fall back on linux/amd64."""
import ctypes
import errno
import os
import sys


def install_clone3_fallback():
    class Filter(ctypes.Structure):
        _fields_ = [('code', ctypes.c_ushort), ('jt', ctypes.c_ubyte),
                    ('jf', ctypes.c_ubyte), ('k', ctypes.c_uint32)]

    class Program(ctypes.Structure):
        _fields_ = [('len', ctypes.c_ushort), ('filter', ctypes.POINTER(Filter))]

    # For x86_64 clone3, return ENOSYS so glibc uses clone instead. Other
    # calls still pass through every previously installed Docker filter.
    instructions = (Filter * 6)(
        Filter(0x20, 0, 0, 4),                  # load seccomp_data.arch
        Filter(0x15, 0, 3, 0xC000003E),         # AUDIT_ARCH_X86_64
        Filter(0x20, 0, 0, 0),                  # load seccomp_data.nr
        Filter(0x15, 0, 1, 435),                # __NR_clone3
        Filter(0x06, 0, 0, 0x00050000 | errno.ENOSYS),
        Filter(0x06, 0, 0, 0x7FFF0000),         # SECCOMP_RET_ALLOW
    )
    program = Program(len(instructions), instructions)
    libc = ctypes.CDLL(None, use_errno=True)
    libc.prctl.argtypes = [ctypes.c_int] + [ctypes.c_ulong] * 4
    libc.prctl.restype = ctypes.c_int
    for args in ((38, 1, 0, 0, 0),          # PR_SET_NO_NEW_PRIVS
                 (22, 2, ctypes.addressof(program), 0, 0)):
        if libc.prctl(*args) != 0:
            code = ctypes.get_errno()
            raise OSError(code, os.strerror(code))


def main():
    try:
        install_clone3_fallback()
    except OSError as error:
        print('无法启用 Docker 编译兼容处理：' + str(error)
              + '；请查看 DOCKER_COMPATIBILITY.md。', file=sys.stderr)
        return 1
    os.execv(sys.executable, [sys.executable, '/opt/storagestacked/entrypoint.py',
                             *sys.argv[1:]])


if __name__ == '__main__':
    sys.exit(main())
