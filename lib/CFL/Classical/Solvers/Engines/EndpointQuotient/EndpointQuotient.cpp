#include "CFL/Classical/Solvers/Engines/EndpointQuotient/EndpointQuotient.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <deque>
#include <limits>
#include <map>
#include <numeric>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace lotus {
namespace cfl {
namespace endpoint {
namespace {

using Clock = std::chrono::steady_clock;
using Lists = std::vector<std::vector<Id>>;

static double milliseconds(Clock::time_point a, Clock::time_point b) {
  return std::chrono::duration<double, std::milli>(b - a).count();
}

static Count product(Id a, Id b) {
  static_assert(sizeof(Id) <= sizeof(Count), "IDs must fit in counters");
  if (b && a > std::numeric_limits<Count>::max() / b)
    throw std::overflow_error("endpoint quotient fact count overflow");
  return static_cast<Count>(a) * static_cast<Count>(b);
}

static void addCount(Count &a, Count b) {
  if (a > std::numeric_limits<Count>::max() - b)
    throw std::overflow_error("endpoint quotient fact count overflow");
  a += b;
}

template <class T> static void sortUnique(std::vector<T> &values) {
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
}

struct VectorHash {
  std::size_t operator()(const std::vector<Id> &v) const noexcept {
    std::size_t h = v.size();
    for (Id x : v)
      h ^= std::hash<Id>{}(x) + std::size_t(0x9e3779b9U) + (h << 6) + (h >> 2);
    return h;
  }
};

static std::vector<Id> intern(const Lists &signatures) {
  std::unordered_map<std::vector<Id>, Id, VectorHash> table;
  table.reserve(signatures.size());
  std::vector<Id> result;
  result.reserve(signatures.size());
  for (const auto &signature : signatures) {
    const auto found = table.find(signature);
    if (found != table.end()) {
      result.push_back(found->second);
    } else {
      const Id id = table.size();
      table.emplace(signature, id);
      result.push_back(id);
    }
  }
  return result;
}

struct Partition {
  Id id = 0;
  std::vector<Id> class_of;
  Lists members;

  static Partition fromSignatures(const Lists &signatures) {
    Partition p;
    p.class_of = intern(signatures);
    for (Id v = 0; v < p.class_of.size(); ++v) {
      Id c = p.class_of[v];
      if (c == p.members.size())
        p.members.emplace_back();
      p.members[c].push_back(v);
    }
    return p;
  }

  static Partition singleton(Id n) {
    Partition p;
    p.class_of.resize(n);
    std::iota(p.class_of.begin(), p.class_of.end(), Id(0));
    p.members.resize(n);
    for (Id v = 0; v < n; ++v)
      p.members[v].push_back(v);
    return p;
  }
};

// Each fine class belongs to exactly one coarse class. The grammar dependency
// closure guarantees this refinement; checking all members in debug builds
// catches mistakes that representative-only quotient constructions can hide.
static Lists lift(const Partition &fine, const Partition &coarse) {
  Lists result(coarse.members.size());
  for (Id f = 0; f < fine.members.size(); ++f) {
    Id c = coarse.class_of[fine.members[f].front()];
#ifndef NDEBUG
    for (Id v : fine.members[f])
      assert(coarse.class_of[v] == c);
#endif
    result[c].push_back(f);
  }
  return result;
}

static Lists dependencyClosure(const Lists &dependencies,
                               const std::vector<bool> &observed) {
  Lists closure(dependencies.size());
  Lists reverse(dependencies.size());
  for (Id a = 0; a < dependencies.size(); ++a)
    for (Id b : dependencies[a])
      reverse[b].push_back(a);
  for (auto &uses : reverse)
    sortUnique(uses);
  // Only labels with input edges can distinguish endpoints. Traverse their
  // reverse dependencies, retaining an O(symbols * observed labels) closure
  // instead of all reachable grammar symbols (including constant signatures).
  for (Id label = 0; label < dependencies.size(); ++label) {
    if (!observed[label])
      continue;
    std::vector<bool> seen(dependencies.size(), false);
    std::vector<Id> stack{label};
    seen[label] = true;
    while (!stack.empty()) {
      Id b = stack.back();
      stack.pop_back();
      closure[b].push_back(label);
      for (Id c : reverse[b]) {
        if (!seen[c]) {
          seen[c] = true;
          stack.push_back(c);
        }
      }
    }
  }
  return closure;
}

// Sparse relations do not allocate a hash table per row. Denser quotients
// promote to a bounded bitmap, making duplicate publication a bit test.
class CellSet {
  struct Hash {
    std::size_t operator()(const std::pair<Id, Id> &p) const {
      return p.first ^ (p.second + std::size_t(0x9e3779b9U) + (p.first << 6) +
                        (p.first >> 2));
    }
  };
  Id columns_ = 0;
  Id area_ = 0;
  std::vector<std::uint64_t> bits_;
  std::unordered_set<std::pair<Id, Id>, Hash> sparse_;

