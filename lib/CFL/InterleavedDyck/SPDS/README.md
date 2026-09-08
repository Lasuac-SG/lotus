# Synchronized pushdown systems (SPDS)

This module implements the reachability construction of:

Johannes Spath, Karim Ali, and Eric Bodden. **Context-, Flow-, and
Field-Sensitive Data-Flow Analysis using Synchronized Pushdown Systems**.
POPL 2019, PACMPL 3, Article 48. DOI: 10.1145/3290361.

Primary specification: the user-supplied `spds.pdf`, particularly Sections 2-4
and 5.1-5.3. The paper describes the call/field encodings and synchronization,
but delegates `post*` and `pre*` to established PDS algorithms. This module
supplies an independently written saturation implementation; it does not import
the authors' Java implementation or reproduce their entire experimental artifact.

Public namespace: `lotus::cfl::interleaved_dyck::spds`.
Library target: `CanaryInterleavedDyckSPDS`.
Executable: `lotus-cfl-interleaved-dyck-spds`.

## Guarantees and scope

Each individual PDS closure is exact relative to the supplied rules and seed
regular language. Synchronization is the conjunction in Definition 4, not a
same-path, two-stack reachability test. Two different graph paths may witness
the two projections (Section 4.1, Figure 7). Therefore the graph result is named
`upper_bound` and positive queries are named `mayReach` / `mayAccept`.

For the default graph query, both the initial and final stacks are empty:

```
R_call  = {(u,v) | some u->v path has a balanced parenthesis projection}
R_field = {(u,v) | some u->v path has a balanced bracket projection}
upper_bound = R_call intersection R_field
```

A true interleaved-Dyck path witnesses both projections and is never discarded.
A missing pair certifies absence of such a path. A present pair need not have a
single balanced witness. The implementation deliberately does not intersect
path witnesses, iteratively prune one PDS with the other, or substitute the
previous LCL engine. Such changes would not be Definition 4's construction.

The paper also queries nonempty field stacks (access paths) and pending calling
contexts. Its Figure 7 contains an outstanding outer call. Use a precise stack
query or existential pending-call mode for that example: the default
empty/empty graph query is intentionally a different question.

This is an engine and a front-end-neutral data-flow rule builder. It is **not**
a complete Soot/Boomerang/IDEal or LLVM pointer-analysis/typestate front end.
Clients remain responsible for extracting CFG/call edges, kills and identity
flows, allocation sites, alias-induced rules, and sound strong-update decisions.
Section 5.1's alias-injection mechanism is supported by rule insertion and
illustrated by the Figure 8 test, not by an automatic general points-to client.
Section 5.3's primitives are provided (`pre*`, forward queries); automatic
backward-demand/forward-answer scheduling remains a client responsibility.

## Components and correspondence with the paper

| Header | Implemented functionality | Paper basis |
|---|---|---|
| `Pushdown.h` | Normal/push/pop PDS rules; regular seed languages; forward `post*`; backward `pre*`; weighted saturation; incremental insertion | Definitions 1-2, Section 2; Sections 5.2-5.3 |
| `Synchronized.h` | Call-PDS over variable controls and statement stacks; field-PDS over variable-at-statement controls and field stacks; synchronized configuration queries | Definition 3, Sections 3.1-3.3, Definition 4 |
| `Semiring.h` | Boolean weights and finite binary-relation typestate weights; customizable finite-height idempotent semiring interface | Section 5.2 |
| `Solver.h` | Lotus directed/typed Graph adapter, all-pairs and per-source/per-target queries | Graph specialization of the Section 4 construction |

For `SynchronizedSystem`, the call-PDS controls are globally unique variable
IDs, its stack symbols are statement IDs, and the top symbol is the current
statement. The remaining call stack lists pending return sites. Field-PDS
controls are `(variable, statement)` pairs. Its stack is an access path.

Each builder operation contributes **one data-flow transfer**, as follows:

| Operation | Call-PDS | Field-PDS |
|---|---|---|
| `addNormal(x@s,y@t)` | replace `s` with `t`, change control `x` to `y` | preserve field top and change location |
| `addStore(x@s,y@t,f)` | same normal call-PDS rule | prepend `f` to field stack |
| `addLoad(x@s,y@t,f)` | same normal call-PDS rule | pop precisely `f` |
| `addCall(x@s,y@entry,return_site)` | replace `s` by `entry return_site` | preserve field stack |
| `addReturn(x@exit,y@caller)` | pop `exit`, map control to `y` | normal flow to `y@caller` |

