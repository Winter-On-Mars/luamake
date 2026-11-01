#include "luamake_strings.hpp"
#include "common.hpp"

#include <cstring>
#include <iostream>
#include <string_view>
#include <utility>

namespace luamake {
LM_CXPR_DEF_IMPL(
    OwnedString::OwnedString(char *&buffer,
                             size_t size) noexcept : buffer(buffer),
    size(size_t{0}), capacity(size) { buffer = nullptr; })

LM_CXPR_DEF_IMPL(OwnedString::OwnedString() noexcept : buffer(nullptr),
                 size(size_t{0}), capacity(size_t{0}){})

LM_CXPR_DEF_IMPL(
    OwnedString::OwnedString(OwnedString &&that) noexcept : buffer(that.buffer),
    size(that.size), capacity(that.capacity) {
      that.buffer = nullptr;
      that.size = 0;
      that.capacity = 0;
    })

LM_CXPR_DEF_IMPL(auto OwnedString::operator=(OwnedString && that) noexcept
                 -> OwnedString & {
                   if (this == &that) {
                     return *this;
                   }
                   std::swap(buffer, that.buffer);
                   std::swap(size, that.size);
                   std::swap(capacity, that.capacity);
                   return *this;
                 })

LM_CXPR_DEF_IMPL(OwnedString::~OwnedString() noexcept {
  if (buffer != nullptr)
    free((void *)buffer);
})

[[nodiscard]]
auto OwnedString::append(std::string_view str) noexcept -> StringViews {
  auto const start = size;
  auto const str_len = str.length();
  if (!(size + str_len < capacity)) {
    /* resize */
    auto next_cap = 3 * (capacity + str_len) / 2;
    buffer = (char *)realloc(buffer, next_cap * sizeof(char));
    if (buffer == nullptr) {
      std::cerr << "Unable to realloc [" << next_cap << "] bytes needed\n";
      std::terminate();
    }
    capacity = next_cap;
  }
  memcpy(buffer + size, str.data(), str_len);
  size += str_len;
  auto const end = size;
  return StringViews{start, end};
}

LM_CXPR_DEF_IMPL(
    FixedString::FixedString(char *&buffer,
                             size_t size) noexcept : buffer(buffer),
    size(size) { buffer = nullptr; })

LM_CXPR_DEF_IMPL(FixedString::~FixedString() noexcept {
  if (buffer != nullptr)
    free((void *)buffer);
})

LM_CXPR_DEF_IMPL(FixedString::FixedString() noexcept : buffer(nullptr),
                 size(0){})

LM_CXPR_DEF_IMPL(
    FixedString::FixedString(FixedString &&that) noexcept : buffer(that.buffer),
    size(that.size) {
      that.buffer = nullptr;
      that.size = 0;
    })

LM_CXPR_DEF_IMPL(auto FixedString::operator=(FixedString && that) noexcept
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