  void promote() {
    bits_.assign((area_ + 63) / 64, 0);
    for (auto cell : sparse_) {
      const Id bit = cell.first * columns_ + cell.second;
      bits_[bit / 64] |= std::uint64_t(1) << (bit % 64);
    }
    decltype(sparse_)().swap(sparse_);
  }

public:
  void reset(Id rows, Id columns) {
    columns_ = columns;
    constexpr Id MAX_BITS = 8 * 1024 * 1024;
    if (columns && rows <= MAX_BITS / columns)
      area_ = rows * columns;
    if (area_ && area_ <= 4096)
      promote();
  }

  bool insert(Id row, Id column) {
    if (!bits_.empty()) {
      const Id bit = row * columns_ + column;
      auto &word = bits_[bit / 64];
      const auto mask = std::uint64_t(1) << (bit % 64);
      const bool added = !(word & mask);
      word |= mask;
      return added;
    }
    const bool added = sparse_.emplace(row, column).second;
    if (added && area_ && sparse_.size() >= (area_ + 127) / 128)
      promote();
    return added;
  }

  bool contains(Id row, Id column) const {
    if (bits_.empty())
      return sparse_.count({row, column}) != 0;
    const Id bit = row * columns_ + column;
    return bits_[bit / 64] & (std::uint64_t(1) << (bit % 64));
  }

  bool isDense() const { return !bits_.empty(); }

  std::uint64_t rowWord(Id row, Id word) const {
    assert(isDense());
    const Id offset = row * columns_ + word * 64;
    const Id shift = offset % 64;
    auto value = bits_[offset / 64] >> shift;
    if (shift && offset / 64 + 1 < bits_.size())
      value |= bits_[offset / 64 + 1] << (64 - shift);
    const Id remaining = columns_ - word * 64;
    if (remaining < 64)
      value &= (std::uint64_t(1) << remaining) - 1;
    return value;
  }

  template <class Visitor> void forEach(Visitor visitor) const {
    if (!isDense()) {
      for (auto cell : sparse_)
        visitor(cell.first, cell.second);
      return;
    }
    for (Id w = 0; w < bits_.size(); ++w) {
      auto bits = bits_[w];
      while (bits) {
        const Id bit = w * 64 + __builtin_ctzll(bits);
        visitor(bit / columns_, bit % columns_);
        bits &= bits - 1;
      }
    }
  }

  std::size_t payloadBytes() const {
    return bits_.capacity() * sizeof(std::uint64_t) +
           sparse_.size() * sizeof(std::pair<Id, Id>) +
           sparse_.bucket_count() * sizeof(void *);
  }
};

// Temporary, word-aligned indexes for batched joins. Unlike known cells,
// active cells contain only popped events, preserving once-per-pair joins.
struct JoinRows {
  Id out_words;
  Id in_words;
  std::vector<std::uint64_t> out;
  std::vector<std::uint64_t> in;
  std::vector<std::uint64_t> known_in;

  void know(Id row, Id column) {
    known_in[column * in_words + row / 64] |= std::uint64_t(1) << (row % 64);
  }

