#ifndef __LUAMAKE_FILE_HPP
#define __LUAMAKE_FILE_HPP

#include "common.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <unistd.h>
#include <utility>

#ifdef __unix__
#include <fcntl.h>
#include <sys/stat.h>
#endif

namespace luamake {
// TODO: add a macro to test if on unix system, and use unix os functions like
// open, read, write, etc..., also update this to not be as bad :)
struct File final {
  enum permissions : unsigned char {
    READ = 1 << 0,
    WRITE = 1 << 1,
    BINARY = 1 << 2,
    CREATE = 1 << 3,
  };

  constexpr File(std::filesystem::path const &path,
                 permissions &&perms) noexcept
      :
#ifdef __unix__
        fd(-1)
#else
        file(nullptr)
#endif
  {
#ifdef __unix__
#define bitset(bit) (perms & bit) == bit
    auto unix_perms = int{};
    // there's probably a better way of writing this, but i can't think of it rn
    if (bitset(READ) && bitset(WRITE)) {
      unix_perms |= O_RDWR;
    } else {
      if (bitset(READ))
        unix_perms |= O_RDONLY;
      if (bitset(WRITE)) {
        unix_perms |= O_WRONLY;
      }
      if (bitset(CREATE)) {
        unix_perms |= O_CREAT;
      }
    }
#else
    char max_length_perms[] = {0, 0, 0,
                               0}; // this should be 5 or 7 from man fread
    if ((perms & READ) == READ)
      max_length_perms[0] = 'r';
    if ((perms & WRITE) == WRITE)
      max_length_perms[max_length_perms[0] != 0 ? 1 : 0] = 'w';
    if ((perms & BINARY) == BINARY)
      max_length_perms[max_length_perms[0] != 0
                           ? max_length_perms[1] != 0 ? 2 : 1
                           : 0] = 'b';
#endif
#ifdef DEBUG
#ifdef __unix__
    char max_length_perms[] = {0, 0, 0, 0};
    if (bitset(READ)) {
      max_length_perms[0] = 'r';
    }
    if (bitset(WRITE)) {
      max_length_perms[max_length_perms[0] != 0 ? 1 : 0] = 'w';
    }
    if (bitset(CREATE)) {
      max_length_perms[max_length_perms[0] != 0
                           ? max_length_perms[1] != 0 ? 2 : 1
                           : 0] = 'c';
    }
    std::cerr << "Opening [" << path << "] with options [" << max_length_perms
              << "]" NL;
#else
    std::cerr << "Opening [" << path << "] with options [" << max_length_perms
              << "]" NL;
#endif // __unix__
#endif // DEBUG

#ifdef __unix__
    if (bitset(CREATE)) {
      // read write user, read for everyone else, this seems to be the standard
      // that most file systems use when you create a file
      fd =
          open(path.c_str(), unix_perms, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    } else {
      fd = open(path.c_str(), unix_perms);
    }
#undef bitset
#else
    file = fopen(path.c_str(), max_length_perms);
#endif
  }
  constexpr ~File() noexcept {
#ifdef __unix__
    if (fd != -1)
      close(fd);
#else
    if (file != nullptr)
      fclose(file);
#endif
  }

#ifndef __unix__
  // implicit conversion operator to FILE*
  operator FILE *() const noexcept { return file; }
#endif

  auto write(void const *__restrict ptr, size_t size, size_t amount) noexcept
      -> size_t {
#ifdef __unix__
    return static_cast<size_t>(::write(fd, ptr, size * amount));
#else
    return fwrite(ptr, size, amount, file);
#endif // __unix__
  }

  /// @return returns the amount read from the file stream
  auto read(void *__restrict dest, size_t size, size_t n) noexcept -> size_t {
#ifdef __unix__
    return static_cast<size_t>(::read(fd, dest, size * n));
#else
    return fread(dest, size, n, file);
#endif // __unix__
  }

  auto flush() noexcept -> void {
#ifdef __unix__
    fdatasync(fd);
#else
    fflush(file);
#endif // __unix__
  }

  // TODO: add better error handling
  // doesn't reset the file, consumes the entire content
  auto dump_content() noexcept -> std::pair<size_t, std::unique_ptr<u8[]>> {
#ifdef __unix__
    struct ::stat file_stats = {};
    ::memset(&file_stats, 0, sizeof(file_stats));

    auto const res = ::fstat(fd, &file_stats);
    if (res == -1) {
      std::cerr << "Error reading stats of file" NL;
      std::terminate();
    }
    auto const fsize = static_cast<size_t>(file_stats.st_size);
    auto fcontent = std::make_unique<u8[]>(fsize + 1);
    if (::read(fd, fcontent.get(), fsize) == -1) {
      return std::make_pair(0, nullptr);
    }
    fcontent[fsize] = 0;
    return std::make_pair(fsize, std::move(fcontent));
#else
    auto fsize = size_t{};
    if (fseek(file, 0, SEEK_END) == -1) {
#ifdef DEBUG
      fprintf(stderr, "[%s]" NL, strerror(errno));
#endif // DEBUG
      return {0, nullptr};
    }
    if (auto const size = ftell(file); size >= 0) {
      fsize = static_cast<size_t>(size);
    } else {
#ifdef DEBUG
      fprintf(stderr, "[%s]" NL, strerror(errno));
#endif // DEBUG
      return {0, nullptr};
    }
    rewind(file);

    auto fcontent = std::make_unique<u8[]>(fsize + 1);
    if (auto const amount_read =
            fread(fcontent.get(), sizeof(unsigned char), fsize, file);
        amount_read != fsize) {
#ifdef DEBUG
      fprintf(stderr, "[%s]" NL, strerror(errno));
#endif // DEBUG
      return {0, nullptr};
    }

    fcontent[fsize] = 0;
    return {fsize, std::move(fcontent)};
#endif
  }

  constexpr operator bool() const {
#ifdef __unix__
    return fd != -1;
#else
    return file != nullptr;
#endif
  }

private:
#ifdef __unix__
  int fd;
#else
  FILE *file;
#endif
};

auto constexpr operator|(File::permissions lhs, File::permissions rhs) noexcept
    -> File::permissions {
  return static_cast<File::permissions>(
      static_cast<std::underlying_type_t<File::permissions>>(lhs) |
      static_cast<std::underlying_type_t<File::permissions>>(rhs));
}

} // namespace luamake

#endif // !__LUAMAKE_FILE_HPP
