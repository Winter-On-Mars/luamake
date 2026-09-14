#include "common.hpp"

#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
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
OS::OS() noexcept {
  error_log = fopen(
      (std::filesystem::temp_directory_path() / "luamake_error.log").c_str(),
      "w+b");
}

OS::~OS() noexcept {
  if (error_log != nullptr) {
    fclose(error_log);
  }
}
OS os = OS();

auto OS::call(std::string_view const cmd) -> int {
  auto const pid = fork();
  // should probably throw?
  if (pid < 0) {
    return 1;
  }
  auto child_status = int{};
  switch (pid) {
  case 0: { // child proc
    // TODO: figure out a better way to get the errors displayed, currently it's
    // all or nothing, but it would be nice if we could parse the errors to give
    // some advice on the issues detected
    if (!builtins::cl_options.verbose) {
      if (!is_ready()) {
        return -2;
      }
      auto error_no = fileno(error_log);
      dup2(error_no, STDERR_FILENO);
      dup2(error_no, STDOUT_FILENO);
    }
    if (execl("/bin/sh", "sh", "-c", cmd.data(), nullptr) == -1) {
      std::cerr << "something in execl failed\n";
      std::terminate();
    }
  } break;
  default: {
    (void)waitpid(pid, &child_status, 0);
    if (child_status == -2) {
      throw std::runtime_error("Unable to set up logging file when need");
    }
  } break;
  }
  return child_status;
}
#else
auto os_call(std::string_view const) -> int { return 0; }
#endif
} // namespace luamake
