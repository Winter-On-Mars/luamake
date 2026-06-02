#ifndef __LUAMAKE_ALLOCATOR_HPP
#define __LUAMAKE_ALLOCATOR_HPP

#include "common.hpp"
#include <cstddef>

extern "C" {
#include "lua.h"
}

#ifdef DEBUG_ALLOCATOR
#include <ostream>
#endif // DEBUG_ALLOCATOR

namespace luamake::allocator {
  struct Page final {
    Page() noexcept;
    auto to_lua_alloc() -> lua_Alloc;

    private:
    static auto lua_alloc(void *, void *, size_t, size_t) -> void *;

    static auto constexpr get_alignment(size_t size) -> size_t {
      return (n + sizeof(word_t) - 1) & ~(sizeof(word_t) - 1);
    }

    struct Header final {
      size_t amount_used;
      u8 *cur;
      Header* next;
    };

    Header start;
    Header *cur_page;

#ifdef DEBUG_ALLOCATOR
    size_t amount_alloc = 0;

    auto dump_stats(std::ostream &) -> std::ostream &;
#endif // DEBUG_ALLOCATOR
  };
} // namespace luamake::allocator

#endif // !__LUAMAKE_ALLOCATOR_HPP
