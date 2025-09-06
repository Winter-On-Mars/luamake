#ifndef __LUAMAKE_STRINGS_HPP
#define __LUAMAKE_STRINGS_HPP

#include <cstddef>
#include <string>

namespace luamake {

/**
 * @brief takes ownership of the buffer, with buffer.size == size
 */
struct OwnedString final {
  char *buffer;
  size_t size;
  size_t capacity;

  explicit OwnedString(char *&buffer, size_t size) noexcept;
  constexpr OwnedString() noexcept;

  constexpr OwnedString(OwnedString &&that) noexcept;

  constexpr auto operator=(OwnedString &&that) noexcept -> OwnedString &;

  ~OwnedString() noexcept;

  OwnedString(OwnedString const &) = delete;
  OwnedString &operator=(OwnedString const &) = delete;

  auto append(std::string &&) noexcept -> void;
};

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
  buffer = that.buffer;
  size = that.size;
  capacity = that.capacity;

  that.buffer = nullptr;
  that.size = 0;
  that.capacity = 0;
  return *this;
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
};

constexpr FixedString::FixedString() noexcept : buffer(nullptr), size(0) {}

constexpr FixedString::FixedString(FixedString &&that) noexcept
    : buffer(that.buffer), size(that.size) {
  that.buffer = nullptr;
  that.size = 0;
}

constexpr auto FixedString::operator=(FixedString &&that) noexcept
    -> FixedString & {
  buffer = that.buffer;
  size = that.size;

  that.buffer = nullptr;
  that.size = 0;
  return *this;
}

} // namespace luamake

#endif
