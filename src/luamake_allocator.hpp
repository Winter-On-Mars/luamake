#ifndef __LUAMAKE_ALLOCATOR_HPP
#define __LUAMAKE_ALLOCATOR_HPP

#include <cstddef>
#include <cstdint>

extern "C" {
#include "lua.h"
}

#ifdef DEBUG_ALLOCATOR
#include <ostream>
#endif // DEBUG_ALLOCATOR

#include "common.hpp"

namespace luamake::allocator {
struct Page final {
  // TODO: expose this as a compilation parameter, this number seems to work the
  // best for my machine in terms of a boost in performance
  static auto constexpr SIZE = size_t{2 << 16};
  Page() noexcept;
  ~Page() noexcept;
  // not technically required, but ensures that the allocator is initialized
  // before use
  auto to_lua_alloc() -> lua_Alloc;

private:
  static auto lua_alloc(void *, void *, size_t, size_t) -> void *;

  static auto constexpr get_alignment(size_t size) -> size_t {
    return (size + sizeof(intptr_t) - 1) & ~(sizeof(intptr_t) - 1);
  }

  auto alloc(size_t) -> void *;
  auto get_new_page() -> void;

  struct Header final {
    size_t amount_used;
    u8 *cur;
    Header *next;
  };

  Header start;
  Header *cur_page;

#ifdef DEBUG_ALLOCATOR
  size_t amount_alloc = 0;
  size_t wasted_space = 0;

  auto dump_stats(std::ostream &) -> std::ostream &;
#endif // DEBUG_ALLOCATOR
};
} // namespace luamake::allocator

#endif // !__LUAMAKE_ALLOCATOR_HPP
