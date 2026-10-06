#include "luamake_allocator.hpp"

#include "common.hpp"

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <format>
#include <iostream>
#include <new>
#include <stdexcept>

extern "C" {
#include "lua/lua.h"
}

#ifdef DEBUG_ALLOCATOR
#include <ostream>
#endif // DEBUG_ALLOCATOR

namespace luamake::allocator {
Page::Page(size_t page_size) noexcept
    : page_size(page_size), start({0, nullptr, nullptr}), cur_page(&start) {}

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

auto Page::to_lua_alloc() -> lua_Alloc {
  init();
  return lua_alloc;
}

auto Page::init() -> void {
  if (start.cur != nullptr) {
    return; // already initialized
  }
  start.cur = static_cast<u8 *>(malloc(page_size));
  if (start.cur == nullptr) {
    std::cerr << std::format(
        "Unable to get enough memory to initialize allocator");
    std::terminate();
  }
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
  if (n_bytes >= page_size - sizeof(Header)) {
    throw std::runtime_error(
        std::format("Unable to allocate requested amount of memory [{}] bytes, "
                    "too big to fit in a page [{}] bytes",
                    n_bytes, page_size - sizeof(Header)));
  }
  [[unlikely]]
  if (n_bytes + cur_page->amount_used > page_size) {
#ifdef DEBUG_ALLOCATOR
    unused_space += page_size - cur_page->amount_used;
#endif // DEBUG_ALLOCATOR
    get_new_page();
  }
  auto ret_ptr = cur_page->cur + cur_page->amount_used;
  cur_page->amount_used += n_bytes;
  return static_cast<void *>(ret_ptr);
}

auto Page::reset() -> void {
  cur_page = &start;
  // the start page doesn't have the header info crammed in the same page, it
  // would technically be fine to also mark the amount used of the start page as
  // sizeof(Header), but then we'd be wasting 24 (3 * sizeof(void*)) bytes
  cur_page->amount_used = 0;
  for (auto *page = cur_page->next; page; page = page->next) {
    page->amount_used = sizeof(Header);
  }
}

#ifdef DEBUG_ALLOCATOR
auto Page::display(std::ostream &out) -> std::ostream & {
  out << std::format("page_size = {}, cur_page = {}, start = {{.amount_used = "
                     "{}, .cur = {}, .next = {}}}@{}",
                     page_size, (void *)cur_page, start.amount_used,
                     (void *)start.cur, (void *)start.next, (void *)&start);
  return out;
}

#endif // !DEBUG_ALLOCATOR

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
      std::memcpy(ret_ptr, ptr, o_size);
      return ret_ptr;
    }
    return ptr;
  }
  return static_cast<Page *>(ud)->alloc(n_size);
}

auto Page::get_new_page() -> void {
  // to work with the reset function, that just sets the current page to the
  // start, used mostly in the cpp eval function to avoid memory allocations
  if (cur_page->next != nullptr) {
    cur_page = cur_page->next;
    return;
  }
  auto page = static_cast<u8 *>(malloc(page_size));
  if (page == nullptr)
    throw std::bad_alloc();
  cur_page->next = new (page) Header{sizeof(Header), page, nullptr};
  cur_page = cur_page->next;
}

#ifdef DEBUG_ALLOCATOR
auto Page::dump_stats(std::ostream &out) -> std::ostream & {
  unused_space += page_size - cur_page->amount_used;
  auto const waste_ratio = wasted_space != 0
                               ? static_cast<double>(amount_alloc) /
                                     static_cast<double>(wasted_space)
                               : 0.0;
  auto const num_pages = [&]() {
    auto num_pages = size_t{};
    for (auto page = &start; page; page = page->next) {
      ++num_pages;
    }
    return num_pages;
  }();
  std::cout << std::format(
      "Allocated [{:*>8}] bytes, wasted [{:*>8}] bytes "
      "({:.2f}%), unused = [{:*>8}] bytes took [{}] pages\n",
      amount_alloc, wasted_space, waste_ratio, unused_space, num_pages);
  return out;
}
#endif // DEBUG_ALLOCATOR
} // namespace luamake::allocator
