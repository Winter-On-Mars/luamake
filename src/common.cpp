#include "common.hpp"

#include <fcntl.h>
#include <iostream>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>

#include "luamake_builtins.hpp"

// source = [[https://en.cppreference.com/w/cpp/utility/unreachable]]
[[noreturn]] auto unreachable() noexcept -> void {
#if defined(_MSC_VER) && !defined(__clang__)
  __assume(false);
#else
  __builtin_unreachable();
#endif
}

namespace luamake {
#ifndef PERF_TESTING
// shouldn't fail(?)
static auto dev_null = open("/dev/null", O_WRONLY);
auto os_call(std::string_view const cmd) -> int {
  auto const pid = fork();
  // should probably throw?
  if (pid < 0) {
    return 1;
  }
  auto child_status = int{};
  switch (pid) {
  case 0: { // child proc
    if (!builtins::cl_options.verbose) {
      dup2(dev_null, STDERR_FILENO);
      dup2(dev_null, STDOUT_FILENO);
    }
    if (execl("/bin/sh", "sh", "-c", cmd.data(), nullptr) == -1) {
      std::cerr << "something in execl failed\n";
      std::terminate();
    }
  } break;
  default: {
    (void)waitpid(pid, &child_status, 0);
  } break;
  }
  return child_status;
}
#else
auto os_call(std::string_view const) -> int { return 0; }
#endif
} // namespace luamake
