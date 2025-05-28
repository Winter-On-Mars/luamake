#ifndef __LUAMAKE_COMMON_HPP
#define __LUAMAKE_COMMON_HPP

#include <filesystem>

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

#endif
