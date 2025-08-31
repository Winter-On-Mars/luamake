#include "luamake_strings.hpp"

#include <cstring>
#include <iostream>

namespace luamake {
OwnedString::OwnedString(char *buffer, size_t size) noexcept
    : buffer(buffer), size(0), capacity(size) {}
OwnedString::~OwnedString() noexcept {
  if (buffer != nullptr)
    free((void *)buffer);
}

auto OwnedString::append(std::string &&str) noexcept -> void {
  auto const str_len = str.length();
  if (!(size < capacity - str_len - 1)) {
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

FixedString::FixedString(char const *buffer, size_t size) noexcept
    : buffer(buffer), size(size) {}

FixedString::~FixedString() noexcept {
  if (buffer != nullptr)
    free((void *)buffer);
}
} // namespace luamake
