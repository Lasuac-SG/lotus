#include "CFL/InterleavedDyck/AffineSPDS/Solver.h"
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string_view>

namespace dyck = lotus::cfl::interleaved_dyck;
namespace affine = dyck::affine;
namespace spds = dyck::spds;
namespace {
template <class T> T number(std::string_view text) {
  T value{};
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
    throw std::invalid_argument("invalid integer: " + std::string(text));
  return value;
}
std::vector<unsigned> stack(std::string_view text) {
  std::vector<unsigned> result;
  if (text.empty()) return result;
  while (true) {
    auto comma = text.find(','); result.push_back(number<unsigned>(text.substr(0, comma)));
    if (comma == std::string_view::npos) break;
    text.remove_prefix(comma + 1);
  }
  return result;
}
void usage() {
  std::cout <<
      "Usage: lotus-cfl-interleaved-dyck-affine-spds [options] graph.dot\n"
      "  --query SOURCE TARGET   one query (default post*)\n"
      "  --source V              query all successors using post*\n"
      "  --target V              query all predecessors using pre*\n"
      "  --backward              use pre* for --query\n"
      "  --call-prefix           any call stack at queried vertex\n"
      "  --field-prefix          any field stack at queried vertex\n"
      "  --call-stack IDS        exact comma-separated stack, top first (--query)\n"
      "  --field-stack IDS       exact stack; empty string means empty (--query)\n"
      "  --events N              automatic event budget (default 4)\n"
      "  --order-pairs N         automatic ordered-pair budget (default 2)\n"
      "  --identity              identity observer, equivalent to Boolean SPDS\n"
      "  --observer FILE         read a shared original-edge matrix map\n"
      "  --dump-observer FILE    save the map, including independent-block metadata\n"
      "  --mode MODE             joint (default), independent, or spds\n"
      "  --certificate           include a separating affine equation when available\n"
      "  --json                  emit machine-readable results and statistics\n"
      "  --pairs                 include sorted candidate pairs\n"
      "  --vertex V              add a vertex (including isolated vertices)\n"
      "  --max-dimension N       matrix-dimension cap; 0 = unlimited\n"
      "  --max-states N          SPDS state limit per projection; 0 = unlimited\n"
      "  --max-transitions N     SPDS transition limit per projection\n"
      "  --max-updates N         SPDS weight-promotion limit per projection\n"
      "  --help                  display this help\n"
      "Default: all-pairs, empty endpoint stacks. Every mode is an upper bound.\n"
      "Pre* queries refer to predecessor stacks; the target stacks are empty.\n"
      "Exit codes: 0 complete; 2 invalid input/I/O; 3 resource failure (no result).\n";
}
const char *reason(affine::Verdict verdict) {
  switch (verdict) {
  case affine::Verdict::MayReach: return "overlapping-histories";
  case affine::Verdict::ProjectionRejected: return "projection-rejected";
  case affine::Verdict::AffineSeparated: return "affine-separated";
  }
  throw std::logic_error("invalid verdict");
}
std::vector<dyck::Pair> ordered(const dyck::PairSet &set) {
  std::vector<dyck::Pair> result(set.begin(), set.end());
  std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) {
    return std::tie(a.source,a.target) < std::tie(b.source,b.target);
  });
  return result;
}
const char *boolean(bool b) { return b ? "true" : "false"; }
const char *answer(bool b) { return b ? "may-reach" : "unreachable"; }
void statistics(std::ostream &out, const affine::Statistics &stats, bool json) {
  const auto &s = stats.saturation;
  if (json) {
    out << "\"statistics\":{\"matrix_dimension\":" << stats.matrix_dimension
        << ",\"max_transition_rank\":" << stats.maximum_affine_rank
        << ",\"states\":" << s.states << ",\"transitions\":" << s.transitions
        << ",\"updates\":" << s.updates << ",\"processed\":" << s.processed
        << ",\"rules\":" << s.rules << '}';
  } else {
    out << "matrix-dimension: " << stats.matrix_dimension
        << "\nmax-transition-rank: " << stats.maximum_affine_rank
        << "\nstates: " << s.states << "\ntransitions: " << s.transitions
        << "\nweight-updates: " << s.updates << "\nprocessed: " << s.processed
        << "\nrules: " << s.rules << '\n';
  }
}
void certificate(std::ostream &out, const affine::HistoryComparison &comparison, bool json) {
  const auto &c = comparison.certificate();
  if (json) out << ",\"certificate\":";
  if (!c) { if (json) out << "null"; else out << "certificate: none\n"; return; }
  if (!c->verify(comparison.callHistory(), comparison.fieldHistory()))
    throw std::logic_error("invalid internal separation certificate");
  if (json) {
    out << "{\"functional\":\"" << c->functional.str() << "\",\"call_value\":"
        << unsigned(c->left_value) << ",\"field_value\":" << unsigned(c->right_value)
        << ",\"verified_against_hulls\":true}";
  } else {
    out << "certificate-functional: " << c->functional.str()
        << "\ncertificate-call-value: " << unsigned(c->left_value)
        << "\ncertificate-field-value: " << unsigned(c->right_value)
        << "\ncertificate-verified-against-hulls: true\n";
  }
}
}
int main(int argc, char **argv) {
  try {
    affine::Options options;
    std::optional<dyck::Vertex> source, target;
    std::optional<std::vector<unsigned>> call_stack, field_stack;
    bool single = false, backward = false, identity = false, pairs = false;
    bool json = false, show_certificate = false, positional = false, feature_options = false;
    std::string path, observer_path, dump_path, mode = "joint";
    std::vector<dyck::Vertex> vertices;
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
        source = number<dyck::Vertex>(argument(i)); target = number<dyck::Vertex>(argument(i)); single = true;
      } else if (!positional && (arg == "--source" || arg == "--target")) {
        if (source || target) throw std::invalid_argument("multiple query modes");
        const auto v = number<dyck::Vertex>(argument(i));
        if (arg == "--source") source = v; else target = v;
      } else if (!positional && arg == "--backward") backward = true;
      else if (!positional && arg == "--call-prefix") options.parentheses = spds::StackAcceptance::Any;
      else if (!positional && arg == "--field-prefix") options.brackets = spds::StackAcceptance::Any;
      else if (!positional && arg == "--call-stack") call_stack = stack(argument(i));
      else if (!positional && arg == "--field-stack") field_stack = stack(argument(i));
      else if (!positional && arg == "--events") { options.observer.max_events = number<std::size_t>(argument(i)); feature_options = true; }
      else if (!positional && arg == "--order-pairs") { options.observer.max_order_pairs = number<std::size_t>(argument(i)); feature_options = true; }
      else if (!positional && arg == "--observer") observer_path = argument(i);
      else if (!positional && arg == "--dump-observer") dump_path = argument(i);
      else if (!positional && arg == "--mode") mode = argument(i);
      else if (!positional && arg == "--identity") identity = true;
      else if (!positional && arg == "--certificate") show_certificate = true;
      else if (!positional && arg == "--json") json = true;
      else if (!positional && arg == "--pairs") pairs = true;
      else if (!positional && arg == "--vertex") vertices.push_back(number<dyck::Vertex>(argument(i)));
      else if (!positional && arg == "--max-dimension") options.max_matrix_dimension = number<std::size_t>(argument(i));
      else if (!positional && arg == "--max-states") options.limits.max_states = number<std::size_t>(argument(i));
      else if (!positional && arg == "--max-transitions") options.limits.max_transitions = number<std::size_t>(argument(i));
      else if (!positional && arg == "--max-updates") options.limits.max_updates = number<std::size_t>(argument(i));
      else if (!positional && !arg.empty() && arg.front() == '-') throw std::invalid_argument("unknown option: " + std::string(arg));
      else { if (!path.empty()) throw std::invalid_argument("multiple graph files"); path = arg; }
    }
    if (path.empty()) throw std::invalid_argument("missing graph.dot; use --help");
    if (mode != "joint" && mode != "independent" && mode != "spds") throw std::invalid_argument("invalid --mode");
    if (backward && !single) throw std::invalid_argument("--backward requires --query");
    if ((call_stack || field_stack || show_certificate) && !single)
      throw std::invalid_argument("explicit stacks and --certificate require --query");
    if ((call_stack || field_stack) && (options.parentheses == spds::StackAcceptance::Any || options.brackets == spds::StackAcceptance::Any))
      throw std::invalid_argument("do not combine explicit stacks with prefix flags");
    if ((!observer_path.empty() && (identity || feature_options)) || (identity && feature_options))
      throw std::invalid_argument("choose automatic feature budgets, --identity, or --observer, not a mixture");
    auto graph = dyck::Graph::parseDotFile(path);
    for (auto v : vertices) graph.addVertex(v);
    if ((source && !graph.containsVertex(*source)) || (target && !graph.containsVertex(*target)))
      throw std::invalid_argument("query vertex is not in graph; use --vertex for isolated vertices");
    affine::HistoryObserver observer;
    if (!observer_path.empty()) {
      std::ifstream input(observer_path); if (!input) throw std::invalid_argument("cannot open observer file");
      observer = affine::HistoryObserver::read(input);
    } else if (!identity) observer = affine::HistoryObserver::automatic(graph, options.observer);
    const bool reverse = backward || (target && !source);
    affine::Solver solver(options); std::ostringstream out;
    if (json) out << "{\"engine\":\"affine-spds\",\"semantics\":\"sound-upper-bound\",\"mode\":\"" << mode
                  << "\",\"direction\":\"" << (reverse ? "pre*" : "post*") << "\",";
    else out << "engine: affine-spds\nsemantics: sound upper bound\nmode: " << mode
             << "\ndirection: " << (reverse ? "pre*" : "post*") << '\n';
    dyck::PairSet candidates;
    if (source || target) {
      auto result = reverse ? solver.analyzeTo(graph,*target,observer) : solver.analyzeFrom(graph,*source,observer);
      if (single) {
        const auto vertex = reverse ? *source : *target;
        auto comparison = call_stack || field_stack
            ? result.compareStacks(vertex,call_stack.value_or(std::vector<unsigned>{}),field_stack.value_or(std::vector<unsigned>{}))
            : result.compare(vertex);
        const bool joint = comparison.mayReach(), baseline = comparison.spdsMayReach();
        const bool independent = comparison.independentMayReach(observer);
        const bool selected = mode == "joint" ? joint : mode == "independent" ? independent : baseline;
        if (selected) candidates.insert({*source,*target});
        if (json) {
          out << "\"source\":" << *source << ",\"target\":" << *target
              << ",\"may_reach\":" << boolean(selected) << ",\"joint_may_reach\":" << boolean(joint)
              << ",\"spds_may_reach\":" << boolean(baseline) << ",\"independent_may_reach\":" << boolean(independent)
              << ",\"joint_reason\":\"" << reason(comparison.verdict()) << "\",\"call_rank\":" << comparison.callHistory().rank()
              << ",\"field_rank\":" << comparison.fieldHistory().rank();
          if (show_certificate) certificate(out,comparison,true);
          out << ',';
        } else {
          out << "query: " << answer(selected) << "\njoint: " << answer(joint)
              << "\nspds: " << answer(baseline) << "\nindependent: " << answer(independent)
              << "\njoint-reason: " << reason(comparison.verdict()) << "\ncall-rank: " << comparison.callHistory().rank()
              << "\nfield-rank: " << comparison.fieldHistory().rank() << '\n';
          if (show_certificate) certificate(out,comparison,false);
        }
      } else {
        for (auto v : graph.vertices()) {
          const bool selected = mode == "joint" ? result.mayReach(v) : mode == "independent" ? result.independentMayReach(v) : result.spdsMayReach(v);
          if (selected) candidates.insert(reverse ? dyck::Pair{v,*target} : dyck::Pair{*source,v});
        }
      }
      statistics(out,result.statistics(),json);
    } else {
      auto result = solver.analyze(graph,observer);
      candidates = mode == "joint" ? result.upper_bound : mode == "independent" ? result.independent_upper_bound : result.spds_upper_bound;
      if (json) out << "\"spds_pairs\":" << result.spds_upper_bound.size() << ",\"independent_pairs\":" << result.independent_upper_bound.size()
                    << ",\"joint_pairs\":" << result.upper_bound.size() << ',';
      else out << "spds-pairs: " << result.spds_upper_bound.size() << "\nindependent-pairs: " << result.independent_upper_bound.size()
               << "\njoint-pairs: " << result.upper_bound.size() << '\n';
      statistics(out,result.statistics,json);
    }
    if (json) {
      out << ",\"candidate_pairs\":" << candidates.size();
      if (pairs) {
        out << ",\"pairs\":["; bool first = true;
        for (const auto &p : ordered(candidates)) { if (!first) out << ','; first = false; out << '[' << p.source << ',' << p.target << ']'; }
        out << ']';
      }
      out << "}\n";
    } else {
      out << "candidate-pairs: " << candidates.size() << '\n';
      if (pairs) for (const auto &p : ordered(candidates)) out << "pair: " << p.source << ' ' << p.target << '\n';
    }
    if (!dump_path.empty()) {
      for (const auto &input : {path,observer_path})
        if (!input.empty() && std::filesystem::exists(dump_path) && std::filesystem::equivalent(input,dump_path))
          throw std::invalid_argument("observer output must not overwrite an input file");
      std::ofstream file(dump_path); if (!file) throw std::runtime_error("cannot create observer output");
      observer.write(file); file.close(); if (!file) throw std::runtime_error("failed to close observer output");
    }
    // Commit output only after ALL requested saturation/readout/I/O succeeds.
    std::cout << out.str(); return 0;
  } catch (const spds::ResourceLimit &e) {
    std::cerr << "resource limit: " << e.what() << '\n'; return 3;
  } catch (const std::bad_alloc &) {
    std::cerr << "resource limit: allocation failed\n"; return 3;
  } catch (const std::exception &e) {
    std::cerr << "error: " << e.what() << '\n'; return 2;
  }
}
