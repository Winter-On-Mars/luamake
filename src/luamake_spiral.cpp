#include "luamake_spiral.hpp"

#include "common.hpp"
#include "luamake_builtins.hpp"
#include "luamake_file.hpp"
#include "luamake_strings.hpp"

#include <concepts>
#include <cstddef>
#include <filesystem>
#include <format>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#ifdef DEBUG
#include <iostream>
#endif // DEBUG

// non portable section :)
#include <endian.h>

namespace fs = std::filesystem;

namespace luamake::spl {
namespace {
struct ByteBuffer final {
  ByteBuffer() noexcept;
  ByteBuffer(std::unique_ptr<u8[]> &&, size_t const) noexcept;
  std::unique_ptr<u8[]> buffer = nullptr;
  size_t cur = 0;
  size_t cap = 0;

  auto resize(size_t at_least = 0) -> void;
};

ByteBuffer::ByteBuffer() noexcept
    : buffer(std::make_unique<u8[]>(2 << 12)), cur(0), cap(2 << 12) {}

ByteBuffer::ByteBuffer(std::unique_ptr<u8[]> &&buffer,
                       size_t const size) noexcept
    : buffer(std::move(buffer)), cur(0), cap(size) {}

auto ByteBuffer::resize(size_t at_least) -> void {
  auto const new_cap = 3 * cap / 2 + at_least;
  auto new_buffer = std::make_unique<u8[]>(new_cap);
  memcpy(new_buffer.get(), buffer.get(), cur);
  buffer = std::move(new_buffer);
  cap = new_cap;
}

auto write(...) -> void = delete;

auto write(ByteBuffer &bytes, std::integral auto t) -> void {
  if (bytes.cur + sizeof(t) >= bytes.cap)
    bytes.resize(sizeof(t));
  switch (sizeof(t)) {
  case 1: {
    // shouldn't need to do any byte flipping because this is a single byte
    memcpy(bytes.buffer.get() + bytes.cur, &t, sizeof(t));
  } break;
  case 2: {
    auto const tmp = htobe16(t);
    memcpy(bytes.buffer.get() + bytes.cur, &tmp, sizeof(t));
  }
  case 4: {
    auto const tmp = htobe32(t);
    memcpy(bytes.buffer.get() + bytes.cur, &tmp, sizeof(t));
  } break;
  case 8: {
    auto const tmp = htobe64(t);
    memcpy(bytes.buffer.get() + bytes.cur, &tmp, sizeof(t));
  } break;
  }
  bytes.cur += sizeof(t);
}

// TODO: idk probably change this to a span(?)
// also change this to writev (?)
// TODO: update this to not take the n_bytes, but n_things(?), or update the
// readv function to also take the num_bytes to be consistent(?)
auto write(ByteBuffer &bytes, void const *const thing, size_t n_bytes) -> void {
  if (bytes.cur + n_bytes >= bytes.cap)
    bytes.resize(n_bytes);
  memcpy(bytes.buffer.get() + bytes.cur, thing, n_bytes);
  bytes.cur += n_bytes;
}

auto check(ByteBuffer &bytes, size_t const amount, std::string_view const type)
    -> void {
  [[unlikely]]
  if (bytes.cur + amount > bytes.cap) {
    throw std::runtime_error(std::format(
        "Attempting to read [{}] bytes for [{}], but not enough bytes "
        "available in buffer. Current position in buffer = [{}], size of "
        "buffer = [{}].",
        amount, type, bytes.cur, bytes.cap));
  }
}

template <class T>
[[nodiscard]]
auto read(ByteBuffer &) -> T = delete;

// TODO: see if we can combine these into one template, like we do with read,
// i'm not sure if we can because the return type of each is different(?)
template <>
[[nodiscard]]
auto read<size_t>(ByteBuffer &bytes) -> size_t {
  check(bytes, sizeof(size_t), "size_t");
  auto res = size_t{};
  memcpy(&res, bytes.buffer.get() + bytes.cur, sizeof(size_t));
  res = be64toh(res);
  bytes.cur += sizeof(size_t);
  return res;
}

template <>
[[nodiscard]]
auto read<uint>(ByteBuffer &bytes) -> uint {
  check(bytes, sizeof(uint), "uint");
  auto res = uint{};
  memcpy(&res, bytes.buffer.get() + bytes.cur, sizeof(uint));
  res = be32toh(res);
  bytes.cur += sizeof(uint);
  return res;
}

template <>
[[nodiscard]]
auto read<u8>(ByteBuffer &bytes) -> u8 {
  check(bytes, sizeof(u8), "u8");
  auto res = u8{};
  memcpy(&res, bytes.buffer.get() + bytes.cur, sizeof(u8));
  bytes.cur += sizeof(u8);
  return res;
}

template <class T>
[[nodiscard]]
auto readv(ByteBuffer &bytes, size_t const size) -> std::unique_ptr<T[]> {
  check(bytes, size * sizeof(T), "vectorized read");
  auto res = std::make_unique<T[]>(size);
  memcpy(res.get(), bytes.buffer.get() + bytes.cur, size * sizeof(T));
  bytes.cur += size * sizeof(T);
  return res;
}
} // namespace

// TODO: idk just move the ByteBuffer out that way we don't need to pass it to
// every ctor
template <class T> struct Serializer<std::span<T>> {
  Serializer(ByteBuffer &bytes) noexcept : bytes(bytes) {}
  auto serialize(std::span<T const>) -> void;
  ByteBuffer &bytes;
};

template <typename T>
  requires std::convertible_to<T, std::string_view>
struct Serializer<T> {
  Serializer(ByteBuffer &bytes) noexcept : bytes(bytes) {}
  auto serialize(std::string_view const) -> void;
  ByteBuffer &bytes;
};

template <class T>
auto Serializer<std::span<T>>::serialize(std::span<T const> const span)
    -> void {
  auto const span_size = span.size();
  write(bytes, span_size);
  auto str_serializer = Serializer<std::string_view>(bytes);
  for (auto i = size_t{}; i < span_size; ++i) {
    // this this causes an issue, but we'll test it
    str_serializer.serialize(std::string_view(span[i].string()));
  }
}

template <class T>
  requires std::convertible_to<T, std::string_view>
auto Serializer<T>::serialize(std::string_view const str) -> void {
  auto const str_size = str.size();
  write(bytes, str_size);
  write(bytes, str.data(), str_size * sizeof(char));
}

// NOTE: there should be a way to take these functions out of line, but i can't
// figure out how without an error about no matching function templates
template <> struct Serializer<builtins::Module::Module_t> {
  Serializer(ByteBuffer &bytes) noexcept : bytes(bytes) {}
  auto serialize(builtins::Module::Module_t const t) -> void {
    auto const byte =
        static_cast<std::underlying_type_t<builtins::Module::Module_t>>(t);
    write(bytes, byte);
  }
  ByteBuffer &bytes;
};

template <> struct Serializer<builtins::Module::DepTree> {
  Serializer(ByteBuffer &bytes) noexcept : bytes(bytes) {}

  // TODO: probably have to actually do endian swap here (?) for all of the
  // types
  auto serialize(builtins::Module::DepTree const &tree) -> void {
    auto str_serializer = Serializer<std::string_view>(bytes);
    str_serializer.serialize(tree.all_paths.view());
    write(bytes, tree.num_files);
    write(bytes, tree.types.get(),
          sizeof(decltype(tree.types[0])) * tree.num_files);
    write(bytes, tree.files.get(),
          sizeof(decltype(tree.files[0])) * tree.num_files);
    for (auto i = size_t{}; i < tree.num_files; ++i) {
      auto const dep_size = tree.deps[i].size();
      write(bytes, dep_size);
      for (auto &&dep : tree.deps[i]) {
        // NOTE: we have to be sure to actually do the endian swap :(
        write(bytes, dep);
      }
    }
    // fuck we also have to do endian swap here
    write(bytes, tree.hashes.get(),
          sizeof(decltype(tree.hashes[0])) * tree.num_files);
  }

  ByteBuffer &bytes;
};

// NOTE: if we were writing a library, then this would be something that we
// write in the luamake_builtins file, and we just expose an api for it, not
// saying i won't do that, but for now we're just trying to make this work with
// gcc
template <> struct Serializer<builtins::Module> {
  Serializer(ByteBuffer &bytes) noexcept : bytes(bytes) {}
  auto serialize(builtins::Module const &mod) -> void {
    auto type_serializer = Serializer<builtins::Module::Module_t>(bytes);
    type_serializer.serialize(mod.type);

    auto tree_serializer = Serializer<builtins::Module::DepTree>(bytes);
    tree_serializer.serialize(mod.tree);

    auto span_serializer = Serializer<std::span<fs::path>>(bytes);
    span_serializer.serialize(mod.roots);
    span_serializer.serialize(mod.headers);
    span_serializer.serialize(mod.includes);
    span_serializer.serialize(mod.sys_includes);
    span_serializer.serialize(mod.linking);

    auto str_serializer = Serializer<std::string_view>(bytes);
    str_serializer.serialize(std::string_view{mod.compiler});
    str_serializer.serialize(std::string_view{mod.name});
    str_serializer.serialize(std::string_view{mod.install_dir});
  }

  ByteBuffer &bytes;
};

template <> struct Deserializer<std::string> {
  Deserializer(ByteBuffer &bytes) noexcept : bytes(bytes) {}
  auto deserialize() -> std::string {
    auto res = std::string();
    auto const size = read<size_t>(bytes);
    check(bytes, size * sizeof(char), "std::string");
    res.reserve(size);
    res.assign(bytes.buffer.get() + bytes.cur,
               bytes.buffer.get() + bytes.cur + size);
    bytes.cur += size * sizeof(char);
    return res;
  }
  ByteBuffer &bytes;
};

template <> struct Deserializer<std::vector<fs::path>> {
  Deserializer(ByteBuffer &bytes) noexcept : bytes(bytes) {}
  auto deserialize() -> std::vector<fs::path> {
    auto res = std::vector<fs::path>();
    auto const size = read<size_t>(bytes);
    res.reserve(size);
    auto str_deserial = Deserializer<std::string>(bytes);
    for (auto i = size_t{}; i < size; ++i) {
      res.emplace_back(fs::path(str_deserial.deserialize()));
    }
    return res;
  }
  ByteBuffer &bytes;
};

template <> struct Deserializer<builtins::Module::Module_t> {
  Deserializer(ByteBuffer &bytes) noexcept : bytes(bytes) {}
  auto deserialize() -> builtins::Module::Module_t {
    return static_cast<builtins::Module::Module_t>(
        read<std::underlying_type_t<builtins::Module::Module_t>>(bytes));
  }
  ByteBuffer &bytes;
};

template <> struct Deserializer<OwnedString> {
  Deserializer(ByteBuffer &bytes) noexcept : bytes(bytes) {}
  auto deserialize() -> OwnedString {
    auto const size = read<size_t>(bytes);
    check(bytes, size * sizeof(char), "OwnedString");
    auto buf = static_cast<char *>(malloc(size * sizeof(char)));
    auto res = OwnedString(buf, size);
    memcpy(res.buffer, bytes.buffer.get() + bytes.cur, size * sizeof(char));
    res.size = size;
    bytes.cur += size * sizeof(char);
    return res;
  }
  ByteBuffer &bytes;
};

template <> struct Deserializer<builtins::Module::DepTree> {
  Deserializer(ByteBuffer &bytes) noexcept : bytes(bytes) {}
  auto deserialize() -> builtins::Module::DepTree {
    auto res = builtins::Module::DepTree();
    auto owned_str_deserial = Deserializer<OwnedString>(bytes);
    res.all_paths = owned_str_deserial.deserialize();
    res.num_files = read<size_t>(bytes);
    res.types =
        readv<builtins::Module::DepTree::SourceFile_t>(bytes, res.num_files);
    res.files = readv<StringViews>(bytes, res.num_files);
    res.deps = std::make_unique<std::vector<uint>[]>(res.num_files);
    for (auto i = size_t{}; i < res.num_files; ++i) {
      auto const size = read<size_t>(bytes);
      res.deps[i] = std::vector<uint>(size);
      // res.deps[i].reserve(size);
      for (auto j = size_t{}; j < size; ++j) {
        res.deps[i][j] = read<uint>(bytes);
      }
    }
    res.hashes = readv<size_t>(bytes, res.num_files);
    return res;
  }
  ByteBuffer &bytes;
};

template <> struct Deserializer<builtins::Module> {
  Deserializer(ByteBuffer &bytes) noexcept : bytes(bytes) {}
  auto deserialize() -> builtins::Module {
    auto mod = builtins::Module();

    auto mod_t_deserial = Deserializer<builtins::Module::Module_t>(bytes);
    mod.type = mod_t_deserial.deserialize();

    auto tree_deserial = Deserializer<builtins::Module::DepTree>(bytes);
    mod.tree = tree_deserial.deserialize();

    auto vec_deserial = Deserializer<std::vector<fs::path>>(bytes);
    mod.roots = vec_deserial.deserialize();
    mod.headers = vec_deserial.deserialize();
    mod.includes = vec_deserial.deserialize();
    mod.sys_includes = vec_deserial.deserialize();
    mod.linking = vec_deserial.deserialize();

    auto str_deserial = Deserializer<std::string>(bytes);
    mod.compiler = str_deserial.deserialize();
    mod.name = str_deserial.deserialize();
    mod.install_dir = str_deserial.deserialize();
    return mod;
  }
  ByteBuffer &bytes;
};

auto serialize(builtins::Module const &mod, std::filesystem::path const &path)
    -> void {
  auto bytes = ByteBuffer();
  auto cereal = Serializer<builtins::Module>(bytes);
  cereal.serialize(mod);

  auto outfile = File(path, File::WRITE | File::CREATE);
  // we should also add some error checking for this
  // idk what we'd do if we can't serialize, like it's not a fatel error, but
  // it's something
  if (!outfile)
    return;
  outfile.write(cereal.bytes.buffer.get(), cereal.bytes.cur, 1);
  outfile.flush();
#ifdef DEBUG
  std::cout << std::format("serialized file [{}] with [{}] bytes" LM_NL,
                           path.string(), cereal.bytes.cur);
#endif // DEBUG
}

// will also throw if there's some big error
auto deserialize(fs::path const &path)
    -> std::variant<builtins::Module, std::string> {
  auto file = File(path, File::READ | File::BINARY);
  if (!file)
    return std::format("unable to open serialization file [{}]", path.string());
#ifdef DEBUG
  std::cout << std::format("deserializing file [{}]" LM_NL, path.string());
#endif // DEBUG
  auto &&[size, buffer] = file.dump_content();
  auto bytes = ByteBuffer(std::move(buffer), size);
  auto decereal = Deserializer<builtins::Module>(bytes);
  return decereal.deserialize();
}
} // namespace luamake::spl
