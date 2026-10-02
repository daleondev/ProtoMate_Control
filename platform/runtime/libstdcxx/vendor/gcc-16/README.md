# GCC 16 runtime sources

`c++17/fs_ops.cc`, `c++17/fs_dir.cc`, `c++20/atomic.cc`,
`filesystem/ops-common.h`, and `filesystem/dir-common.h` are copied verbatim from the
`releases/gcc-16.1.0` tag of <https://github.com/gcc-mirror/gcc>.

All five files were compared with the `releases/gcc-16.2.0` tag and are
byte-for-byte identical. GCC 16.2 therefore reuses this source snapshot.
The Arm header guard accepts the GCC 16.1 `__GLIBCXX__` date `20260430` and
the GCC 16.2 date `20260807` (including Arch's `arm-none-eabi-gcc` 16.2.0).

The upstream files are licensed under GPLv3 with the GCC Runtime Library
Exception 3.1. The corresponding texts are in `licenses/COPYING3` and
`licenses/COPYING.RUNTIME`.

`bits/largefile-config.h` is a project-owned generated compatibility header
for the Newlib target; it is not an upstream source file.
