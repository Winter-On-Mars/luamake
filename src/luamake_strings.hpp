#ifndef __LUAMAKE_STRINGS_HPP
#define __LUAMAKE_STRINGS_HPP

#include <cstddef>
#include <format>
#include <string_view>
#include <utility>

namespace luamake {

struct StringViews;

/**
 * @brief takes ownership of the buffer, with buffer.size == size
 *  buffer *must* be allocated with (m|re|ca)alloc, as ~OwnedString calls into
 *  free
 */
struct OwnedString final {
  char *buffer;
  size_t size;
  size_t capacity;

  explicit constexpr OwnedString(char *&buffer, size_t size) noexcept;
  constexpr OwnedString() noexcept;

  constexpr OwnedString(OwnedString &&that) noexcept;

  constexpr auto operator=(OwnedString &&that) noexcept -> OwnedString &;

  constexpr ~OwnedString() noexcept;

  OwnedString(OwnedString const &) = delete;
  OwnedString &operator=(OwnedString const &) = delete;

  auto append(std::string_view const) noexcept -> void;
  auto find(std::string_view const) const noexcept
      -> std::pair<bool, StringViews>;

  auto constexpr view() const noexcept -> std::string_view {
    return std::string_view{buffer, size};
  }
};

constexpr OwnedString::OwnedString(char *&buffer, size_t size) noexcept
    : buffer(buffer), size(0), capacity(size) {
  buffer = nullptr;
}

constexpr OwnedString::OwnedString() noexcept
    : buffer(nullptr), size(0), capacity(0) {}

constexpr OwnedString::OwnedString(OwnedString &&that) noexcept
    : buffer(that.buffer), size(that.size), capacity(that.capacity) {
  that.buffer = nullptr;
  that.size = 0;
  that.capacity = 0;
}

constexpr auto OwnedString::operator=(OwnedString &&that) noexcept
    -> OwnedString & {
  if (this == &that) {
    return *this;
  }
  buffer = that.buffer;
  size = that.size;
  capacity = that.capacity;

  that.buffer = nullptr;
  that.size = 0;
  that.capacity = 0;
  return *this;
}

constexpr OwnedString::~OwnedString() noexcept {
  if (buffer != nullptr)
    free((void *)buffer);
}

struct StringViews final {
  unsigned int start;
  unsigned int end;
};

struct FixedString final {
  char const *buffer;
  size_t size;

  explicit FixedString(char const *buffer, size_t size) noexcept;
  constexpr FixedString() noexcept;

  constexpr FixedString(FixedString &&that) noexcept;

  constexpr auto operator=(FixedString &&that) noexcept -> FixedString &;

  ~FixedString() noexcept;

  FixedString(FixedString const &) = delete;
  FixedString &operator=(FixedString const &) = delete;

  auto constexpr view() const noexcept -> std::string_view {
    return std::string_view{buffer, size};
  }
};

constexpr FixedString::FixedString() noexcept : buffer(nullptr), size(0) {}

constexpr FixedString::FixedString(FixedString &&that) noexcept
    : buffer(that.buffer), size(that.size) {
  that.buffer = nullptr;
  that.size = 0;
}

constexpr auto FixedString::operator=(FixedString &&that) noexcept
    -> FixedString & {
  if (this == &that) {
    return *this;
  }
  buffer = that.buffer;
  size = that.size;

  that.buffer = nullptr;
  that.size = 0;
  return *this;
}
} // namespace luamake

namespace std {
template <> struct formatter<luamake::OwnedString, char> {
  auto constexpr parse(format_parse_context &ctx) { return ctx.begin(); }
  auto format(luamake::OwnedString const &str, format_context &ctx) const {
    return std::format("{}", string_view{str.buffer, str.size});
  }
};

template <> struct formatter<luamake::FixedString, char> {
  auto constexpr parse(format_parse_context &ctx) { return ctx.begin(); }
  auto format(luamake::FixedString const &str, format_context &ctx) const {
    return std::format("{}", string_view{str.buffer, str.size});
  }
};
} // namespace std

#endif
