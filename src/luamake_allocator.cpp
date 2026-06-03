#include "luamake_allocator.hpp"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <format>
#include <iostream>
#include <stdexcept>

namespace luamake::allocator {
Page::Page() noexcept : start({0, nullptr, nullptr}), cur_page(&start) {
  start.cur = static_cast<u8 *>(malloc(SIZE));
  if (start.cur == nullptr) {
    std::cerr << std::format(
        "Unable to get enough memory to initialize allocator");
    std::terminate();
  }
}

Page::~Page() noexcept {
#ifdef DEBUG_ALLOCATOR
  dump_stats(std::cout).flush();
#endif // DEBUG_ALLOCATOR
  for (auto *page = &start; page;) {
    auto const next = page->next;
    free(page->cur);
    page = next;
  }
}

auto Page::to_lua_alloc() -> lua_Alloc { return lua_alloc; }

auto Page::lua_alloc(void *ud, void *ptr, size_t o_size, size_t n_size)
    -> void * {
  // don't have to do any work
  if (n_size == 0)
    return nullptr;
  if (ptr != nullptr && o_size != 0 && n_size != 0) {
    if (n_size > o_size) {
      // realloc, i.e. memcpy
      auto *page = static_cast<Page *>(ud);
      auto ret_ptr = page->alloc(n_size);
#ifdef DEBUG_ALLOCATOR
      page->wasted_space += o_size;
#endif // DEBUG_ALLOCATOR
      // realloc
      memcpy(ret_ptr, ptr, o_size);
      return ret_ptr;
    }
    return ptr;
  }
  return static_cast<Page *>(ud)->alloc(n_size);
}

auto Page::alloc(size_t n_bytes) -> void * {
#ifdef DEBUG_ALLOCATOR
  wasted_space += get_alignment(n_bytes) - n_bytes;
#endif // DEBUG_ALLOCATOR
  n_bytes = get_alignment(n_bytes);
#ifdef DEBUG_ALLOCATOR
  amount_alloc += n_bytes;
#endif // DEBUG_ALLOCATOR
  [[unlikely]]
  if (n_bytes >= SIZE - sizeof(Header)) {
    throw std::runtime_error(
        std::format("Unable to allocate requested amount of memory [{}] bytes, "
                    "too big to fit in a page [{}] bytes",
                    n_bytes, SIZE - sizeof(Header)));
  }
  [[unlikely]]
  if (n_bytes + cur_page->amount_used >= SIZE) {
#ifdef DEBUG_ALLOCATOR
    wasted_space += SIZE - cur_page->amount_used;
#endif // DEBUG_ALLOCATOR
    get_new_page();
  }
  auto ret_ptr = cur_page->cur + cur_page->amount_used;
  cur_page->amount_used += n_bytes;
  return static_cast<void *>(ret_ptr);
}

auto Page::get_new_page() -> void {
  cur_page->next = static_cast<Header *>(malloc(SIZE));
  if (cur_page->next == nullptr)
    throw std::runtime_error("Unable to allocate new page of memory");
  cur_page = cur_page->next;
  cur_page->cur = reinterpret_cast<u8 *>(cur_page);
  cur_page->amount_used = sizeof(Header);
  cur_page->next = nullptr;
}

#ifdef DEBUG_ALLOCATOR
auto Page::dump_stats(std::ostream &out) -> std::ostream & {
  std::cout << std::format("Allocated [{:*>8}] bytes, wasted [{:*>8}] bytes, "
                           "ratio [{:.2f}%], took [{}] pages\n",
                           amount_alloc, wasted_space,
                           static_cast<double>(amount_alloc) /
                               static_cast<double>(wasted_space),
                           [&]() {
                             auto num_pages = size_t{};
                             for (auto page = &start; page; page = page->next) {
                               ++num_pages;
                             }
                             return num_pages;
                           }());
  return out;
}
#endif // DEBUG_ALLOCATOR
} // namespace luamake::allocator
