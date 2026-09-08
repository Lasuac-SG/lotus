# AffineSPDS: affine history synchronization for Lotus

This C++17 engine strengthens the endpoint-only SPDS upper bound by comparing
**joint affine relations between histories of projected witnesses**. It uses the
existing `spds::PushdownSystem`, `postStar`, `preStar`, and `SaturationSession`
with an `AffineSemiring` weight domain. It neither replaces those algorithms nor
changes Core or the Boolean SPDS engine.

This is the proposed affine-history extension discussed with the user, **not an
algorithm attributed to the POPL 2019 paper**. The paper supplies the separate
call/field PDS construction and weighted-saturation interface; the synchronized
history interpretation and new implementation are an extension.

## Semantics and scope

For a fixed shared map from original graph edges to GF(2) matrices, let
`rho(e1 ... ek) = M_e1 ... M_ek` and `rho(epsilon) = I`. For the call-valid and
field-valid trace languages of a query, the engine computes

```
H_call  = aff { rho(w) : w is a call-valid projected witness }
H_field = aff { rho(w) : w is a field-valid projected witness }
```

A pair is retained only if these affine spaces intersect. For empty endpoint
stacks, the following inclusions hold:

```
concrete same-path two-stack reachability
  <= joint affine history synchronization
  <= independent diagonal-block readout
  <= Boolean SPDS endpoint intersection.
```

The implementation computes exact affine hulls for the **fixed observer** on
each individual projection. It does not retain exact sets of matrices and does
not solve arbitrary same-path two-stack reachability exactly. Intersecting two
nonempty hulls may retain a false positive. Consequently, all APIs use `mayReach`
and `upper_bound`: false establishes unreachability under the supplied graph
semantics; true is not a concrete witness or a feasible program execution.

The library supports arbitrary directed graphs, typed labels, neutral edges,
cycles, sparse signed 64-bit vertices, maximum unsigned label IDs, isolated
vertices, and nonempty-stack queries. Stack and path lengths are not bounded.
The feature budget bounds the observation, not the execution.

## Build and integration

Library target: `CanaryInterleavedDyckAffineSPDS`.
CLI target: `lotus-cfl-interleaved-dyck-affine-spds`.
Lotus GoogleTest target: `interleaved_dyck_affine_spds_test`.
Namespace: `lotus::cfl::interleaved_dyck::affine`.

Register the engine after its dependency:

```cmake
# lib/CFL/InterleavedDyck/CMakeLists.txt
add_subdirectory(Core)
add_subdirectory(SPDS)
add_subdirectory(AffineSPDS)
```

The engine registers after its dependency in `lib/CFL/InterleavedDyck/CMakeLists.txt`:
`Core`, then `SPDS`, then `AffineSPDS`. It does not require the LCL patch.

In an already configured Lotus build, reconfigure with its usual dependencies
and options, then build:

```sh
cmake -S . -B build -DLOTUS_BUILD_TESTS=ON
cmake --build build --target CanaryInterleavedDyckAffineSPDS \
  lotus-cfl-interleaved-dyck-affine-spds interleaved_dyck_affine_spds_test
ctest --test-dir build -R interleaved_dyck_affine_spds --output-on-failure
```

## Main API

```cpp
#include "CFL/InterleavedDyck/AffineSPDS/Solver.h"
namespace dyck = lotus::cfl::interleaved_dyck;
namespace affine = dyck::affine;

int main() {
  dyck::Graph graph;
  graph.addEdge(0, 1, dyck::Label::openParenthesis(1));
  graph.addEdge(1, 2, dyck::Label::openBracket(2));
  graph.addEdge(2, 3, dyck::Label::closeParenthesis(1));
  graph.addEdge(3, 4, dyck::Label::closeBracket(2));

  affine::Solver solver;
  auto forward = solver.analyzeFrom(graph, 0);
  auto backward = solver.analyzeTo(graph, 4);
  const auto &comparison = forward.compare(4);

  // The positive answer in a general graph is only a candidate.
  return comparison.mayReach() && backward.mayReach(0) &&
         forward.mayAccept(2, {1}, {2}) ? 0 : 1;
}
```

