#ifndef __LUAMAKE_COMMON_HPP
#define __LUAMAKE_COMMON_HPP

#include <cstdint>
#include <string_view>

// TODO: add fmt as a subproject, that way we can used their color system to
// have color in the terminal for displaying errors on all platforms

#if defined(_WIN32)
#define LM_NL "\r\n"
#elif defined(__unix__)
#define LM_NL "\n"
#elif defined(__MACH__)
#define LM_NL "\n"
#else
#warning ("new line macro defined, you can help the project by adding another header guard and defining it");
#define LM_NL ""
#endif

// color things for error messages/ warnings
#ifdef NO_TERM_COLOR
#define LM_ERROR
#define LM_WARNING
#define LM_DBG
#define LM_NORMAL
#else
#if defined(__unix__) || defined(__MACH__)
#define LM_ERROR "\033[0;31m"   // red
#define LM_WARNING "\033[0;33m" // yellow
#define LM_HELP "\033[0;32m"    // blue (should change to green)
#define LM_NORMAL "\033[0;0m"   // resets
#else
#define LM_ERROR
#define LM_WARNING
#define LM_DBG
#define LM_NORMAL
#endif
#endif

#define error_message(msg)                                                     \
  fprintf(stderr, LM_ERROR "Fatel Error:" LM_NORMAL " " msg LM_NL)
#define ferror_message(msg, ...)                                               \
  fprintf(stderr, LM_ERROR "Fatel Error:" LM_NORMAL " " msg LM_NL, __VA_ARGS__)
#define warning_message(msg)                                                   \
  fprintf(stderr, LM_WARNING "Warning:" LM_NORMAL " " msg LM_NL)
#define fwarning_message(msg, ...)                                             \
  fprintf(stderr, LM_WARNING "Warning:" LM_NORMAL " " msg LM_NL, __VA_ARGS__)

#define ASSERT_ERROR(expr)                                                     \
  if ((expr)) {                                                                \
    fprintf(stderr, LM_ERROR "Fatel Error:" LM_NORMAL " " #expr LM_NL);        \
    assert(false);                                                             \
  }

#ifdef DEBUG
#define fn_print()                                                             \
  struct __print final {                                                       \
    __print() noexcept {                                                       \
      fprintf(stderr, "\t" LM_HELP "calling" LM_NORMAL " [%s]" LM_NL,          \
              __PRETTY_FUNCTION__);                                            \
      fflush(stderr);                                                          \
    }                                                                          \
    ~__print() noexcept {                                                      \
      fprintf(stderr, "\t\t" LM_HELP "exiting" LM_NORMAL " [%s]" LM_NL,        \
              __PRETTY_FUNCTION__);                                            \
      fflush(stderr);                                                          \
    }                                                                          \
  } ____ {                                                                     \
  }

#define expr_dbg(expr)                                                         \
  do {                                                                         \
    auto const _expr_res = (expr);                                             \
    std::cerr << LM_HELP "[expr] " LM_NORMAL #expr " = " << _expr_res << '\n'; \
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
using u16 = std::uint16_t;
using u32 = std::uint32_t;

namespace luamake {
auto os_call(std::string_view const) -> int;
}

#endif
