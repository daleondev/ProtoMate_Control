// Keep libstdc++ file-stream objects ABI-compatible with the library that
// supplies their out-of-line constructors and member functions.
//
// The runtime's gthreads overlay intentionally replaces __gthread_mutex_t
// with a compact ThreadX handle. basic_filebuf embeds an unused __c_lock,
// however, and its size remains part of the libstdc++ ABI. On Linux the
// system library was built with pthread_mutex_t in that field. Using the
// compact handle there makes basic_filebuf (and therefore fstream objects)
// too small and lets the system constructor overwrite adjacent memory.

#ifndef _GLIBCXX_CXX_IO_H
#define _GLIBCXX_CXX_IO_H 1

#include <cstdio>
#include <bits/gthr.h>

#if defined(__linux__)
#include <pthread.h>
#endif

namespace std _GLIBCXX_VISIBILITY(default)
{
_GLIBCXX_BEGIN_NAMESPACE_VERSION

#if defined(__linux__)
  using __c_lock = pthread_mutex_t;
#elif defined(__GTHREAD_LEGACY_MUTEX_T)
  using __c_lock = __GTHREAD_LEGACY_MUTEX_T;
#else
  // Arm's libstdc++ uses a four-byte __gthread_mutex_t for this ABI slot.
  // The ThreadX MutexHandle is also pointer-sized (four bytes on Cortex-M).
  using __c_lock = __gthread_mutex_t;
#endif

  using __c_file = FILE;

_GLIBCXX_END_NAMESPACE_VERSION
} // namespace std

#endif // _GLIBCXX_CXX_IO_H