`compare(v)` exposes both hulls, a verdict, and an optional separation equation.
`compareStacks(v, calls, fields)` queries precise stacks, top first. Forward
queries start with empty stacks at the source; backward queries fix empty stacks
at the target and ask about predecessor stacks. `Options::parentheses` and
`Options::brackets` select `spds::StackAcceptance::Any` for an existential stack
at the queried vertex instead of the default empty stack.

`analyze(graph)` returns three pair sets: `upper_bound`, `spds_upper_bound`, and
`independent_upper_bound`, as well as saturation statistics. The graph-to-PDS
conversion is reused across sources, but this version performs separate
saturations per source; it does not claim cross-query summary sharing.

Query results cache default-stack readouts. A single `QueryResult` is therefore
not safe for concurrent readout from several threads without synchronization.
Distinct solvers/results share no mutable global analysis state.

## Fixed computational algebra

The matrix dimension is dynamic. Packed bit vectors and row-slice XOR implement
GF(2) arithmetic without a 32/64-bit dimension cap or an external algebra library.
There are tests crossing word boundaries at dimensions 9, 63, 64, and 65.

An affine space is either empty or `a + span(B)`. `B` is a canonical reduced
row-echelon basis; `a` is reduced by that basis. Equality is semantic, so redundant
derivations do not keep a worklist alive and insertion order does not affect the
result.

The semiring operations are:

```
zero         = the empty affine space
one          = { I }
combine(U,V) = aff(U union V)
extend(U,V)  = aff { X*Y : X in U, Y in V }
```

A singleton containing the all-zero MATRIX is **not** semiring zero. Alternative
paths are combined with affine hull, never XOR-summed: XOR summation would
incorrectly cancel real witnesses.

For `U = a + span(u_i)` and `V = b + span(v_j)`, multiplication computes

```
ab + span( u_i*b, a*v_j, u_i*v_j ).
```

The bilinear `u_i*v_j` terms must not be omitted. The implementation does not
enumerate the exponentially many points in a represented affine space.
Bilinearity implies

```
aff( aff(S) * aff(T) ) = aff(S*T).
```

Together with exact joins, this means intermediate saturation summaries lose no
information beyond taking the affine hull of concrete projected histories.
Associativity, distributivity, zero annihilation, and idempotent combine then
justify using the existing finite-height weighted pushdown algorithm.

For matrix dimension `r`, the ambient vector dimension is `D = r*r`. A summary
has at most `D` independent directions, and an increasing chain has at most
`D+2` elements including bottom. This provides finite-height termination without
any bound on stack depth.

A rank-h summary stores O((h+1)*ceil(D/64)) packed words, plus container overhead.
Multiplying rank-h and rank-k summaries generates at most `1+h+k+h*k` candidate
vectors before reduction. Matrix multiplication uses packed row slices; basis
insertion uses Gaussian elimination. These bounds describe the domain costs;
they do not establish practical scalability for arbitrary large observers.

## How matrices are chosen

The fixed algebra is not restricted to triangular matrices, parity counts, or
pattern observers. `HistoryObserver::set(edge, matrix)` accepts arbitrary square
GF(2) matrices, including singular and zero matrices. Both projections use the
same matrix for the same original edge.

The default **deterministic graph-only heuristic** works as follows:

1. Sort original edges by `(source, target, label kind, label id)`.
2. Select one non-default outgoing edge at each branch, then additional branch
   alternatives, then remaining sorted edges, up to `max_events` (default 4).
3. Construct a 2x2 parity observer for each selected edge and 3x3 ordered-pair
   observers for the first selected pairs, up to `max_order_pairs` (default 2).
4. Form their direct sum and saturate the **joint** affine hull. Default matrix
   dimension is at most 14. No selection yields the 1x1 identity observer.

Pair enumeration is prefix-stable under increasing the event budget. Increasing
these budgets retains the existing component observations, up to block
permutation, so it does not weaken the joint result. `directSum` is also an
explicit monotone extension operation.

This policy requires no client-written algebraic laws, no random edge hashes,
and no SAT/automata synthesis. It is an experimental default, not a claim that
these few selected events suffice for all clients or programs. No real-program
benchmark evaluation of the policy is included.

Additional deterministic constructors:

- `parity(events)`: one bit toggled for each event in the set.
- `orderedPair(first, second)`: counts first events, second events, and ordered
  first-before-second subsequences modulo two. Events can belong to both sets;
  one event is not counted as preceding itself.
