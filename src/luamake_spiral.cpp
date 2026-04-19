#include "luamake_spiral.hpp"

#include "common.hpp"
#include "luamake_builtins.hpp"
#include "luamake_file.hpp"
#include "luamake_strings.hpp"

#include <concepts>
#include <cstddef>
#include <format>
#include <memory>
#include <span>
#include <stdexcept>
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
  auto write(...) = delete;
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
  case 4: {
    auto const tmp = htobe32(t);
    memcpy(buffer() + m.size, &tmp, sizeof(t));
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
  m.cap = new_size;
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
  // auto constexpr tree_dep_size = sizeof(decltype(tree.deps[0][0]));
  for (auto i = size_t{}; i < tree.num_files; ++i) {
    auto const dep_size = tree.deps[i].size();
    m.size += write(dep_size);
    for (auto &&dep : tree.deps[i]) {
      // we have to be sure to actually do the endian swap
      m.size += write(dep);
    }
    // m.size += write(tree.deps[i].data(), dep_size * tree_dep_size);
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
  Deserializer(File &file);

  template <class T> auto deserialize() -> T = delete;
  template <> auto deserialize<builtins::Module>() -> builtins::Module;

private:
  template <>
  auto deserialize<builtins::Module::Module_t>() -> builtins::Module::Module_t;
  template <>
  auto deserialize<builtins::Module::DepTree>() -> builtins::Module::DepTree;
  template <>
  auto deserialize<std::vector<fs::path>>() -> std::vector<fs::path>;
  template <> auto deserialize<std::string>() -> std::string;
  template <> auto deserialize<OwnedString>() -> OwnedString;

  template <class T>
  [[nodiscard]]
  auto read() -> T = delete;

  template <>
  [[nodiscard]]
  auto read<size_t>() -> size_t;

  template <>
  [[nodiscard]]
  auto read<uint>() -> uint;

  template <>
  [[nodiscard]]
  auto read<u8>() -> u8;

  template <class T>
  [[nodiscard]]
  auto readv(size_t const) -> std::unique_ptr<T[]>;

  auto check(size_t const, std::string_view const) const -> void;

  auto constexpr buffer() const noexcept -> u8 * { return m.buf.get(); }

  struct M {
    std::unique_ptr<u8[]> buf = nullptr;
    size_t cur = 0;
    size_t size = 0;
  } m;
};

Deserializer::Deserializer(File &file) : m() {
  auto &&[size, buf] = file.dump_content();
  m.size = size;
  m.buf = std::move(buf);
}

template <> auto Deserializer::read<size_t>() -> size_t {
  check(sizeof(size_t), "size_t");
  auto res = size_t{};
  memcpy(&res, buffer() + m.cur, sizeof(size_t));
  // this *should* be the same as if we did an if constexpr branch
  res = be64toh(res);
  m.cur += sizeof(size_t);
  return res;
}

template <> auto Deserializer::read<uint>() -> uint {
  check(sizeof(uint), "uint");
  auto res = uint{};
  memcpy(&res, buffer() + m.cur, sizeof(uint));
  // this *should* be the same as if we did an if constexpr branch
  res = be32toh(res);
  m.cur += sizeof(uint);
  return res;
}

template <> auto Deserializer::read<u8>() -> u8 {
  check(sizeof(u8), "byte");
  auto res = u8{};
  // could probably just static_cast<u8>(buffer() + m.cur) (?), it'd almost
  // certainly be faster, i just don't know if it would 100% work
  memcpy(&res, buffer() + m.cur, sizeof(u8));
  m.cur += sizeof(u8);
  return res;
}

template <class T>
auto Deserializer::readv(size_t const size) -> std::unique_ptr<T[]> {
  check(size * sizeof(T), "vectorized read");
  auto res = std::make_unique<T[]>(size);
  memcpy(res.get(), buffer() + m.cur, size * sizeof(T));
  m.cur += size * sizeof(T);
  return res;
}

auto Deserializer::check(size_t const amount, std::string_view const type) const
    -> void {
  [[unlikely]]
  if (m.cur + amount > m.size) {
    throw std::runtime_error(std::format(
        "Attempting to read [{}] bytes for [{}], but not enough bytes "
        "available in buffer. Current position in buffer = [{}], size of "
        "buffer = [{}].",
        amount, type, m.cur, m.size));
  }
}

template <>
auto Deserializer::deserialize<builtins::Module>() -> builtins::Module {
  auto mod = builtins::Module();
  mod.type = deserialize<builtins::Module::Module_t>();
  mod.tree = deserialize<builtins::Module::DepTree>();
  mod.roots = deserialize<std::vector<fs::path>>();
  mod.headers = deserialize<std::vector<fs::path>>();
  mod.includes = deserialize<std::vector<fs::path>>();
  mod.dep_includes = deserialize<std::vector<fs::path>>();
  mod.sys_includes = deserialize<std::vector<fs::path>>();
  mod.linking = deserialize<std::vector<fs::path>>();
  // TODO: deserialize pp::Interpreter
  mod.compiler = deserialize<std::string>();
  mod.name = deserialize<std::string>();
  mod.install_dir = deserialize<std::string>();
  return mod;
}

template <>
auto Deserializer::deserialize<builtins::Module::Module_t>()
    -> builtins::Module::Module_t {
  return static_cast<builtins::Module::Module_t>(
      read<std::underlying_type_t<builtins::Module::Module_t>>());
}

template <>
auto Deserializer::deserialize<builtins::Module::DepTree>()
    -> builtins::Module::DepTree {
  auto res = builtins::Module::DepTree();
  res.all_paths = deserialize<OwnedString>();
  res.num_files = read<size_t>();
  res.types = readv<builtins::Module::DepTree::SourceFile_t>(res.num_files);
  res.files = readv<StringViews>(res.num_files);
  res.deps = std::make_unique<std::vector<uint>[]>(res.num_files);
  for (auto i = size_t{}; i < res.num_files; ++i) {
    auto const size = read<size_t>();
    res.deps[i] = std::vector<uint>(size);
    // res.deps[i].reserve(size);
    for (auto j = size_t{}; j < size; ++j) {
      res.deps[i][j] = read<uint>();
    }
  }
  res.hashes = readv<size_t>(res.num_files);
  return res;
}

template <>
auto Deserializer::deserialize<std::vector<fs::path>>()
    -> std::vector<fs::path> {
  auto res = std::vector<fs::path>();
  auto const size = read<size_t>();
  res.reserve(size);
  for (auto i = size_t{}; i < size; ++i) {
    res.emplace_back(fs::path(deserialize<std::string>()));
  }
  return res;
}

template <> auto Deserializer::deserialize<std::string>() -> std::string {
  auto res = std::string();
  auto const size = read<size_t>();
  check(size * sizeof(char), "std::string");
  res.reserve(size);
  res.assign(buffer() + m.cur, buffer() + m.cur + size);
  m.cur += size * sizeof(char);
  return res;
}

template <> auto Deserializer::deserialize<OwnedString>() -> OwnedString {
  auto const size = read<size_t>();
  check(size * sizeof(char), "OwnedString");
  auto buf = static_cast<char *>(malloc(size * sizeof(char)));
  auto res = OwnedString(buf, size);
  memcpy(res.buffer, buffer() + m.cur, size * sizeof(char));
  res.size = size;
  m.cur += size * sizeof(char);
  return res;
}

auto serialize(builtins::Module const &mod, std::filesystem::path const &path)
    -> void {
  auto cereal = Serializer();
  cereal.serialize(mod);

  auto outfile = File(path, File::WRITE | File::CREATE);
  // we should also add some error checking for this
  // idk what we'd do if we can't serialize, like it's not a fatel error, but
  // it's something
  if (!outfile)
    return;
  outfile.write(cereal.buffer(), cereal.size(), 1);
  outfile.flush();
#ifdef DEBUG
  std::cout << std::format("serialized file [{}] with [{}] bytes" NL,
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
  std::cout << std::format("deserializing file [{}]" NL, path.string());
#endif // DEBUG
  auto decereal = Deserializer(file);
  return decereal.deserialize<builtins::Module>();
}
} // namespace luamake::spl