  void activate(Id row, Id column) {
    out[row * out_words + column / 64] |= std::uint64_t(1) << (column % 64);
    in[column * in_words + row / 64] |= std::uint64_t(1) << (row % 64);
  }
};

struct Relation {
  CellSet known;
  Lists active_out;
  Lists active_in;
  std::unique_ptr<JoinRows> join_rows;
  bool tried_join_rows = false;
};

struct UnaryPlan {
  Id lhs;
  Id child;
  std::shared_ptr<const Lists> rows;
  std::shared_ptr<const Lists> columns;
};

struct Bridge {
  Lists to_right;
  Lists to_left;
  Count pairs = 0;
};

struct BinaryPlan {
  Id lhs;
  Id left;
  Id right;
  std::shared_ptr<const Lists> rows;
  std::shared_ptr<const Lists> columns;
  // An interface edge (j,k) means Q_left[j] intersects P_right[k].
  // Numeric class ID equality has NO semantic meaning across partitions.
  std::shared_ptr<const Bridge> bridge;
  // Only nontrivial refinement products need a separate output cache.
  CellSet expanded;
  bool expanded_ready = false;
};

struct Fact {
  Id symbol;
  Id row;
  Id column;
};

} // namespace

void Problem::validate() const {
  for (const Edge &e : edges)
    if (e.source >= nodes || e.target >= nodes || e.symbol >= symbols)
      throw std::invalid_argument("endpoint quotient edge ID out of range");
  for (const Rule &r : rules) {
    if (r.lhs >= symbols)
      throw std::invalid_argument("endpoint quotient rule LHS out of range");
    switch (r.kind) {
    case Rule::Kind::Epsilon:
      break;
    case Rule::Kind::Unary:
      if (r.left >= symbols)
        throw std::invalid_argument("endpoint quotient unary RHS out of range");
      break;
    case Rule::Kind::Binary:
      if (r.left >= symbols || r.right >= symbols)
        throw std::invalid_argument(
            "endpoint quotient binary RHS out of range");
      break;
    default:
      throw std::invalid_argument("endpoint quotient invalid rule kind");
    }
  }
}

struct Solver::Impl {
  Problem problem;
  Options options;
  bool solved = false;
  bool started = false;
  Statistics stats;
  std::vector<bool> nullable;
  std::vector<std::shared_ptr<const Partition>> sources;
  std::vector<std::shared_ptr<const Partition>> targets;
  std::vector<Relation> relations;
  std::vector<UnaryPlan> units;
  std::vector<BinaryPlan> binaries;
  Lists unit_uses;
  Lists left_uses;
  Lists right_uses;
  std::deque<Fact> worklist;

  Impl(Problem p, Options o) : problem(std::move(p)), options(o) {
    problem.validate();
    switch (options.partitions) {
    case PartitionMode::Grammar:
    case PartitionMode::Global:
    case PartitionMode::Singleton:
      break;
    default:
      throw std::invalid_argument("endpoint quotient invalid partition mode");
    }
  }

  void requireSolved() const {
    if (!solved)
      throw std::logic_error(
          "endpoint quotient queried before solve completed");
  }

  void requireSymbol(Id a) const {
    if (a >= problem.symbols)
      throw std::out_of_range("endpoint quotient query symbol out of range");
  }

  void computeNullable() {
    nullable.assign(problem.symbols, false);
    Lists uses(problem.symbols);
    std::vector<Id> remaining(problem.rules.size());
    std::vector<Id> pending;
    auto mark = [&](Id symbol) {
      if (!nullable[symbol]) {
        nullable[symbol] = true;
        pending.push_back(symbol);
        ++stats.nullable_symbols;
      }
    };
    for (Id i = 0; i < problem.rules.size(); ++i) {
      const Rule &r = problem.rules[i];
      if (r.kind == Rule::Kind::Epsilon) {
        mark(r.lhs);
      } else {
        remaining[i] = r.kind == Rule::Kind::Unary ? 1 : 2;
        uses[r.left].push_back(i);
        if (r.kind == Rule::Kind::Binary)
          uses[r.right].push_back(i);
      }
    }
    for (Id i = 0; i < pending.size(); ++i)
      for (Id rule : uses[pending[i]])
        if (--remaining[rule] == 0)
          mark(problem.rules[rule].lhs);
  }

