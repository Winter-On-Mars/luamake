#ifndef __DEP_GRAPH_HPP
#define __DEP_GRAPH_HPP

#include <filesystem>
#include <optional>
#include <unordered_map>
#include <vector>

namespace dependency_graph {

struct src_file final {
  // std::size_t hash;
  std::filesystem::path name;

  auto is_cpp_file() const noexcept -> bool {
    return name.extension() == ".cpp";
  }
};

class graph final {
  using graph_t =
      std::unordered_map<std::filesystem::path, std::vector<src_file>>;
  graph_t dep_graph;

public:
  graph() noexcept = default;
  ~graph() noexcept = default;

  graph(graph &&) noexcept = default;
  auto operator=(graph &&) noexcept -> graph & = default;

  graph(graph const &) = delete;
  auto operator=(graph const &) = delete;

  auto insert(std::filesystem::path const &, src_file &&) noexcept -> void;

  auto seen(std::filesystem::path const &fpath) const noexcept -> bool;

  auto constexpr size() const noexcept -> size_t { return dep_graph.size(); }
  auto begin() const noexcept -> graph_t::const_iterator {
    return dep_graph.begin();
  }
  auto end() const noexcept -> graph_t::const_iterator {
    return dep_graph.end();
  }
};

auto generate(FILE *, std::filesystem::path const &) noexcept
    -> std::optional<graph>;

} // namespace dependency_graph
#endif