As in the paper's Table 2, a call-PDS pop rule does not separately test the
specified `caller` statement: the actual next statement is revealed by the
saved call stack. The field-PDS follows the explicitly supplied possible
caller edge. Synchronization requires agreement on the queried statement.

Do not interpret `addStore` as a whole statement transfer: the identity flow
retaining the source variable, for instance, must be supplied separately if it
belongs to the client analysis. Likewise, no implicit assignment identity can
resurrect a fact that the client intended to kill.

For a raw Lotus `Graph`, nodes already represent data-flow locations and do not
carry separate variable/statement metadata. The graph adapter therefore uses
nodes as controls in each projection. Parentheses push/pop only in the call
projection, brackets only in the field projection, and other edges preserve
the respective stack. It does not invent variable or statement metadata.

## Saturation and implementation decisions

A primitive PDS rule replaces exactly one top symbol by zero, one, or two
symbols. Stacks and words are top-first. Epsilon is an automaton label, never
an allowed PDS stack symbol. Primitive PDSs support genuinely empty stacks.

Given an automaton transition `p -a-> q`, a forward rule
`<p,a> -> <p',w>` adds:

- `p' -epsilon-> q` for an empty replacement;
- `p' -b-> q` for replacement `b`;
- `p' -b-> generated(p',b) -c-> q` for replacement `b c`.

Generated states are interned by `(target control, first replacement symbol)`.
The first edge of a weighted push has weight ONE; the continuation carries
prior history extended with the push-rule weight. This placement preserves
correlation between continuation stacks and weights.

Backward saturation uses:

```
<p,a> -> <p',epsilon>                    => p -a-> p'
<p,a> -> <p',b>,    p' -b-> q            => p -a-> q
<p,a> -> <p',b c>,  p' -b-> m -c-> q    => p -a-> q
```

The backward two-symbol join is indexed from both operands. A first-operand
arrival registers a dependency on its second operand, and second-operand
arrivals revisit registered dependencies. This avoids dependence on insertion
order. Both directions also maintain epsilon closure and symbol edges composed
with epsilon prefixes/suffixes.

Rules are indexed by the control/top they inspect. All changed edge weights,
including promotions of existing transitions, are re-enqueued. Adjacency lists
are copied before a propagation step that can reallocate them; cyclic and
self-loop cases do not retain invalid iterators.

### Regular seed normalization

A client-supplied `RegularSet` can contain incoming edges to PDS controls,
accepting controls, and epsilon cycles. Before saturation, all seed states are
cloned and each original control is connected to its clone by epsilon. This
ensures no edge enters an original PDS control in the initial automaton. Without
this standard separation, post* rules can incorrectly rewrite a seed stack's
continuation. A regression explicitly exercises that failure mode.

The primitive regular-set API permits unioned seeds within a single PDS.
**Do not independently union different allocation-site seeds in the two PDSs
and then synchronize:** that would mix objects. `SynchronizedSystem::postStar`
and `preStar` deliberately accept one synchronized configuration at a time.
Use one result/session per allocation site, as Section 5.1 describes.

### Weighted execution order

The domain contract is:

```
using Weight = ...;
Weight zero() const;
Weight one() const;
Weight combine(Weight, Weight) const;
Weight extend(Weight, Weight) const;
// Weight also supports equality/inequality.
```

`extend(a,b)` always means executing `a` and then `b`. Combine must be
associative, commutative and idempotent; extend associative and distributive;
zero must annihilate extend; the ascending order induced by combine must have
finite height. Domain objects can hold state (e.g. the number of typestates).
Algebraic laws are a client contract, not automatically checked properties.

Weighted post* automaton paths accumulate weights **right to left**, whereas
pre* paths accumulate them **left to right**. Forward rules append their rule
weight to the consumed edge weight; backward rules prepend it to the RHS path
weight. This is essential for noncommutative typestate relations. A pre* query
still returns weights in the original program's forward execution order, not
an inverse typestate transition.

`RelationSemiring(n)` represents arbitrary finite binary relations on `n`
typestates. Combine is union and extend is relational composition. Bit rows
span as many machine words as needed; there is no 64-state limit. For this
domain every edge has at most `n*n` strict weight promotions. This does not
implement IDEal's independent alias-based strong-update decisions.

### Protected bottoms