  void buildPartitions(const Lists &first, const Lists &last) {
    const Id k = problem.symbols, n = problem.nodes;
    sources.resize(k);
    targets.resize(k);
    // Canonical class numbering lets equal partitions share both membership
    // storage and all subsequent lift/bridge plans.
    std::unordered_map<std::vector<Id>, std::shared_ptr<const Partition>,
                       VectorHash>
        partitions;
    auto retain = [&](Partition p) {
      const auto found = partitions.find(p.class_of);
      if (found != partitions.end())
        return found->second;
      p.id = stats.partitions_built++;
      auto result = std::make_shared<const Partition>(std::move(p));
      partitions.emplace(result->class_of, result);
      return result;
    };
    if (options.partitions == PartitionMode::Singleton) {
      const auto p = retain(Partition::singleton(n));
      std::fill(sources.begin(), sources.end(), p);
      std::fill(targets.begin(), targets.end(), p);
      return;
    }

    std::vector<std::vector<Edge>> seeds(k);
    for (const Edge &e : problem.edges)
      seeds[e.symbol].push_back(e);
    Lists atomic_out(k), atomic_in(k);
    std::vector<Id> observed;
    for (Id a = 0; a < k; ++a) {
      // A symbol with no axioms contributes only a constant signature. Its
      // dependencies still participate in FIRST/LAST before this projection.
      if (seeds[a].empty())
        continue;
      observed.push_back(a);
      Lists out(n), in(n);
      for (const Edge &e : seeds[a]) {
        out[e.source].push_back(e.target);
        in[e.target].push_back(e.source);
      }
      for (auto &row : out)
        sortUnique(row);
      for (auto &column : in)
        sortUnique(column);
      atomic_out[a] = intern(out);
      atomic_in[a] = intern(in);
    }

    auto build = [&](const Lists &closure, const Lists &atomic,
                     std::vector<std::shared_ptr<const Partition>> &result) {
      std::unordered_map<std::vector<Id>, std::shared_ptr<const Partition>,
                         VectorHash>
          cache;
      for (Id a = 0; a < k; ++a) {
        std::vector<Id> labels;
        if (options.partitions == PartitionMode::Global) {
          labels = observed;
        } else {
          for (Id b : closure[a])
            if (!seeds[b].empty())
              labels.push_back(b);
        }
        const auto found = cache.find(labels);
        if (found != cache.end()) {
          result[a] = found->second;
          continue;
        }
        Lists signatures(n);
        for (Id v = 0; v < n; ++v) {
          signatures[v].reserve(labels.size());
          for (Id b : labels)
            signatures[v].push_back(atomic[b][v]);
        }
        result[a] = retain(Partition::fromSignatures(signatures));
        cache.emplace(std::move(labels), result[a]);
      }
    };
    build(first, atomic_out, sources);
    build(last, atomic_in, targets);
  }

  void prepare() {
    computeNullable();
    std::vector<std::pair<Id, Id>> unary_rules;
    std::vector<std::tuple<Id, Id, Id>> binary_rules;
    for (const Rule &r : problem.rules) {
      if (r.kind == Rule::Kind::Unary)
        unary_rules.emplace_back(r.lhs, r.left);
      if (r.kind == Rule::Kind::Binary) {
        binary_rules.emplace_back(r.lhs, r.left, r.right);
        // R_A = nullable(A)*I union R_A^+. Identity must never be represented
        // as a complete block: a multi-vertex diagonal is not a rectangle.
        if (nullable[r.right])
          unary_rules.emplace_back(r.lhs, r.left);
        if (nullable[r.left])
          unary_rules.emplace_back(r.lhs, r.right);
      }
    }
    sortUnique(unary_rules);
    sortUnique(binary_rules);
    Lists left_dependencies(problem.symbols),
        right_dependencies(problem.symbols);
    for (auto r : unary_rules) {
      left_dependencies[r.first].push_back(r.second);
      right_dependencies[r.first].push_back(r.second);
    }
    for (auto r : binary_rules) {
      left_dependencies[std::get<0>(r)].push_back(std::get<1>(r));
      right_dependencies[std::get<0>(r)].push_back(std::get<2>(r));
    }
    if (options.partitions == PartitionMode::Grammar) {
      std::vector<bool> observed(problem.symbols, false);
      for (const auto &edge : problem.edges)
        observed[edge.symbol] = true;
      buildPartitions(dependencyClosure(left_dependencies, observed),
                      dependencyClosure(right_dependencies, observed));
    } else {
      buildPartitions({}, {});
    }

    unit_uses.resize(problem.symbols);
    left_uses.resize(problem.symbols);
    right_uses.resize(problem.symbols);
    relations.resize(problem.symbols);
    for (Id a = 0; a < problem.symbols; ++a) {
      relations[a].known.reset(sources[a]->members.size(),
                               targets[a]->members.size());
      relations[a].active_out.resize(sources[a]->members.size());
      relations[a].active_in.resize(targets[a]->members.size());
    }
    using Key = std::pair<Id, Id>;
    std::map<Key, std::shared_ptr<const Lists>> lifts;
    auto getLift = [&](const std::shared_ptr<const Partition> &fine,
                       const std::shared_ptr<const Partition> &coarse) {
      const Key key{fine->id, coarse->id};
      auto &entry = lifts[key];
      if (!entry) {
        entry = std::make_shared<const Lists>(lift(*fine, *coarse));
        ++stats.lifts_built;
      }
      return entry;
    };
    std::map<Key, std::shared_ptr<const Bridge>> bridges;
    for (auto r : unary_rules) {
      Id a = r.first, b = r.second;
      if (a == b)
        continue;
      unit_uses[b].push_back(units.size());
      units.push_back({a, b, getLift(sources[a], sources[b]),
                       getLift(targets[a], targets[b])});
    }
    for (auto r : binary_rules) {
      Id a = std::get<0>(r), b = std::get<1>(r), c = std::get<2>(r);
      const Key key{targets[b]->id, sources[c]->id};
      auto &bridge = bridges[key];
      if (!bridge) {
        auto value = std::make_shared<Bridge>();
        value->to_right.resize(targets[b]->members.size());
        value->to_left.resize(sources[c]->members.size());
        if (targets[b] == sources[c]) {
          for (Id i = 0; i < targets[b]->members.size(); ++i) {
            value->to_right[i].push_back(i);
            value->to_left[i].push_back(i);
          }
          value->pairs = targets[b]->members.size();
        } else {
          std::vector<Key> pairs;
          pairs.reserve(problem.nodes);
          for (Id v = 0; v < problem.nodes; ++v)
            pairs.emplace_back(targets[b]->class_of[v],
                               sources[c]->class_of[v]);
          sortUnique(pairs);
          value->pairs = pairs.size();
          for (auto pair : pairs) {
            value->to_right[pair.first].push_back(pair.second);
            value->to_left[pair.second].push_back(pair.first);
          }
        }
        bridge = std::move(value);
        ++stats.bridges_built;
      }
      addCount(stats.bridge_pairs, bridge->pairs);
      left_uses[b].push_back(binaries.size());
      right_uses[c].push_back(binaries.size());
      binaries.push_back({a,
                          b,
                          c,
                          getLift(sources[a], sources[b]),
                          getLift(targets[a], targets[c]),
                          bridge,
                          {}});
    }
  }

