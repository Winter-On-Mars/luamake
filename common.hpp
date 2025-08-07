#ifndef __LUAMAKE_COMMON_HPP
#define __LUAMAKE_COMMON_HPP

#include <cstdio>
#include <filesystem>

#ifdef DEBUG
#include <iostream>
#endif

#if defined(_WIN32)
#define NL "\r\n"
#elif defined(__unix__)
#define NL "\n"
#elif defined(__MACH__)
#define NL "\n"
#else
#warning ("new line macro defined, you can help the project by adding another header guard and defining it");
#define NL ""
#endif

// color things for error messages/ warnings
// TODO: extract these into a platform independent thing so this'll actually
// work on windows and shit
#ifdef NO_TERM_COLOR
#define ERROR
#define WARNING
#define DBG
#define NORMAL
#else
#define ERROR "\033[0;31m"
#define WARNING "\033[0;33m"
#define DBG "\033[0;32m"
#define NORMAL "\033[0;0m"
#endif

#define error_message(msg)                                                     \
  fprintf(stderr, ERROR "Fatel Error:" NORMAL " " msg NL)
#define ferror_message(msg, ...)                                               \
  fprintf(stderr, ERROR "Fatel Error:" NORMAL " " msg NL, __VA_ARGS__)
#define warning_message(msg)                                                   \
  fprintf(stderr, WARNING "Warning:" NORMAL " " msg NL)
#define fwarning_message(msg, ...)                                             \
  fprintf(stderr, WARNING "Warning:" NORMAL " " msg NL, __VA_ARGS__)

#define ASSERT_ERROR(expr)                                                     \
  if (expr) {                                                                  \
    fprintf(stderr, ERROR "Fatel Error:" NORMAL " " #expr NL);                 \
    assert(false);                                                             \
  }

#define fn_print()                                                             \
  fprintf(stderr, "\t" DBG "calling" NORMAL " [%s]" NL, __PRETTY_FUNCTION__)

#define exit_fn_print()                                                        \
  fprintf(stderr, "\t\t" DBG "exiting" NORMAL " [%s]" NL, __PRETTY_FUNCTION__)

#define expr_dbg(expr)                                                         \
  do {                                                                         \
    auto const res = (expr);                                                   \
    std::cerr << DBG "[expr] " NORMAL #expr " = " << res << '\n';              \
  } while (false);

[[noreturn]] auto unreachable() noexcept -> void;

namespace fs = std::filesystem;

struct File final {
  enum permissions : unsigned char {
    READ = 1 << 0,
    WRITE = 1 << 1,
    BINARY = 1 << 2,
  };

  constexpr File(fs::path const &path, permissions &&perms) noexcept
      : file(nullptr) {
    char max_length_perms[] = {0, 0, 0,
                               0}; // this should be 5 or 7 from man fread
    if ((perms & READ) == READ)
      max_length_perms[0] = 'r';
    if ((perms & WRITE) == WRITE)
      max_length_perms[max_length_perms[0] != 0 ? 1 : 0] = 'w';
    if ((perms & BINARY) == BINARY)
      max_length_perms[max_length_perms[0] != 0
                           ? max_length_perms[1] != 0 ? 2 : 1
                           : 0] = 'b';

#ifdef DEBUG
    std::cerr << "Opening [" << path << "] with options [" << max_length_perms
              << "]\n";
#endif // DEBUG
    file = fopen(path.c_str(), max_length_perms);
  }
  constexpr ~File() noexcept {
    if (file != nullptr)
      fclose(file);
  }

  // implicit conversion operator to FILE*
  operator FILE *() const noexcept { return file; }

  auto write(void const *__restrict ptr, size_t size, size_t amount) noexcept
      -> size_t {
    return fwrite(ptr, size, amount, file);
  }

  /// @return returns the amount read from the file stream
  auto read(void *__restrict dest, size_t size, size_t n) noexcept -> size_t {
    return fread(dest, size, n, file);
  }

  auto write_num(int c) noexcept -> int { return fputc(c, file); }

  auto flush() noexcept -> void { fflush(file); }

private:
  FILE *file;
};

auto constexpr operator|(File::permissions lhs, File::permissions rhs) noexcept
    -> File::permissions {
  return static_cast<File::permissions>(
      static_cast<std::underlying_type_t<File::permissions>>(lhs) |
      static_cast<std::underlying_type_t<File::permissions>>(rhs));
}

#endif
