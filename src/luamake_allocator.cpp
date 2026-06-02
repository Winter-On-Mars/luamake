#include "luamake_allocator.hpp"

namespace luamake::allocator {
Page::Page() noexcept : start({0, nullptr, nullptr}), cur_page(&start) {

}

auto Page::to_lua_alloc() -> lua_Alloc {
  return nullptr;
}

auto Page::lua_alloc(void * ud, void * ptr, size_t o_size, size_t n_size) -> void * {
  return nullptr;
}
}