The graph adapter and field-PDS builder encode an empty stack by a protected
bottom symbol `0`. It is distinct from automaton epsilon and is never popped.
External label/field IDs are mapped to positive symbols, including external
ID `0`. Graph labels use a 64-bit symbol for `unsigned(id)+1`, so `UINT_MAX`
does not overflow. Seed-only field IDs are included when expanding normal
and store-rule wildcards.

The call-PDS builder follows the paper literally: its top is a statement, and
it does not add a bottom statement. A synchronized configuration always has a
current statement; a primitive call-PDS configuration whose statement stack
was entirely popped is not by itself such a synchronized configuration.

## APIs

### Lotus graph queries

```cpp
#include "CFL/InterleavedDyck/SPDS/Solver.h"
namespace dyck = lotus::cfl::interleaved_dyck;
namespace spds = dyck::spds;

dyck::Graph graph;
graph.addEdge(0, 1, dyck::Label::openParenthesis(1));
graph.addEdge(1, 2, dyck::Label::openBracket(2));
graph.addEdge(2, 3, dyck::Label::closeParenthesis(1));
graph.addEdge(3, 4, dyck::Label::closeBracket(2));

spds::Solver solver;
auto all = solver.analyze(graph);
bool candidate = all.mayReach(0, 4);
auto forward = solver.analyzeFrom(graph, 0);
auto backward = solver.analyzeTo(graph, 4);
// Both true for this graph:
(void)forward.mayReach(4);
(void)backward.mayReach(0);
// Precise endpoint/predecessor stack pair at node 2:
(void)forward.mayAccept(2, {1}, {2});
(void)backward.mayAccept(2, {1}, {2});
```

`Options::parentheses` and `Options::brackets` select `StackAcceptance::Empty`
(default) or `Any` for the queried vertex. In forward mode, Any permits pending
opens but never mismatched pops or underflow. In backward mode, it quantifies
over predecessor stacks that can reach the empty-stack anchor, which is a
different question from accepting a forward prefix. The CLI only exposes
prefix flags for forward queries to avoid that ambiguity.

`analyze` compiles the two projections once and performs two saturations per
source. `analyzeFrom` / `analyzeTo` only perform two saturations. Source/target
anchors must exist; queries about an unknown candidate vertex return false.
`Result` also exposes both independent projection relations and total statistics.
No graph edges are symmetrized; duplicate arcs are handled by the shared Graph.

### Synchronized data-flow and typestate

```cpp
#include "CFL/InterleavedDyck/SPDS/Synchronized.h"
namespace spds = lotus::cfl::interleaved_dyck::spds;

spds::RelationSemiring weights(3);
spds::SynchronizedSystem<spds::RelationSemiring> system(weights);
// Nodes are {globally_unique_variable_id, statement_id}.
system.addNormal({0, 1}, {1, 2}, weights.transition(0, 1));
system.addStore({1, 2}, {2, 3}, 7);
system.addCall({2, 3}, {3, 10}, 4, weights.transition(1, 2));
system.addReturn({3, 10}, {4, 4});
system.addLoad({4, 4}, {5, 5}, 7);

auto result = system.postStar({{0, 1}, {}, {}});
// Explicit empty calling context and empty access path:
auto relation = result.weight({{5, 5}, {}, {}});
bool reaches_state_2 = relation.contains(0, 2);
// Any calling context at the callee, with access path field 7:
auto in_callee = result.weightAt({3, 10}, {7});
// Exact backward reachability for each projection:
auto predecessors = system.preStar({{5, 5}, {}, {}});
```

For unweighted use, `SynchronizedSystem<>` uses `BooleanSemiring`.
`mayAlias(node)` asks whether the base variable (empty field sequence) reaches
the tracked seed at that node under some context. `mayReachNode(node)` allows
both an arbitrary context and an arbitrary access path. The names intentionally
separate those questions. `weightAt` filters the weighted call answer through
the unweighted field automaton, as described in Section 5.2.

### Incremental rules and regular languages

```cpp
spds::PushdownSystem<> pds;
auto x = pds.addControl();
auto y = pds.addControl();
auto seed = spds::RegularSet::singleton(pds.controls(), {x, {42}});
spds::SaturationSession<> session(pds, seed);
session.run();
session.addRule(x, 42, y, {43});
const auto &updated = session.run();
// updated.accepts(y, {43}) == true
```

Controls are fixed for a session; new rules may refer to any predeclared
control. An existing transition's new weight is propagated as well. Call
`run()` after insertions. `result()` throws while work is incomplete. Returned
references belong to the session and must not outlive it. The higher-level
`SynchronizedSystem` can also accept new transfers between queries, but its
next `postStar`/`preStar` rebuilds the pair; it does not automatically maintain
a coordinated pair of incremental sessions.