- `cyclic(events, modulus)`: a permutation-matrix observer, including modulo-three
  distinctions not expressible by the earlier truncated unary parity hierarchy.
- `directSum(observers)`: one joint observation with component boundaries.

These are observer constructors within ONE computational algebra, not separate
saturation engines or client-selected algebra laws.

An edge not explicitly mapped carries identity. Core deduplicates identical
`(source,target,label)` edges; distinct semantic events must therefore be distinct
graph edges or be represented using the lower-level PDS/client API.

## Joint versus independent synchronization

`correlation.dot` contains two binary choices, x and y. The call projection
accepts exactly x=y, whereas the field projection accepts exactly x!=y. Every
edge is supported by both projected queries, but there is no common balanced
path. The supplied observer tracks x and y in two parity blocks.

```sh
FIXTURES=tests/regress/CFL/InterleavedDyck/AffineSPDS
build/bin/lotus-cfl-interleaved-dyck-affine-spds \
  --query 0 14 --observer "$FIXTURES/correlation.observer" \
  --certificate "$FIXTURES/correlation.dot"
```

The result includes:

```
query: unreachable
joint: unreachable
spds: may-reach
independent: may-reach
joint-reason: affine-separated
certificate-functional: 0100/0000/0001/0000
certificate-call-value: 0
certificate-field-value: 1
certificate-verified-against-hulls: true
```

The functional is `M[0,1] XOR M[2,3]`: x XOR y. Its constant value differs
between the two projected witness hulls. No client supplied that equation; it
was obtained from the joint relations.

`--mode independent` selects the weaker per-block readout and retains this pair.
`--mode spds` selects endpoint nonemptiness. **Both modes still compute the same
joint weighted saturation**; they are precision ablations, not independent
performance implementations. For a Boolean performance baseline, use the
original SPDS executable, not `--mode spds`. `--identity` is another semantic
baseline, with a trivial observer and very small domain overhead.

For custom non-block-diagonal matrices, independent mode is a projection of the
full computed hull onto diagonal blocks. It remains a sound weakening, but it
need not equal a separately composed block observer. Default/direct-sum matrices
are block diagonal, where the two interpretations agree.

## Certificates

When both hulls are nonempty and disjoint, `separate(left,right)` produces a
`SeparationCertificate { functional, left_value, right_value }`. The functional
annihilates both direction spaces, and its evaluations on their offsets differ.
The implementation derives it directly from the reduced basis of the union of
directions and the reduced offset difference.

`certificate.verify(left,right)` checks all these conditions. CLI output can
include the certificate in text or JSON.

**Certificate boundary:** this verifies separation against the supplied affine
hulls. It is not an independent proof that the hulls cover all pushdown paths.
The saturation implementation and event mapping remain part of the trusted
computation. Empty-projection rejection needs no separating functional, and
reports `ProjectionRejected` with no certificate. A certificate is not returned
for overlapping hulls.

## Observer file format and CLI

```
# Comments begin with #. Missing original edges use identity.
dimension 2
block 0 2
edge 0 1 op--1 11/01
```

Each edge directive gives source, target, a Core label, and slash-separated bit
rows. Optional block directives must partition the diagonal in order. Unknown
edges, duplicate edge assignments, invalid dimensions/rows/IDs, and incomplete
block partitions are rejected. `--dump-observer` preserves the matrix map and
block metadata, enabling reproducible experiments.

`--json` emits numeric statistics, the selected result, the joint/SPDS/independent
answers for single queries, optional sorted pairs, and optional certificates.
`--backward`, `--source`, `--target`, explicit stack flags, and prefix flags are
listed by `--help`. The default query is all-pairs with empty stacks.

The Core DOT parser currently reads edge statements, not isolated vertex
statements. Use `--vertex V` or `Graph::addVertex(V)` to retain isolated vertices.
This engine does not change the Core parser.

## Front-end-neutral synchronized data-flow builder

`AffineSPDS/Synchronized.h` provides `affine::SynchronizedSystem`. It follows the
previous SPDS variable/statement encoding, but carries history weights on BOTH
systems and compares their hulls at synchronized configurations:

