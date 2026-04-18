#include "luamake_spiral.hpp"

#include "common.hpp"
#include "luamake_builtins.hpp"
#include "luamake_file.hpp"
#include "luamake_strings.hpp"

#include <cstddef>
#include <format>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>
#include <variant>

#ifdef DEBUG
#include <iostream>
#endif // DEBUG

// non portable section :)
#include <endian.h>

namespace fs = std::filesystem;

namespace luamake::spl {
struct Serializer {
  template <class T> auto serialize(T) = delete;
  auto serialize(builtins::Module const &) -> void;

  auto serialize(builtins::Module::Module_t const) -> void;
  auto serialize(builtins::Module::DepTree const &) -> void;
  auto serialize(std::span<fs::path const> const) -> void;
  auto serialize(std::string_view const) -> void;

  constexpr auto buffer() const noexcept -> u8 * { return m.buffer.get(); }
  constexpr auto size() const noexcept -> size_t { return m.size; }

private:
  [[nodiscard]]
  auto write(std::integral auto) -> size_t;
  [[nodiscard]]
  auto write(void const *const, size_t) -> size_t;

  auto resize(size_t at_least = 0) -> void;

  struct M {
    std::unique_ptr<u8[]> buffer;
    size_t size;
    size_t cap;
  } m;
};

auto Serializer::write(std::integral auto t) -> size_t {
  if (m.cap + sizeof(t) >= m.size)
    resize(sizeof(t));
  // TODO: idk if we actually need to fill out this table because we'll only be
  // using it with 64bit (8 byte) numbers
  switch (sizeof(t)) {
  case 1: {
    // shouldn't need to do any byte flipping because this is a single byte
    memcpy(m.buffer.get() + m.size, &t, sizeof(t));
    return sizeof(t);
  } break;
  case 8: {
    auto const tmp = htobe64(t);
    memcpy(m.buffer.get() + m.size, &tmp, sizeof(t));
    return sizeof(t);
  } break;
  }

  unreachable();
}

auto Serializer::write(void const *const bytes, size_t n_bytes) -> size_t {
  if (m.size + n_bytes >= m.cap)
    resize(n_bytes);
  memcpy(m.buffer.get() + m.size, bytes, n_bytes);
  return n_bytes;
}

auto Serializer::resize(size_t at_least) -> void {
  auto const new_size = 3 * size() / 2 + at_least;
  auto new_buffer = std::make_unique<u8[]>(new_size);
  memcpy(new_buffer.get(), buffer(), size());
  m.buffer = std::move(new_buffer);
  m.size = new_size;
}

auto Serializer::serialize(builtins::Module const &mod) -> void {
  serialize(mod.type);
  serialize(mod.tree);

  serialize(std::span<fs::path const>(mod.roots));
  serialize(std::span<fs::path const>(mod.headers));
  serialize(std::span<fs::path const>(mod.includes));
  serialize(std::span<fs::path const>(mod.dep_includes));
  serialize(std::span<fs::path const>(mod.sys_includes));
  serialize(std::span<fs::path const>(mod.linking));
  // TODO: serialize pp::Interpreter
  serialize(std::string_view{mod.compiler});
  serialize(std::string_view{mod.name});
  serialize(std::string_view{mod.install_dir});
}

auto Serializer::serialize(builtins::Module::Module_t const t) -> void {
  auto const byte =
      static_cast<std::underlying_type_t<builtins::Module::Module_t>>(t);
  m.size += write(byte);
}

auto Serializer::serialize(builtins::Module::DepTree const &tree) -> void {
  serialize(tree.all_paths.view());
  m.size += write(tree.num_files);
  m.size +=
      write(tree.types.get(), sizeof(decltype(tree.types[0])) * tree.num_files);
  m.size +=
      write(tree.files.get(), sizeof(decltype(tree.files[0])) * tree.num_files);
  auto constexpr tree_dep_size = sizeof(decltype(tree.deps[0][0]));
  for (auto i = size_t{}; i < tree.num_files; ++i) {
    auto const dep_size = tree.deps[i].size();
    m.size += write(dep_size);
    m.size += write(tree.deps[i].data(), dep_size * tree_dep_size);
  }
  m.size += write(tree.hashes.get(),
                  sizeof(decltype(tree.hashes[0])) * tree.num_files);
}

auto Serializer::serialize(std::span<fs::path const> const span) -> void {
  auto const span_size = span.size();
  m.size += write(span_size);
  for (auto i = size_t{}; i < span_size; ++i) {
    // this this causes an issue, but we'll test it
    serialize(std::string_view(span[i].string()));
    // auto const &str = span[i].string();
    // serialize(std::string_view(str));
  }
}

auto Serializer::serialize(std::string_view const str) -> void {
  auto const str_size = str.size();
  m.size += write(str_size);
  m.size += write(str.data(), str_size * sizeof(char));
}

struct Deserializer {
  Deserializer(fs::path const &path);

  template <class T> auto deserialize() -> T = delete;
  auto deserialize() -> builtins::Module;
};

auto serialize(builtins::Module const &mod, std::filesystem::path const &path)
    -> void {
  // this just kind of sticks around, when we only really need it at the end,
  // but if there's some issue opening the file, then we'd want to error out
  // early(?)
  auto outfile = File(path, File::WRITE | File::CREATE);
  if (!outfile)
    return;

  auto cereal = Serializer();

  cereal.serialize(mod);

  outfile.write(cereal.buffer(), cereal.size(), 1);
  outfile.flush();
#ifdef DEBUG
  std::cout << std::format("serialized file [{}] with [{}] bytes\n",
                           path.string(), cereal.size());

#endif // DEBUG
}

// will also throw if there's some big error
auto deserialize(fs::path const &path)
    -> std::variant<builtins::Module, std::string> {
  auto file = File(path, File::READ | File::BINARY);
  if (!file)
    return std::format("unable to open serialization file [{}]", path.string());
#ifdef DEBUG
  std::cout << std::format("deserializing file [{}]\n", path.string());
#endif // DEBUG
  auto decereal = Deserializer(path);
  return decereal.deserialize<builtins::Module>();
}
} // namespace luamake::spl
