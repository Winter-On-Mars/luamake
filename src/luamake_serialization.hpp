#ifndef __LUAMAKE_SERIALIZATION_HPP
#define __LUAMAKE_SERIALIZATION_HPP

#include "common.hpp"
#include "luamake_file.hpp"
#include "luamake_strings.hpp"

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace luamake {

// NOTE: a lot of libfmt templates off the char type, idk if that's something we
// should do as well?
struct SerializeContext;
namespace {
enum class type {
  none_type,
  // Integer types should go first,
  int_type,
  uint_type,
  long_long_type,
  ulong_long_type,
  int128_type,
  uint128_type,
  bool_type,
  char_type,
  last_integer_type = char_type,
  // followed by floating-point types.
  float_type,
  double_type,
  long_double_type,
  last_numeric_type = long_double_type,
  cstring_type,
  string_type,
  pointer_type,
  custom_type
};

struct Monostate final {
  constexpr Monostate() {}
};

template <class T>
struct type_constant : std::integral_constant<type, type::custom_type> {};

#define TYPE_CONSTANT(Type, constant)                                          \
  template <>                                                                  \
  struct type_constant<Type> : std::integral_constant<type, type::constant> {}

TYPE_CONSTANT(int, int_type);
TYPE_CONSTANT(unsigned, uint_type);
TYPE_CONSTANT(long long, long_long_type);
TYPE_CONSTANT(unsigned long long, ulong_long_type);
TYPE_CONSTANT(bool, bool_type);
TYPE_CONSTANT(char, char_type);
TYPE_CONSTANT(float, float_type);
TYPE_CONSTANT(double, double_type);
TYPE_CONSTANT(long double, long_double_type);
TYPE_CONSTANT(const char *, cstring_type);
TYPE_CONSTANT(std::basic_string_view<char>, string_type);
TYPE_CONSTANT(const void *, pointer_type);

constexpr auto is_integral_type(type t) -> bool {
  return t > type::none_type && t <= type::last_integer_type;
}
constexpr auto is_arithmetic_type(type t) -> bool {
  return t > type::none_type && t <= type::last_numeric_type;
}

// source:
// [[https://stackoverflow.com/questions/60449592/how-do-you-define-a-c-concept-for-the-standard-library-containers]]
template <class ContainerType>
concept Container = requires(ContainerType a, const ContainerType b) {
  requires std::regular<ContainerType>;
  requires std::swappable<ContainerType>;
  requires std::destructible<typename ContainerType::value_type>;
  requires std::same_as<typename ContainerType::reference,
                        typename ContainerType::value_type &>;
  requires std::same_as<typename ContainerType::const_reference,
                        const typename ContainerType::value_type &>;
  requires std::forward_iterator<typename ContainerType::iterator>;
  requires std::forward_iterator<typename ContainerType::const_iterator>;
  requires std::signed_integral<typename ContainerType::difference_type>;
  requires std::same_as<typename ContainerType::difference_type,
                        typename std::iterator_traits<
                            typename ContainerType::iterator>::difference_type>;
  requires std::same_as<
      typename ContainerType::difference_type,
      typename std::iterator_traits<
          typename ContainerType::const_iterator>::difference_type>;
  { a.begin() } -> std::same_as<typename ContainerType::iterator>;
  { a.end() } -> std::same_as<typename ContainerType::iterator>;
  { b.begin() } -> std::same_as<typename ContainerType::const_iterator>;
  { b.end() } -> std::same_as<typename ContainerType::const_iterator>;
  { a.cbegin() } -> std::same_as<typename ContainerType::const_iterator>;
  { a.cend() } -> std::same_as<typename ContainerType::const_iterator>;
  { a.size() } -> std::same_as<typename ContainerType::size_type>;
  { a.max_size() } -> std::same_as<typename ContainerType::size_type>;
  { a.empty() } -> std::same_as<bool>;
};

// ironically this could probably be moved into luamake::strings, and it'd be
// more platform agnostic, just with a macro checking if we're on windows, and
// taking FixedString = Stringvalue<wchar_t>; and stuff like that
template <typename Char> struct StringValue {
  Char const *buff;
  size_t size;

  auto str() const noexcept -> std::basic_string_view<Char> {
    return std::basic_string_view<Char>{buff, size};
  }
};

template <class Context> struct CustomValue {
  using format_func = auto (*)(void *const, SerializeContext &, Context &)
      -> void;
  void *arg;
  format_func func;
};

template <class Context> struct Value {
  using char_t = typename Context::char_t;

  union {
    Monostate monostate;
    int i;
    unsigned int u;
    long long ll;
    unsigned long long ull;
    bool b;
    char ch;
    float f;
    double d;
    long double ld;
    StringValue<char_t> str;
    void const *ptr;
    CustomValue<Context> custom;
  };
};

template <class Context> struct BasicSerializeArg {
  Value<Context> val;
  type t;
};

template <class Context, size_t num_args>
using arg_t =
    std::conditional_t<true, BasicSerializeArg<Context>, Value<Context>>;
} // namespace

// all of this code architecture is stolen from fmt::format
// [[https://github.com/fmtlib/fmt]]
struct SerializeContext;

// probably rewrite this, we don't really need a ByteSlice class, bc we don't
// really need a no owning view, as far as i know, plus then we'd have to deal
// with pointer invalidation
class ByteSlice {
public:
  auto constexpr size() const noexcept -> size_t { return m_size; }
  auto constexpr data() const noexcept -> u8 * { return m_buffer; }

protected:
  ByteSlice(size_t size, u8 *ptr) noexcept : m_size(size), m_buffer(ptr) {}

private:
  size_t m_size;
  u8 *m_buffer;
};

template <class Allocator>
struct AbstractByteBuffer final : public ByteSlice, private Allocator {
  using Alloc = Allocator;

  AbstractByteBuffer(Allocator &&alloc_args) noexcept
      : ByteSlice(1 << 12, nullptr), Allocator(alloc_args), cur(0) {
    data() = allocate(size());

    if (data() == nullptr)
      throw;
  }

  constexpr ~AbstractByteBuffer() { this->deallocate(data(), size()); }

private:
  // TODO: either add a func overload or another function to resize_atleast,
  // that takes in a number of bytes that we need to resize the value to at
  // least
  auto resize() noexcept -> void {
    auto const new_size = 3 * size() / 2;
    auto new_buf = allocate(new_size);

    std::memcpy(new_buf, data(), size());
    deallocate(data(), size());
    this->ByteSlice = ByteSlice(new_size, new_buf);
  }

  size_t cur;

  friend SerializeContext;
};

using ByteBuffer = AbstractByteBuffer<std::allocator<u8>>;

struct SerializeContext final {
  using char_t = char;

  std::string_view fmt;
  size_t next_arg = 0;

  // TODO
  auto out() -> char_t const *;
};

template <class T, typename Enable = void> struct Serializer {
  Serializer() = delete;
};

template <class T>
  requires(std::is_integral_v<T>)
struct NativeSerializer {
  auto specifier(SerializeContext &ctx) -> decltype(ctx.out()) {}

  template <class SerializeContext>
  auto serialize(T t, SerializeContext &ctx) -> decltype(ctx.out()) {
    return ctx.write(&t, sizeof(T));
  }
};

template <class T>
struct Serializer<
    T, std::enable_if_t<type_constant<T>::value != type::custom_type, void>>
    : NativeSerializer<T> {};

template <class T>
  requires Container<T>
struct Serializer<T, void> {
  auto specifier(SerializeContext &ctx) -> decltype(ctx.out()) {}

  auto serialize(T const &container, SerializeContext &) {}
};

// TODO: optimize this struct
template <class Context> struct BasicSerializeArgs final {
  using char_t = char;

  size_t num_args;
  Value<Context> const *values;
};

using SerializeArgs = BasicSerializeArg<SerializeContext>;

namespace {
struct BufferedContext final {
  ByteBuffer &buffer;
  SerializeArgs args;
};

template <class Context, size_t num_args> struct SerializeArgStore {
  arg_t<Context, num_args> args[num_args];
};

template <class... T>
using sargs = SerializeArgStore<SerializeContext, sizeof...(T)>;

template <class Context> struct SerializeHandler {
  SerializeContext serial_ctx;
  Context ctx;

  auto copy_bytes(char const *from, char const *to) noexcept -> void {
    // TODO:
    ctx.data();
  }
};

template <class Context>
auto parse_serialize_string(std::string_view const str,
                            SerializeHandler<Context> &&handler) -> void {
  // TODO: this is where we actually like loop over the string and do shit
  // we'll allow the user to put in arbitrary text, we just have to make sure
  // that it's only hex values or something like that
  auto begin = str.begin();
  auto end = str.end();
  for (auto cur = begin; cur != end;) {
    auto const ch = *cur;
    switch (ch) {
    case '{': {
      handler.copy_bytes(begin, cur - 1);
      begin = ++cur;
    } break;
    case '}': {
      // TODO: we now have to pass these args to the Context, so that we can
      // determine the serialization formatting
    } break;
    default:
      ++cur;
    }
  }
}

template <class... T>
auto serialize_impl(std::string_view const fmt, SerializeArgs args)
    -> ByteBuffer {
  auto byte_buffer = ByteBuffer(ByteBuffer::Alloc{});
  parse_serialize_string(
      fmt, SerializeHandler<BufferedContext>{
               SerializeContext{fmt}, BufferedContext{byte_buffer, args}});
  return byte_buffer;
}
} // namespace

// TODO: change the string input from a std::string_view to a custom type that
// does compile time checking to make sure we only have either hex input or `{}`
// chars with values in them, then let it cast to a std::string_view or whatever
/**
 * auto x = serialize("{}{}{}", foo.x, foo.y, foo.z);
 * with `i` as an int and `f` as a float
 * auto buf_i = serialize("{0:ne}{0:be}{0:le}", i);
 * auto buf_f = serialize("{0:ne}{0:be}{0:le}", f);
 * file.write(buf_i.buffer(), buf_i.size());
 */
template <class... T>
auto serialize(std::string_view const fmt, T &&...args) noexcept -> ByteBuffer {
  // try using std::forward
  return serialize_impl(fmt, sargs<T...>{{args...}});
}

template <class SourceIter, class T> auto deserialize() -> T;

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
  Deserializer(Deserializer const &) = delete;
  Deserializer &operator=(Deserializer const &) = delete;

private:
  size_t size;
  size_t cur;
  std::unique_ptr<u8[]> buf;
};
} // namespace luamake

#endif // !DEBUG
