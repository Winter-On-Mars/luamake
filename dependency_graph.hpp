#ifndef __DEP_GRAPH_HPP
#define __DEP_GRAPH_HPP

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace dependency_graph {

struct src_file final {
  // std::size_t hash;
  std::string name;

  auto constexpr is_cpp_file() const noexcept -> bool {
    return name.rfind(".cpp") != std::string::npos;
  }
};

class graph final {
  using graph_t = std::unordered_map<std::string, std::vector<src_file>>;
  graph_t dep_graph;

public:
  graph() noexcept = default;
  ~graph() noexcept = default;

  graph(graph &&) noexcept = default;
  auto operator=(graph &&) noexcept -> graph & = default;

  graph(graph const &) = delete;
  auto operator=(graph const &) = delete;

  auto insert(std::string &&, src_file &&) noexcept -> void;

  auto constexpr size() const noexcept -> size_t { return dep_graph.size(); }
  auto begin() const noexcept -> graph_t::const_iterator {
    return dep_graph.begin();
  }
  auto end() const noexcept -> graph_t::const_iterator {
    return dep_graph.end();
  }
};

auto generate(FILE *, std::string_view const,
              std::filesystem::path const &) noexcept -> std::optional<graph>;

} // namespace dependency_graph
#endif
