#include "luamake_strings.hpp"

#include <cstring>
#include <iostream>
#include <string_view>
#include <utility>

namespace luamake {
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
} // namespace luamake