  bool publish(Id a, Id row, Id column) {
    ++stats.insert_attempts;
    if (!relations[a].known.insert(row, column)) {
      ++stats.duplicate_inserts;
      return false;
    }
    if (relations[a].join_rows)
      relations[a].join_rows->know(row, column);
    worklist.push_back({a, row, column});
    ++stats.cells;
    ++stats.worklist_pushes;
    stats.peak_worklist =
        std::max(stats.peak_worklist, static_cast<Count>(worklist.size()));
    return true;
  }

  void binaryJoin(BinaryPlan &plan, Id row, Id column) {
    ++stats.binary_joins;
    if ((*plan.rows)[row].size() > 1 || (*plan.columns)[column].size() > 1) {
      if (!plan.expanded_ready) {
        plan.expanded.reset(plan.rows->size(), plan.columns->size());
        plan.expanded_ready = true;
      }
      if (!plan.expanded.insert(row, column)) {
        ++stats.repeated_binary_outputs;
        return;
      }
    }
    for (Id i : (*plan.rows)[row])
      for (Id j : (*plan.columns)[column]) {
        ++stats.binary_propagations;
        if (publish(plan.lhs, i, j))
          ++stats.successful_binary_propagations;
      }
  }

  void prepareJoinRows(Id a) {
    auto &r = relations[a];
    if (r.tried_join_rows || !r.known.isDense())
      return;
    r.tried_join_rows = true;
    const Id rows = r.active_out.size(), columns = r.active_in.size();
    const Id out_words = (columns + 63) / 64, in_words = (rows + 63) / 64;
    // Bound padded indexes too: a very skinny matrix must stay sparse here.
    constexpr Id MAX_WORDS = 128 * 1024;
    if (!out_words || !in_words || rows > MAX_WORDS / out_words ||
        columns > MAX_WORDS / in_words)
      return;
    r.join_rows = std::make_unique<JoinRows>(JoinRows{
        out_words, in_words, std::vector<std::uint64_t>(rows * out_words),
        std::vector<std::uint64_t>(columns * in_words),
        std::vector<std::uint64_t>(columns * in_words)});
    r.known.forEach([&](Id row, Id column) { r.join_rows->know(row, column); });
    for (Id row = 0; row < rows; ++row)
      for (Id column : r.active_out[row])
        r.join_rows->activate(row, column);
  }

