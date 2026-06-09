#ifndef __LUAMAKE_COMMON_HPP
#define __LUAMAKE_COMMON_HPP

#include <cstdint>
#include <string_view>

// TODO: add fmt as a subproject, that way we can used their color system to
// have color in the terminal for displaying errors on all platforms

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
#ifdef NO_TERM_COLOR
#define ERROR
#define WARNING
#define DBG
#define NORMAL
#else
#if defined(__unix__) || defined(__MACH__)
#define ERROR "\033[0;31m"
#define WARNING "\033[0;33m"
#define DBG "\033[0;32m"
#define NORMAL "\033[0;0m"
#else
#define ERROR
#define WARNING
#define DBG
#define NORMAL
#endif
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
  if ((expr)) {                                                                \
    fprintf(stderr, ERROR "Fatel Error:" NORMAL " " #expr NL);                 \
    assert(false);                                                             \
  }

#ifdef DEBUG
#define fn_print()                                                             \
  struct __print final {                                                       \
    __print() noexcept {                                                       \
      fprintf(stderr, "\t" DBG "calling" NORMAL " [%s]" NL,                    \
              __PRETTY_FUNCTION__);                                            \
      fflush(stderr);                                                          \
    }                                                                          \
    ~__print() noexcept {                                                      \
      fprintf(stderr, "\t\t" DBG "exiting" NORMAL " [%s]" NL,                  \
              __PRETTY_FUNCTION__);                                            \
      fflush(stderr);                                                          \
    }                                                                          \
  } ____ {                                                                     \
  }

#define expr_dbg(expr)                                                         \
  do {                                                                         \
    auto const _expr_res = (expr);                                             \
    std::cerr << DBG "[expr] " NORMAL #expr " = " << _expr_res << '\n';        \
  } while (false);
#else
#define fn_print()
#define expr_dbg(expr)
#endif // DEBUG

// NOTE: this is a fucking horrible way of going about this, but it's the only
// way i can think of using c++ :)
#ifdef DEBUG
#define LM_CXPR
#define LM_CXPR_DEF(...)
#define LM_CXPR_DEF_IMPL(...) __VA_ARGS__

#define LM_DBG(...) __VA_ARGS__
#else
#define LM_CXPR constexpr
#define LM_CXPR_DEF(...) constexpr __VA_ARGS__
#define LM_CXPR_DEF_IMPL(...)

#define LM_DBG(...)
#endif // DEBUG

[[noreturn]] auto unreachable() noexcept -> void;

static_assert(sizeof(unsigned char) == 1);
using u8 = unsigned char;

using uint = unsigned int;
using u32 = std::uint32_t;

namespace luamake {
auto os_call(std::string_view const) -> int;
}

#endif
