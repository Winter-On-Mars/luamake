#ifndef __LUAMAKE_ERROR_HPP
#define __LUAMAKE_ERROR_HPP

#include <memory>
#include <utility>

template <class Success, class Error> struct Result;

template <class Error> struct Result<void, Error> final {
  struct Err {
    constexpr Err(Error &&e) noexcept : e(std::move(e)) {}

  private:
    Error e;
    friend Result<void, Error>;
  };

  enum Type : unsigned char {
    OK,
    ERR,
  };

  auto constexpr ok() const noexcept -> bool { return e == nullptr; }
  constexpr operator Type() const noexcept { return ok() ? OK : ERR; }

  auto err() noexcept -> std::unique_ptr<Error> { return std::move(e); }

  constexpr Result() noexcept : e(nullptr) {}
  constexpr Result(Err &&err) noexcept : e(std::make_unique<Error>(err.e)) {}
  constexpr Result(std::unique_ptr<Error> &&err) noexcept : e(std::move(err)) {}

  constexpr Result(Result &&) = default;
  constexpr Result &operator=(Result &&) = default;

  Result(Result const &) = delete;
  Result &operator=(Result const &) = delete;

private:
  std::unique_ptr<Error> e;
};

template <class T> using Opt = Result<void, T>;

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
  auto err() noexcept -> std::unique_ptr<Error> { return std::move(e); }

  Result() = delete;
  Result(Result const &) = delete;
  Result &operator=(Result const &) = delete;

  constexpr Result(Ok &&suc) noexcept : suc(std::move(suc.res)), e(nullptr) {}
  constexpr Result(Err &&err) noexcept
      : suc(), e(std::make_unique<Error>(err.e)) {}

  constexpr Result(Result &&) = default;
  constexpr Result &operator=(Result &&) = default;

  constexpr operator Result<void, Error>() noexcept {
    auto opt = Result<void, Error>(std::move(e));
    return opt;
  }

private:
  Success suc;
  std::unique_ptr<Error> e;
};

#endif // !__LUAMAKE_ERROR_HPP