  bool batchJoin(const BinaryPlan &plan, const Fact &f, bool from_left) {
    // Identical endpoint partitions make the lifts identities. Union active
    // rows (or columns) and subtract known output with word operations instead
    // of visiting every witness and attempting the same insertion repeatedly.
    if (sources[plan.lhs] != sources[plan.left] ||
        targets[plan.lhs] != targets[plan.right])
      return false;
    const Id other = from_left ? plan.right : plan.left;
    prepareJoinRows(other);
    prepareJoinRows(plan.lhs);
    const auto &input = relations[other].join_rows;
    auto &output = relations[plan.lhs];
    if (!input || !output.join_rows)
      return false;
    const auto &middles = from_left ? plan.bridge->to_right[f.column]
                                    : plan.bridge->to_left[f.row];
    const Id words = from_left ? input->out_words : input->in_words;
    for (Id middle : middles) {
      const auto &values = from_left ? input->out : input->in;
      for (Id w = 0; w < words; ++w) {
        ++stats.binary_join_words;
        const auto candidates = values[middle * words + w];
        const auto known =
            from_left ? output.known.rowWord(f.row, w)
                      : output.join_rows->known_in[f.column * words + w];
        auto delta = candidates & ~known;
        const Count joins = __builtin_popcountll(candidates);
        stats.binary_joins += joins;
        stats.repeated_binary_outputs += joins - __builtin_popcountll(delta);
        while (delta) {
          const Id endpoint = w * 64 + __builtin_ctzll(delta);
          ++stats.binary_propagations;
          if (publish(plan.lhs, from_left ? f.row : endpoint,
                      from_left ? endpoint : f.column))
            ++stats.successful_binary_propagations;
          delta &= delta - 1;
        }
      }
    }
    return true;
  }

  void saturate() {
    stats.input_edges = problem.edges.size();
    for (const Edge &e : problem.edges) {
      Id i = sources[e.symbol]->class_of[e.source];
      Id j = targets[e.symbol]->class_of[e.target];
      if (publish(e.symbol, i, j)) {
        ++stats.seed_cells;
        addCount(stats.seed_facts,
                 product(sources[e.symbol]->members[i].size(),
                         targets[e.symbol]->members[j].size()));
      }
    }
    while (!worklist.empty()) {
      // Copy before publishing: queue growth must not invalidate this event.
      Fact f = worklist.front();
      worklist.pop_front();
      ++stats.worklist_pops;
      for (Id id : unit_uses[f.symbol]) {
        const UnaryPlan &plan = units[id];
        for (Id i : (*plan.rows)[f.row])
          for (Id j : (*plan.columns)[f.column]) {
            ++stats.unary_propagations;
            if (publish(plan.lhs, i, j))
              ++stats.successful_unary_propagations;
          }
      }
      // Only popped events enter active adjacency. Thus each distinct pair
      // joins at the later activation, independent of enqueue/derivation order.
      // publish() changes known cells and the queue, never active adjacency.
      for (Id id : left_uses[f.symbol]) {
        BinaryPlan &plan = binaries[id];
        if (!batchJoin(plan, f, true))
          for (Id middle : plan.bridge->to_right[f.column])
            for (Id column : relations[plan.right].active_out[middle])
              binaryJoin(plan, f.row, column);
        // A cell used twice is absent from both active scans. Handle this
        // diagonal of the *event pair space* exactly once, not once per node.
        if (plan.right == f.symbol &&
            std::binary_search(plan.bridge->to_right[f.column].begin(),
                               plan.bridge->to_right[f.column].end(), f.row))
          binaryJoin(plan, f.row, f.column);
      }
      for (Id id : right_uses[f.symbol]) {
        BinaryPlan &plan = binaries[id];
        if (!batchJoin(plan, f, false))
          for (Id middle : plan.bridge->to_left[f.row])
            for (Id row : relations[plan.left].active_in[middle])
              binaryJoin(plan, row, f.column);
      }
      relations[f.symbol].active_out[f.row].push_back(f.column);
      relations[f.symbol].active_in[f.column].push_back(f.row);
      if (relations[f.symbol].join_rows)
        relations[f.symbol].join_rows->activate(f.row, f.column);
    }
  }

