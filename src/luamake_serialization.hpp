#ifndef __LUAMAKE_SERIALIZATION_HPP
#define __LUAMAKE_SERIALIZATION_HPP

#include "common.hpp"

#include <cstddef>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <type_traits>

namespace luamake {
// TODO: support vectorization, it should really speed things up when
// serializing the DepTree
struct Serializer final {
  // should be page size, this should be enough to never have to resize, but we
  // still need the resize funcs for completeness, might also be a good idea to
  // change this to be platform dependant, just on my system page size is 4kb
  Serializer() noexcept
      : cap(1 << 12), size(0), buf(std::make_unique<u8[]>(cap)) {}
  ~Serializer() noexcept = default;

  // there should be a better way of doing this, that allows for partial
  // specialization if we use structs with an overloaded operator(), but idk how
  // to really do that
  template <class T>
    requires(std::is_trivial_v<std::remove_cv_t<T>>)
  inline auto serialize(T) noexcept -> Serializer & = delete;

  // used for string_view, and other view types that are not technically
  // trivial, but are trivially copyable
  template <class T>
    requires(std::is_trivially_copyable_v<std::remove_cv_t<T>> &&
             !std::is_trivial_v<std::remove_cv_t<T>>)
  inline auto serialize(T const) noexcept -> Serializer & = delete;

  template <class T>
    requires(!std::is_trivial_v<std::remove_cv_t<T>> &&
             !std::is_trivially_copyable_v<std::remove_cv_t<T>>)
  inline auto serialize(T const &) noexcept -> Serializer & = delete;

  auto buffer() noexcept -> std::pair<size_t, std::unique_ptr<u8[]>> {
    // this probably doesn't do what i want it to do :)
    return std::make_pair(size, std::move(buf));
  }

  // this might not be right
  Serializer(Serializer &&) = default;
  Serializer &operator=(Serializer &&) = default;

  Serializer(Serializer const &) = delete;
  Serializer &operator=(Serializer const &) = delete;

private:
  // TODO: either add a func overload or another function to resize_atleast,
  // that takes in a number of bytes that we need to resize the value to at
  // least
  auto resize() noexcept -> void {
    auto const new_cap = 3 * cap / 2;
    auto new_buf = std::make_unique<u8[]>(new_cap);

    std::memcpy(new_buf.get(), buf.get(), cap);

    buf = std::move(new_buf);
    cap = new_cap;
  }

  size_t cap;
  size_t size;
  std::unique_ptr<u8[]> buf;
};

struct Deserializer final {
  Deserializer(File &file) : size(0), cur(0), buf(nullptr) {
    auto &&[tmp_size, tmp_buf] = file.dump_content();
    if (tmp_buf == nullptr) {
      throw std::runtime_error("Unable to read file content");
    }
    size = tmp_size;
    buf = std::move(tmp_buf);
  };
  ~Deserializer() noexcept = default;

  template <class T> inline auto deserialize() noexcept -> T = delete;

  // this might not be right
  Deserializer(Deserializer &&) = default;
  Deserializer &operator=(Deserializer &&) = default;

  Deserializer() noexcept = delete;
  Deserializer(Serializer const &) = delete;
  Deserializer &operator=(Serializer const &) = delete;

private:
  size_t size;
  size_t cur;
  std::unique_ptr<u8[]> buf;
};
} // namespace luamake

#endif // !DEBUG
