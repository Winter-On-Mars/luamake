#ifndef __LUAMAKE_ERROR_HPP
#define __LUAMAKE_ERROR_HPP

#include <memory>

template <class Success, class Error> struct Result final {
  struct Ok {
    constexpr Ok(Success &&t) noexcept : res(std::move(t)) {}

  private:
    Success res;
    friend Result<Success, Error>;
  };

  struct Err {
    constexpr Err(Error &&e) noexcept : e(std::move(e)) {}

  private:
    Error e;
    friend Result<Success, Error>;
  };

  enum Type : unsigned char {
    OK,
    ERR,
  };

  auto constexpr ok() const noexcept -> bool { return e == nullptr; }
  constexpr operator Type() const noexcept { return ok() ? OK : ERR; }

  auto get() noexcept -> Success && { return std::move(suc); }
  auto err() noexcept -> Error { return *e; }

  Result() = delete;
  Result(Result const &) = delete;
  Result &operator=(Result const &) = delete;

  constexpr Result(Ok &&suc) noexcept : suc(std::move(suc.res)), e(nullptr) {}
  constexpr Result(Err &&err) noexcept
      : suc(), e(std::make_unique<Error>(err.e)) {}

  constexpr Result(Result &&) = default;
  constexpr Result &operator=(Result &&) = default;

private:
  Success suc;
  std::unique_ptr<Error> e;
};

#endif // !__LUAMAKE_ERROR_HPP