  void count() {
    stats.per_symbol.resize(problem.symbols);
    for (Id a = 0; a < problem.symbols; ++a) {
      SymbolStatistics &s = stats.per_symbol[a];
      s.source_classes = sources[a]->members.size();
      s.target_classes = targets[a]->members.size();
      for (Id i = 0; i < relations[a].active_out.size(); ++i) {
        addCount(s.positive_cells, relations[a].active_out[i].size());
        for (Id j : relations[a].active_out[i])
          addCount(s.positive_facts, product(sources[a]->members[i].size(),
                                             targets[a]->members[j].size()));
      }
      Count positive_diagonal = 0;
      if (sources[a] == targets[a]) {
        for (Id i = 0; i < sources[a]->members.size(); ++i)
          if (relations[a].known.contains(i, i))
            addCount(positive_diagonal, sources[a]->members[i].size());
      } else if (s.positive_cells) {
        for (Id v = 0; v < problem.nodes; ++v)
          positive_diagonal += positiveContains(a, v, v) ? 1 : 0;
      }
      s.logical_facts = s.positive_facts;
      s.diagonal_facts = nullable[a] ? problem.nodes : positive_diagonal;
      if (nullable[a])
        addCount(s.logical_facts, problem.nodes - positive_diagonal);
      addCount(stats.logical_facts, s.logical_facts);
    }
    stats.inferred_facts = stats.logical_facts - stats.seed_facts;
  }

  bool positiveContains(Id a, Id u, Id v) const {
    return relations[a].known.contains(sources[a]->class_of[u],
                                       targets[a]->class_of[v]);
  }

