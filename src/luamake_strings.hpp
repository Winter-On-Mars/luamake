#ifndef __LUAMAKE_STRINGS_HPP
#define __LUAMAKE_STRINGS_HPP

#include "common.hpp"

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

  explicit LM_CXPR OwnedString(char *&buffer, size_t size) noexcept;
  LM_CXPR OwnedString() noexcept;

  LM_CXPR OwnedString(OwnedString &&that) noexcept;

  LM_CXPR auto operator=(OwnedString &&that) noexcept -> OwnedString &;

  LM_CXPR ~OwnedString() noexcept;

  OwnedString(OwnedString const &) = delete;
  OwnedString &operator=(OwnedString const &) = delete;

  LM_CXPR auto subview(size_t start, size_t end) const noexcept
      -> std::string_view;

  auto append(std::string_view const) noexcept -> void;
  auto find(std::string_view const) const noexcept
      -> std::pair<bool, StringViews>;

  auto LM_CXPR view() const noexcept -> std::string_view {
    return std::string_view{buffer, size};
  }
};

LM_CXPR_DEF(
    OwnedString::OwnedString(char *&buffer,
                             size_t size) noexcept : buffer(buffer),
    size(size_t{0}), capacity(size) { buffer = nullptr; })

LM_CXPR_DEF(OwnedString::OwnedString() noexcept : buffer(nullptr),
            size(size_t{0}), capacity(size_t{0}){})

LM_CXPR_DEF(
    OwnedString::OwnedString(OwnedString &&that) noexcept : buffer(that.buffer),
    size(that.size), capacity(that.capacity) {
      that.buffer = nullptr;
      that.size = 0;
      that.capacity = 0;
    })

LM_CXPR_DEF(auto OwnedString::operator=(OwnedString && that) noexcept
            -> OwnedString & {
              if (this == &that) {
                return *this;
              }
              std::swap(buffer, that.buffer);
              std::swap(size, that.size);
              std::swap(capacity, that.capacity);
              return *this;
            })

LM_CXPR_DEF(OwnedString::~OwnedString() noexcept {
  if (buffer != nullptr)
    free((void *)buffer);
})

struct StringViews final {
  size_t start;
  size_t end;
};

struct FixedString final {
  char const *buffer;
  size_t size;

  explicit LM_CXPR FixedString(char *&buffer, size_t size) noexcept;
  LM_CXPR FixedString() noexcept;

  LM_CXPR FixedString(FixedString &&that) noexcept;

  LM_CXPR auto operator=(FixedString &&that) noexcept -> FixedString &;

  LM_CXPR ~FixedString() noexcept;

  FixedString(FixedString const &) = delete;
  FixedString &operator=(FixedString const &) = delete;

  auto LM_CXPR view() const noexcept -> std::string_view {
    return std::string_view{buffer, size};
  }
};

template <size_t len, typename char_t> struct ASmallString final {
  static auto constexpr length() noexcept -> size_t { return len; }

  auto constexpr data() noexcept -> char_t * { return buffer.data() + 1; }

  auto constexpr data() const noexcept -> char_t const * {
    return buffer.data() + 1;
  }

  auto constexpr find(char_t ch) const noexcept -> size_t {
    for (auto i = size_t{}; i < len; ++i) {
      if (buffer[i] == ch)
        return i;
    }
    return npos;
  };

  static auto constexpr npos = size_t(-1);

private:
  std::array<char_t, len> buffer;
};

template <size_t len> using SmallString = ASmallString<len, char>;

LM_CXPR_DEF(
    FixedString::FixedString(char *&buffer,
                             size_t size) noexcept : buffer(buffer),
    size(size) { buffer = nullptr; })

LM_CXPR_DEF(FixedString::~FixedString() noexcept {
  if (buffer != nullptr)
    free((void *)buffer);
})

LM_CXPR_DEF(FixedString::FixedString() noexcept : buffer(nullptr), size(0){})

LM_CXPR_DEF(
    FixedString::FixedString(FixedString &&that) noexcept : buffer(that.buffer),
    size(that.size) {
      that.buffer = nullptr;
      that.size = 0;
    })

LM_CXPR_DEF(auto FixedString::operator=(FixedString && that) noexcept
            -> FixedString & {
              if (this == &that) {
                return *this;
              }
              buffer = that.buffer;
              size = that.size;

              that.buffer = nullptr;
              that.size = 0;
              return *this;
            })
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
