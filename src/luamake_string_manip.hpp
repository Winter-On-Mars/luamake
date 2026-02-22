#pragma once

#include <algorithm>
#include <span>
#include <string_view>

namespace luamake {
template <typename int_t, typename char_t>
constexpr auto skip_ws(std::basic_string_view<char_t> const buf,
                       int_t i) noexcept -> int_t {
  auto constexpr ws = std::span<char_t const>(" \t\n\r");
  while (i < buf.size()) {
    auto const found = std::find(ws.begin(), ws.end(), buf[i]);
    if (found != ws.end()) {
      ++i;
    } else {
      return i;
    }
  }
  return buf.npos;
}

static_assert([]() {
  auto constexpr hello = std::string_view("hello world");
  auto constexpr skipped_index = skip_ws(hello, size_t{});
  if (skipped_index != 0) {
    throw skipped_index;
  }
  return true;
  // return skip_ws(hello, size_t{}) == 0;
}());

static_assert([]() {
  auto constexpr hello = std::string_view("  hello");
  return skip_ws(hello, size_t{}) == 2;
}());

template <typename int_t, typename char_t>
constexpr auto skip_until(std::basic_string_view<char_t> const delims,
                          std::basic_string_view<char_t> const buf,
                          int_t i) noexcept -> int_t {
  for (; i < buf.size(); ++i) {
    for (auto &&delim : delims) {
      if (buf[i] == delim) {
        return i;
      }
    }
  }
  return i;
}

static_assert([]() {
  auto constexpr hello = std::string_view("hello world");
  return skip_until(std::string_view(" "), hello, size_t{}) == 5;
}());

template <typename int_t, typename char_t>
constexpr auto skip_until(char_t delim,
                          std::basic_string_view<char_t> const buf,
                          int_t i) noexcept -> int_t {
  for (; i < buf.size(); ++i) {
    if (buf[i] == delim)
      return i;
  }
  return i;
}

template <typename int_t, typename char_t>
constexpr auto skip_until(char_t delim, std::span<char_t> const buf,
                          int_t i) noexcept -> int_t {
  for (; i < buf.size(); ++i) {
    if (buf[i] == delim)
      return i;
  }
  return i;
}

template <typename int_t, typename char_t>
constexpr auto skip_until(auto &&delims, std::span<char_t> const buf,
                          int_t i) noexcept -> int_t {
  for (; i < buf.size(); ++i) {
    for (auto &&delim : delims) {
      if (buf[i] == delim) {
        return i;
      }
    }
  }
  return i;
}

template <typename int_t, typename char_t>
constexpr auto skip_while(std::basic_string_view<char_t> const delims,
                          std::basic_string_view<char_t> const buf,
                          int_t i) noexcept -> int_t {
  for (; i < buf.size(); ++i) {
    if (delims.find(buf[i]) == delims.npos) {
      return i;
    }
  }
  return i;
}

static_assert([]() {
  auto constexpr hello = std::string_view(" \t hello world");
  static_assert(hello[3] == 'h');
  return skip_while(std::string_view(" \t"), hello, size_t{}) == 3;
}());

static_assert([]() {
  auto constexpr macro = std::string_view(" (a)");
  static_assert(macro[2] == 'a');
  return skip_while(std::string_view(" \t"), macro, size_t{}) == 1;
}());

static_assert([]() {
  auto constexpr macro = std::string_view("\n");
  return skip_while(std::string_view(" \t"), macro, size_t{}) == 0;
}());

template <typename char_t>
constexpr auto is_any_of(std::basic_string_view<char_t> const delims,
                         char_t ch) noexcept -> bool {
  return delims.find(ch) != delims.npos;
}

template <typename char_t> constexpr auto is_digit(char_t ch) noexcept -> bool {
  if (ch - '0' >= 0 && ch - '9' <= 0)
    return true;
  else
    return false;
}

static_assert(is_digit('9'));
static_assert(!is_digit('a'));
static_assert(is_digit('4'));
static_assert(is_digit('5'));
static_assert(!is_digit('!'));

template <typename char_t> constexpr auto is_alpha(char_t ch) noexcept -> bool {
  if ((ch - 'a' >= 0 && ch - 'z' <= 0) || (ch - 'A' >= 0 && ch - 'Z' <= 0))
    return true;
  else
    return false;
}

static_assert(!is_alpha('9'));
static_assert(is_alpha('a'));
static_assert(is_alpha('A'));
static_assert(is_alpha('X'));
static_assert(!is_alpha('4'));
static_assert(!is_alpha('5'));
static_assert(!is_alpha('!'));
} // namespace luamake
