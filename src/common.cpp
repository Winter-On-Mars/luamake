#include "common.hpp"

// source = [[https://en.cppreference.com/w/cpp/utility/unreachable]]
[[noreturn]] auto unreachable() noexcept -> void {
#if defined(_MSC_VER) && !defined(__clang__)
  __assume(false);
#else
  __builtin_unreachable();
#endif
}
