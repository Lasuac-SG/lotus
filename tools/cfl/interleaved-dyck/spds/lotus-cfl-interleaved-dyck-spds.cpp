#include "CFL/InterleavedDyck/SPDS/Solver.h"
#include <charconv>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace dyck = lotus::cfl::interleaved_dyck;
namespace spds = dyck::spds;
namespace {
template <class T> T number(std::string_view text) {
  T result{};
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
  if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
    throw std::invalid_argument("invalid integer: " + std::string(text));
  return result;
}
void usage() {
  std::cout << "Usage: lotus-cfl-interleaved-dyck-spds [options] graph.dot\n"
      "  --query SOURCE TARGET   one balanced reachability query\n"
      "  --source V              candidate successors of V (post*)\n"
      "  --target V              candidate predecessors of V (pre*)\n"
      "  --backward              use pre* for --query\n"
      "  --call-prefix           allow pending calls at a forward endpoint\n"
      "  --field-prefix          allow pending stores at a forward endpoint\n"
      "  --vertex V              add a vertex (also supports isolated vertices)\n"
      "  --pairs                 print sorted candidate pairs\n"
      "  --max-states N          state limit per projection (0 = unlimited)\n"
      "  --max-transitions N     transition limit per projection\n"
      "  --max-updates N         weight-update limit per projection\n"
      "  --help                  show this help\n"
      "Default: all-pairs, both stacks empty. Results are a sound upper bound.\n"
      "Exit codes: 0 = complete; 2 = input error; 3 = resource limit (no result).\n";
}
void printStats(const spds::Statistics &s) {
  std::cout << "states: " << s.states << "\ntransitions: " << s.transitions
            << "\nweight-updates: " << s.updates << '\n';
}
void printPairs(const dyck::PairSet &pairs) {
  std::vector<dyck::Pair> ordered(pairs.begin(), pairs.end());
  std::sort(ordered.begin(), ordered.end(), [](const auto &a, const auto &b) {
    return std::tie(a.source, a.target) < std::tie(b.source, b.target);
  });
  for (const auto &p : ordered) std::cout << "pair: " << p.source << ' ' << p.target << '\n';
}
} // namespace
int main(int argc, char **argv) {
  try {
    spds::Options options;
    std::optional<dyck::Vertex> source, target;
    std::vector<dyck::Vertex> vertices;
    bool single_query = false, backward = false, pairs = false, positional = false;
    std::string path;
    auto argument = [&](int &i) -> std::string_view {
      if (++i >= argc) throw std::invalid_argument("missing option argument");
      return argv[i];
    };
    for (int i = 1; i < argc; ++i) {
      const std::string_view arg = argv[i];
      if (!positional && arg == "--") { positional = true; continue; }
      if (!positional && arg == "--help") { usage(); return 0; }
      if (!positional && arg == "--query") {
        if (source || target) throw std::invalid_argument("multiple query modes");
        source = number<dyck::Vertex>(argument(i));
        target = number<dyck::Vertex>(argument(i));
        single_query = true;
      } else if (!positional && (arg == "--source" || arg == "--target")) {
        if (source || target) throw std::invalid_argument("multiple query modes");
        auto value = number<dyck::Vertex>(argument(i));
        if (arg == "--source") source = value; else target = value;
      } else if (!positional && arg == "--backward") backward = true;
      else if (!positional && arg == "--call-prefix") options.parentheses = spds::StackAcceptance::Any;
      else if (!positional && arg == "--field-prefix") options.brackets = spds::StackAcceptance::Any;
      else if (!positional && arg == "--pairs") pairs = true;
      else if (!positional && arg == "--vertex") vertices.push_back(number<dyck::Vertex>(argument(i)));
      else if (!positional && arg == "--max-states") options.limits.max_states = number<std::size_t>(argument(i));
      else if (!positional && arg == "--max-transitions") options.limits.max_transitions = number<std::size_t>(argument(i));
      else if (!positional && arg == "--max-updates") options.limits.max_updates = number<std::size_t>(argument(i));
      else if (!positional && !arg.empty() && arg.front() == '-')
        throw std::invalid_argument("unknown option: " + std::string(arg));
      else {
        if (!path.empty()) throw std::invalid_argument("multiple graph files");
        path = arg;
      }
    }
    if (path.empty()) throw std::invalid_argument("missing graph.dot; use --help");
    if (backward && !single_query) throw std::invalid_argument("--backward requires --query");
    const bool reverse = backward || (target && !source);
    if (reverse && (options.parentheses != spds::StackAcceptance::Empty ||
                    options.brackets != spds::StackAcceptance::Empty))
      throw std::invalid_argument("prefix flags require a forward query");
    dyck::Graph graph = dyck::Graph::parseDotFile(path);
    for (auto v : vertices) graph.addVertex(v);
    if ((source && !graph.containsVertex(*source)) || (target && !graph.containsVertex(*target)))
      throw std::invalid_argument("query vertex is not in graph; use --vertex for isolated vertices");
    spds::Solver solver(options);
    // Do not print result headers until saturation has completed successfully.
    const auto header = [&] {
      std::cout << "engine: spds\nsemantics: sound upper bound\n"
                << "direction: " << (reverse ? "pre*" : "post*") << '\n'
                << "call-stack: " << (options.parentheses == spds::StackAcceptance::Empty ? "empty" : "any") << '\n'
                << "field-stack: " << (options.brackets == spds::StackAcceptance::Empty ? "empty" : "any") << '\n';
    };
    if (source || target) {
      auto result = reverse ? solver.analyzeTo(graph, *target) : solver.analyzeFrom(graph, *source);
      header();
      if (single_query) {
        auto v = reverse ? *source : *target;
        std::cout << "query: " << (result.mayReach(v) ? "may-reach" : "unreachable") << '\n'
                  << "call-projection: " << (result.parenthesisReachable(v) ? "accept" : "reject") << '\n'
                  << "field-projection: " << (result.bracketReachable(v) ? "accept" : "reject") << '\n';
        if (pairs && result.mayReach(v)) std::cout << "pair: " << *source << ' ' << *target << '\n';
      } else {
        dyck::PairSet candidates;
        for (auto v : graph.vertices()) if (result.mayReach(v))
          candidates.insert(reverse ? dyck::Pair{v, *target} : dyck::Pair{*source, v});
        std::cout << "candidate-pairs: " << candidates.size() << '\n';
        if (pairs) printPairs(candidates);
      }
      std::cout << "call-automaton\n"; printStats(result.callAutomaton().statistics());
      std::cout << "field-automaton\n"; printStats(result.fieldAutomaton().statistics());
    } else {
      const auto result = solver.analyze(graph);
      header();
      std::cout << "candidate-pairs: " << result.upper_bound.size() << '\n';
      printStats(result.statistics);
      if (pairs) printPairs(result.upper_bound);
    }
    return 0;
  } catch (const spds::ResourceLimit &error) {
    std::cerr << "resource-limit: " << error.what() << "; no result returned\n";
    return 3;
  } catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    return 2;
  }
}
