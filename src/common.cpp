#include "common.hpp"

#include <cstdio>
#include <exception>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
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

namespace luamake::os {
static auto counter = std::atomic<size_t>{};
auto call(std::string_view const cmd) noexcept -> int {
#ifndef PERF_TESTING
  try {
    auto const cur = counter++;
    // TODO: factor this out, also move the calling with the empty file into
    // this directory
    std::filesystem::create_directories(std::filesystem::temp_directory_path() /
                                        "luamake");
    auto const fname = std::filesystem::temp_directory_path() / "luamake" /
                       std::to_string(cur);
    auto out = fopen(fname.c_str(), "w+b");

    auto const pid = fork();
    // should probably throw?
    if (pid < 0) {
      return 1;
    }
    auto child_status = int{};
    switch (pid) {
    case 0: { // child proc
      if (!builtins::cl_options.verbose) {
        auto const out_no = fileno(out);
        dup2(out_no, STDERR_FILENO);
        dup2(out_no, STDOUT_FILENO);
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
    switch (child_status) {
    case -2:
      throw std::runtime_error("Unable to set up logging file when need");
    case 0:
      break;
    default: {
      // TODO: parse the file /tmp/luamake/<cur> to get the errors,
      // also figure out which compiler we used because they have different
      // error layouts
      // for now we just display (part of) the error message, just the head
      auto buffer = std::array<char, 1024>{};
      rewind(out);
      auto const amount_read = fread(buffer.data(), 1, buffer.size(), out);
      fprintf(stdout, "%.*s", static_cast<int>(amount_read), buffer.data());
    } break;
    }
    if (out != nullptr)
      fclose(out);
    return child_status;
  } catch (std::exception const &e) {
    std::cerr << "Error: " << e.what() << '\n';
    return -1;
  } catch (...) {
    std::cerr << "Fuck";
    return -1;
  }
#else
  auto const _ = cmd;
  return 0;
#endif
}
} // namespace luamake::os