```cpp
#include "CFL/InterleavedDyck/AffineSPDS/Synchronized.h"
namespace a = lotus::cfl::interleaved_dyck::affine;

a::SynchronizedSystem system(2);
a::Matrix event = a::Matrix::parse("11/01");
system.addCall({0,10}, {1,20}, 11, event);
system.addStore({1,20}, {2,21}, 7);
system.addReturn({2,21}, {3,11});
system.addLoad({3,11}, {4,12}, 7);
auto result = system.postStar({{0,10}, {}, {}});
bool candidate = result.mayAccept({{4,12}, {}, {}});
```

`addNormal`, `addStore`, `addLoad`, `addCall`, and `addReturn` take an optional
matrix (identity when omitted). A transfer's matrix is attached to every
corresponding rule in both projections exactly once. `compareAt` queries an
exact access path in any calling context; `compareNode` permits any access path
and context. `preStar` is also available.

Clients must supply data-flow rules, globally scoped variable identities,
actual/formal mappings, kills, and alias-induced transfers. This is not a new
LLVM/Soot front end, automatic alias solver, or strong-update analysis. These
history matrices are not protocol/typestate weights: property-coupled typestate
and observer synthesis are deliberately outside this implementation.

## Generic and incremental PDS access

`AffineSemiring` satisfies the earlier SPDS domain interface. It can be used
with arbitrary PDS rules and unweighted regular seeds, including incoming seed
edges to controls and accepting controls, which SPDS normalizes internally.

```cpp
affine::AffineSemiring domain(2);
spds::PushdownSystem<affine::AffineSemiring> system(domain);
auto p = system.addControl();
auto q = system.addControl();
system.addRule(p, 0, q, {0}, domain.lift(affine::Matrix::parse("11/01")));
auto seed = spds::RegularSet::singleton(system.controls(), {p, {0}});
spds::SaturationSession<affine::AffineSemiring> session(system, seed);
const auto &automaton = session.run();
```

New rules can be added with `session.addRule(...)`, followed by `run()`. Improved
weights are re-enqueued even on existing transitions. Rule deletions, changing
the observer after saturation, and automatically incremental graph conversion
are not supported. Fix all controls before starting a session.

`affine::synchronize` combines queries to two affine-weighted PDS automata. It
checks dimensions and direction, but cannot verify that a client assigned the
same event semantics to its two rule systems. Use the graph solver or the
synchronized builder when that shared mapping should be enforced structurally.

## Limits and validation

`Options::limits` applies to each individual PDS saturation. Optional limits
cover states, transitions, and weight promotions; `max_matrix_dimension` guards
the accepted observer dimension before graph saturation. Allocation/resource
failures throw exceptions. No rank truncation, widening, partial saturation, or
resource exhaustion is silently interpreted as a sound negative result.
The dimension check is not a global memory budget for constructing an observer;
choose modest feature budgets for large graphs.

Readout over a saturated automaton is finite-height too, but its extra work is
not counted against the saturation's update limit. This version does not expose
a whole-analysis wall-clock or memory-budget controller.

The CLI buffers result output until the complete requested operation succeeds.
Exit codes are 0 (complete), 2 (invalid input/I/O), and 3 (resource failure).

The shared C++ tests cover exhaustive affine separation, semiring laws, concrete
DAG executions, exact finite-image CFL closure on cyclic graphs, exact acyclic
PDS execution, forward/backward order, 200-symbol recursive stack queries,
regular-seed normalization, incremental promotions, observer round trips,
insertion order, width beyond 64 bits, and precision inclusion. The package's
`VALIDATION.md` gives exact test counts and toolchain logs.

## References / attribution

- J. Späth, K. Ali, E. Bodden. Context-, Flow-, and Field-Sensitive Data-Flow
  Analysis using Synchronized Pushdown Systems. POPL 2019, article 48.
  DOI: 10.1145/3290361. In particular Definition 4, Section 4.1, and Sections 5.2–5.3.
- The affine-set-of-matrices representation is related to the established
  interprocedural affine-relation/MOS domain. This implementation does not claim
  to invent affine matrix domains or their semiring properties.
- Lotus integration interfaces were inspected at
  https://github.com/ZJU-PL/lotus/tree/main/include/CFL/InterleavedDyck and
  https://github.com/ZJU-PL/lotus/tree/main/lib/CFL/InterleavedDyck.