  void solve() {
    if (solved)
      return;
    if (started)
      throw std::logic_error(
          "cannot retry an interrupted endpoint quotient solve");
    started = true;
    auto begin = Clock::now();
    prepare();
    auto prepared = Clock::now();
    saturate();
    auto saturated = Clock::now();
    count();
    auto counted = Clock::now();
    stats.preprocess_ms = milliseconds(begin, prepared);
    stats.saturation_ms = milliseconds(prepared, saturated);
    stats.count_ms = milliseconds(saturated, counted);
    // Queries need partitions and final adjacency, not grammar plans, input
    // copies, worklists, or the temporary refinement-output cache.
    decltype(units)().swap(units);
    decltype(binaries)().swap(binaries);
    Lists().swap(unit_uses);
    Lists().swap(left_uses);
    Lists().swap(right_uses);
    decltype(worklist)().swap(worklist);
    decltype(problem.edges)().swap(problem.edges);
    decltype(problem.rules)().swap(problem.rules);
    for (auto &relation : relations)
      relation.join_rows.reset();
    solved = true;
  }
};

Solver::Solver(Problem problem, Options options)
    : impl_(new Impl(std::move(problem), options)) {}
Solver::~Solver() = default;
Solver::Solver(Solver &&) noexcept = default;
Solver &Solver::operator=(Solver &&) noexcept = default;

void Solver::solve() { impl_->solve(); }

bool Solver::contains(Id symbol, Id source, Id target) const {
  impl_->requireSolved();
  impl_->requireSymbol(symbol);
  if (source >= impl_->problem.nodes || target >= impl_->problem.nodes)
    throw std::out_of_range("endpoint quotient query node out of range");
  return (source == target && impl_->nullable[symbol]) ||
         impl_->positiveContains(symbol, source, target);
}

bool Solver::isNullable(Id symbol) const {
  impl_->requireSolved();
  impl_->requireSymbol(symbol);
  return impl_->nullable[symbol];
}

const Statistics &Solver::statistics() const {
  impl_->requireSolved();
  return impl_->stats;
}

Id Solver::nodeCount() const { return impl_->problem.nodes; }

bool Solver::visitSuccessors(Id a, Id source,
                             const NodeVisitor &visitor) const {
  impl_->requireSolved();
  impl_->requireSymbol(a);
  if (source >= nodeCount())
    throw std::out_of_range("endpoint quotient source out of range");
  const Id row = impl_->sources[a]->class_of[source];
  for (Id column : impl_->relations[a].active_out[row])
    for (Id target : impl_->targets[a]->members[column])
      if (!visitor(target))
        return false;
  return !impl_->nullable[a] || impl_->positiveContains(a, source, source) ||
         visitor(source);
}

bool Solver::visitPredecessors(Id a, Id target,
                               const NodeVisitor &visitor) const {
  impl_->requireSolved();
  impl_->requireSymbol(a);
  if (target >= nodeCount())
    throw std::out_of_range("endpoint quotient target out of range");
  const Id column = impl_->targets[a]->class_of[target];
  for (Id row : impl_->relations[a].active_in[column])
    for (Id source : impl_->sources[a]->members[row])
      if (!visitor(source))
        return false;
  return !impl_->nullable[a] || impl_->positiveContains(a, target, target) ||
         visitor(target);
}

bool Solver::visitFacts(Id a, const PairVisitor &visitor) const {
  impl_->requireSolved();
  impl_->requireSymbol(a);
  for (Id row = 0; row < impl_->relations[a].active_out.size(); ++row)
    for (Id column : impl_->relations[a].active_out[row])
      for (Id source : impl_->sources[a]->members[row])
        for (Id target : impl_->targets[a]->members[column])
          if (!visitor(source, target))
            return false;
  if (impl_->nullable[a])
    for (Id v = 0; v < nodeCount(); ++v)
      if (!impl_->positiveContains(a, v, v) && !visitor(v, v))
        return false;
  return true;
}

Count Solver::countOffDiagonalUnion(std::vector<Id> symbols) const {
  impl_->requireSolved();
  sortUnique(symbols);
  for (Id a : symbols)
    impl_->requireSymbol(a);
  if (symbols.empty())
    return 0;
  if (symbols.size() == 1) {
    const auto &s = impl_->stats.per_symbol[symbols.front()];
    return s.logical_facts - s.diagonal_facts;
  }

  // Sources with the same tuple of source classes have identical positive
  // successor sets across all selected symbols. Count their union once.
  Lists signatures(nodeCount());
  for (Id v = 0; v < nodeCount(); ++v) {
    signatures[v].reserve(symbols.size());
    for (Id a : symbols)
      signatures[v].push_back(impl_->sources[a]->class_of[v]);
  }
  const Partition common = Partition::fromSignatures(signatures);
  std::vector<Id> marks(nodeCount(), 0);
  Count result = 0;
  for (Id group = 0; group < common.members.size(); ++group) {
    const Id source = common.members[group].front();
    const Id stamp = group + 1;
    Id targets = 0;
    for (Id a : symbols) {
      const Id row = impl_->sources[a]->class_of[source];
      for (Id column : impl_->relations[a].active_out[row])
        for (Id target : impl_->targets[a]->members[column])
          if (marks[target] != stamp) {
            marks[target] = stamp;
            ++targets;
          }
    }
    Count pairs = product(common.members[group].size(), targets);
    for (Id v : common.members[group])
      if (marks[v] == stamp)
        --pairs;
    addCount(result, pairs);
  }
  return result;
}

std::size_t Solver::estimatedPayloadBytes() const {
  impl_->requireSolved();
  auto listsBytes = [](const Lists &lists) {
    std::size_t bytes = lists.capacity() * sizeof(std::vector<Id>);
    for (const auto &list : lists)
      bytes += list.capacity() * sizeof(Id);
    return bytes;
  };
  std::size_t bytes = sizeof(*this) + sizeof(Impl);
  bytes += (impl_->sources.capacity() + impl_->targets.capacity()) *
           sizeof(std::shared_ptr<const Partition>);
  bytes += impl_->nullable.capacity() / 8;
  bytes += impl_->stats.per_symbol.capacity() * sizeof(SymbolStatistics);
  std::unordered_set<const Partition *> seen;
  for (const auto *partitions : {&impl_->sources, &impl_->targets})
    for (const auto &p : *partitions)
      if (seen.insert(p.get()).second)
        bytes += sizeof(Partition) + p->class_of.capacity() * sizeof(Id) +
                 listsBytes(p->members);
  bytes += impl_->relations.capacity() * sizeof(Relation);
  for (const auto &r : impl_->relations)
    bytes += r.known.payloadBytes() + listsBytes(r.active_out) +
             listsBytes(r.active_in);
  return bytes;
}

void Solver::forEachPositiveRectangle(const RectangleVisitor &visitor) const {
  impl_->requireSolved();
  for (Id a = 0; a < impl_->problem.symbols; ++a)
    for (Id i = 0; i < impl_->relations[a].active_out.size(); ++i)
      for (Id j : impl_->relations[a].active_out[i])
        visitor(a, impl_->sources[a]->members[i],
                impl_->targets[a]->members[j]);
}

void Solver::forEachFact(const FactVisitor &visitor) const {
  forEachPositiveRectangle(
      [&](Id a, const std::vector<Id> &rows, const std::vector<Id> &columns) {
        for (Id u : rows)
          for (Id v : columns)
            visitor(a, u, v);
      });
  for (Id a = 0; a < impl_->problem.symbols; ++a)
    if (impl_->nullable[a])
      for (Id v = 0; v < impl_->problem.nodes; ++v)
        if (!impl_->positiveContains(a, v, v))
          visitor(a, v, v);
}

} // namespace endpoint
} // namespace cfl
} // namespace lotus
