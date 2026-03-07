#include "luamake_strings.hpp"
#include "common.hpp"

#include <cstring>
#include <iostream>
#include <string_view>
#include <utility>

namespace luamake {
LM_CXPR_DEF_IMPL(
    OwnedString::OwnedString(char *&buffer,
                             len_t size) noexcept : buffer(buffer),
    size(len_t{0}), capacity(size) { buffer = nullptr; })

LM_CXPR_DEF_IMPL(OwnedString::OwnedString() noexcept : buffer(nullptr),
                 size(len_t{0}), capacity(len_t{0}){})

LM_CXPR_DEF_IMPL(
    OwnedString::OwnedString(OwnedString &&that) noexcept : buffer(that.buffer),
    size(that.size), capacity(that.capacity) {
      expr_dbg(__FUNCTION__);
      expr_dbg(this);
      expr_dbg(&that);
      that.buffer = nullptr;
      that.size = 0;
      that.capacity = 0;
    })

LM_CXPR_DEF_IMPL(auto OwnedString::operator=(OwnedString && that) noexcept
                 -> OwnedString & {
                   expr_dbg(__FUNCTION__);
                   expr_dbg(this);
                   expr_dbg(&that);
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
                 })

LM_CXPR_DEF_IMPL(OwnedString::~OwnedString() noexcept {
  if (buffer != nullptr)
    free((void *)buffer);
})

auto OwnedString::append(std::string_view str) noexcept -> void {
  auto const str_len = str.length();
  if (!(size + str_len + 1 < capacity)) {
    /* resize */
    auto next_cap = 3 * (capacity + str_len + 1) / 2;
    buffer = (char *)realloc(buffer, next_cap * sizeof(char));
    if (buffer == nullptr) {
      std::cerr << "Unable to realloc [" << next_cap << "] bytes needed\n";
      std::terminate();
    }
    capacity = next_cap;
  }
  memcpy(buffer + size, str.data(), str_len);
  size += str_len;
  buffer[size] = 0;
  ++size;
}

auto OwnedString::find(std::string_view const str) const noexcept
    -> std::pair<bool, StringViews> {
  auto const *start = buffer;
  auto const *current = buffer;
  auto end = size_t{};

  while (end != size) {
    if (buffer[end] == 0) {
      current = buffer + end;
      // check
      auto const path_view = std::string_view{start, current};
      if (path_view.size() == str.size() && *path_view.data() == *str.data() &&
          strncmp(path_view.data(), str.data(), path_view.size()) == 0) {
        return std::pair(true,
                         StringViews{static_cast<unsigned int>(start - buffer),
                                     static_cast<unsigned int>(end)});
      }
      start = current + 1;
    }
    ++end;
  }

  return std::make_pair(false, StringViews{0, 0});
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