`RegularSet` has explicit state, transition and final-state construction for
regular rather than singleton seeds. `postStar(pds, seed)` and
`preStar(pds, target)` return immutable queryable `Automaton` values. Use
`accepts(p,{})` for an empty PDS stack, or `acceptsPrefix(p,{})` for existence of
any accepted stack. Weighted equivalents are `weight` and `weightWithPrefix`.

## Complexity and resource limits

There is no stack-height, path-length, or iteration cutoff. Cycles in the
finite automata represent unbounded stacks. Exact single-PDS saturation
terminates over the stated finite-height semiring contract.

This implementation favors explicit inspectable automata over specialized
compression. It materializes epsilon-composed edges and uses ordered maps.
Do not ascribe the paper's experimental speedups or its optimized implementation
complexity to this code without measurement.

A conservative implementation bound uses `N` automaton states, `A` distinct
stack symbols, `T <= N*N*(A+1)` possible transitions, `R` PDS rules, and `H`
maximum strict promotions of one transition weight. Epsilon propagation costs
at most `O(H*T*T)` edge-composition attempts; rule propagation contributes at
most `O(H*R*N)` for post* or `O(H*R*N*N)` for pre*, before map and semiring
operation costs. Storage is `O(T + R*N + N)` records (plus weights, seed input,
and rule storage). This is deliberately conservative rather than an asserted
tight bound. Boolean H is one. Graph all-pairs mode multiplies per-source work
by the vertex count and additionally stores quadratic pair relations.

`Limits` can bound automaton states, stored transitions, or successful weight
updates. Zero means unlimited. Counts include seed normalization and initial
transitions. Limits are per PDS saturation, not whole-process memory limits;
they do not bound input parsing/rule compilation or query evaluation costs.
Exceeding a limit throws `ResourceLimit`; no incomplete relation is returned
as an upper bound. A failed incremental session is poisoned and cannot return
or resume a partial result. Discard it and restart with an appropriate budget.

## Build and CLI

In an already configured Lotus build, reconfigure with its usual dependencies
and options, then build:

```sh
cmake -S . -B build -DLOTUS_BUILD_TESTS=ON
cmake --build build --target CanaryInterleavedDyckSPDS \
  lotus-cfl-interleaved-dyck-spds interleaved_dyck_spds_test
ctest --test-dir build -R interleaved_dyck_spds --output-on-failure

build/bin/lotus-cfl-interleaved-dyck-spds --query 0 4 \
  tests/regress/CFL/InterleavedDyck/SPDS/crossing.dot
build/bin/lotus-cfl-interleaved-dyck-spds --query 0 4 --backward \
  tests/regress/CFL/InterleavedDyck/SPDS/crossing.dot
```

The CLI defaults to all pairs and accepts `--source`, `--target`, `--query`,
`--backward`, `--call-prefix`, `--field-prefix`, `--pairs`, and resource limits.
Run `--help` for the complete syntax. Exit 0 means a completed computation
(including an unreachable answer), 2 means invalid input, and 3 means a resource
limit. Failure cases print no result relation.

The existing shared DOT parser reads labeled edge lines, not standalone node
declarations. Use `--vertex ID` for an isolated vertex or `Graph::addVertex` in
C++. No Core parser changes are included in this patch.

## Validation

The shared test runner contains 23 suites and is wrapped by GoogleTest for
Lotus integration. The CTest integration adds 20 independent CLI cases.
Tests cover 7,381 exhaustive words, exact one-stack CFL closure on 250 random
cyclic graphs, concrete two-stack paths on 200 DAGs, weighted post*/pre* on 60
acyclic PDSs with complete concrete execution oracles, noncommutative weights,
recursive stacks, late promotions, regular-seed normalization, aliases,
resource limits, and Figures 3-8 / Table 4 examples. The Figure 7 tests use its
actual drawn edge labels; the paragraph following the figure has inconsistent
h/f spellings and is not silently treated as a different graph.

Complete concrete enumeration is used only where acyclicity proves finiteness.
The cyclic projection oracle is an independent exact CFL-closure algorithm,
not bounded two-stack exploration. Tests do not constitute a proof of exact
same-path reachability: deliberate false-positive regressions document that
the latter is not the engine's guarantee. See the package's VALIDATION.md for
commands, logs, and the full-checkout validation boundary.
