# Newtonian Program Analysis

We use the engine in `include/Dataflow/NPA/NPA.h`.

A generic method for solving *interprocedural dataflow equations* by *generalizing Newton’s method** to **ω-continuous semirings*.

The key insight is that Newton’s method can be reformulated **purely algebraically**, without division or limits, and applied to semirings.  
For idempotent semirings this avoids subtraction entirely; non-idempotent domains require a suitable `subtract`.

Analyses are expressed over an **ω-continuous semiring**: $⟨S, +, ·, 0, 1⟩$

- `+`: join / aggregation (may be non-idempotent)
- `·`: sequencing / composition
- supports infinite sums and a natural order `⊑`

This generalizes:
- lattices (classical dataflow analysis),
- language semirings,
- counting semirings,
- probabilistic and cost semirings.

## Solver layers

The current implementation exposes three related layers that are easy to blur
together if you only look at file names:

- **Kleene solver**: solves the full system `X = f(X)` directly by repeated
  evaluation. Public entry point: `KleeneSolver<D>` in `Solver/KleeneSolver.h`.
- **JACM Newton/NPA solver**: solves the same full system `X = f(X)` by outer
  Newton iteration plus a linearized correction solve on each round. Public
  entry point: `NPASolver<D>` in `Solver/NPASolver.h`.
- **TOPLAS tensor-product machinery**: not a separate outer solver. It is an
  optional backend for the inner linearized system `Df|ν(X) + δ = X`, and only
  applies when the current system has LCFL structure and the domain explicitly
  opts into tensor semantics.

That means `LinearStrategy` is only relevant to the **Newton** path. It does
not choose between Kleene and Newton; it only chooses the inner backend used by
Newton for its current linearized system.

NPA intentionally keeps its semiring terminology rather than adopting the
Mono/APA lattice API. A base NPA domain provides `zero`, `one`, `combine`,
`extend`, `extend_lin`, `ndetCombine`, `condCombine`, and `equal`.
`Core/Domain.h` performs compile-time capability detection, while
`Solver/DomainValidation.h` checks identity, annihilation, associativity,
distributivity, idempotence, and consistency laws over identity and optional
representative values. `DomainContractMode::BasicChecks` records failures;
`DomainContractMode::Strict` rejects the solve.

Both public solvers validate equation systems before evaluation. Every LHS
must be unique, every free symbol must have an equation, local `Bound` symbols
must be in scope, and expressions must be non-null. Validation assigns dense
equation indices reused by SCC planning and tensor topology caching. Domain
values do not need a default constructor; solver storage is initialized from
the domain identities.



## TOPLAS 2016 / LCFL support

The engine supports **TOPLAS 2016**-style algorithms for LCFL (linear
context-free) linear sub-problems that arise inside Newton:

- **LinearStrategy**: `Naive`, `SCC`, `AdaptiveScc`, `TensorProduct`
- **SCC**: Solve in topological order of strongly connected components, using a local dependency-driven worklist within each SCC. This is the ordinary general-purpose inner backend.
- **AdaptiveScc**: Solve the linearized system SCC-by-SCC. Singleton acyclic SCCs use direct evaluation, ordinary recursive SCCs use the SCC worklist solver, and tensor-eligible cyclic LCFL SCCs use the tensor solver locally.
- **TensorProduct**: Rewrite LCFL terms into a tensorized left-linear system, solve there via Tarjan path expressions when extractable to a left-linear graph, and otherwise fall back to tensor-space worklist iteration.
- **TensorDiff**: Direct tensor-side differential builder used by the Newton tensor path.
- **TensorSemiringTraits**: Optional specialization point for domains that want to supply a custom tensor semiring/readout instead of the default exact-correlated tensor domain.
- **LCFLDetector**: `has_lcfl_structure(E1)` detects Concat/Star in linear RHS (used to decide whether tensor is applicable).

`Star` is the paper-faithful Newton/tensor construct. `Mu` is evaluable as a
generic least fixpoint, but NPA rejects it on Newton/tensor paths.

Domains that expose `project()` must additionally opt into `project_newton_safe`
before projection is accepted on Newton/tensor paths.
Domains may expose `project()`, `projectT()`, or both; when both exist,
`project()` takes precedence. Evaluating `Project` with neither operation throws
`UnsupportedDomainProjectError`. Width-dependent domains likewise throw if no
active `WidthScope` exists.

Use `KleeneSolver<D>::solve(eqns, ...)` for plain Kleene solving.
Use `NPASolver<D>::solve(eqns, verbose, -1, LinearStrategy::SCC)`,
`LinearStrategy::AdaptiveScc`, or `LinearStrategy::TensorProduct` for the JACM
Newton/NPA outer solver with different inner linear backends; or pass
`LinearStrategy` into `BitVectorSolver::run` (optional 5th parameter).

When using `AdaptiveScc`, the solver reports aggregate counts for SCC-local direct/worklist/tensor choices and tensor fallbacks in `Stat`.

## Public header structure

The public API lives under `include/Dataflow/NPA/`:

```text
include/Dataflow/NPA/
├── NPA.h
├── Core/
│   ├── Domain.h
│   ├── DomainExecution.h
│   ├── Symbol.h
│   └── Expr/                  # Immutable equation AST and evaluation
├── Solver/
│   ├── Options.h
│   ├── Statistics.h
│   ├── SolveContext.h
│   ├── Fixpoint.h
│   ├── KleeneSolver.h
│   ├── NPASolver.h
│   └── Newton/
│       ├── Differential.h
│       └── Linear/
│           ├── SccSolver.h
│           ├── AdaptivePlan.h
│           └── Tensor/        # Optional inner Newton backend
├── LLVM/                      # LLVM bit-vector and interprocedural engines
├── Domains/                   # Semiring and transformer domains
├── Analyses/Intra/            # Intraprocedural analysis clients
└── Analyses/Inter/            # Interprocedural analysis clients
```

The public and implementation trees are intentionally not exact mirrors.
`include/Dataflow/NPA/` also contains template implementations that must remain
visible to clients, while `lib/Dataflow/NPA/` contains only separately compiled
non-template implementations.

Notable entry points:

- `Solver/KleeneSolver.h` contains the public Kleene solver.
- `Solver/NPASolver.h` contains the public Newton/NPA solver.
- `Solver/Fixpoint.h` contains low-level fixpoint utilities reused
  internally.
- `Solver/Newton/Differential.h` implements ordinary and tensor-side
  differentials.
- `Solver/Newton/Linear/SccSolver.h` implements the ordinary inner
  linearized-system machinery used by Newton/NPA.
- `Solver/Newton/Linear/Tensor/` contains the optional TOPLAS tensor backend.
- `LLVM/ForwardInterEngine.h` and `LLVM/BackwardInterEngine.h` contain the
  interprocedural LLVM infrastructure.
- `LLVM/BitVectorSolver.h` contains the intraprocedural bit-vector bridge.
- `Analyses/Inter/` contains the public analysis wrappers used by
  the in-tree constant-propagation, interval, taint, nullability, and related
  clients.

## Usage notes

- Intraprocedural clients can use `BitVectorSolver` and related local engines.
- Inter forward clients use `InterEngine<Domain, Analysis>`.
- Inter backward clients use `BackwardInterEngine<Domain, Analysis>`.
- `TransformerSummary` is the current bounded abstract-summary path used
  by in-tree subdistributive clients such as interprocedural constant
  propagation and interval analysis. Transformer carriers live directly under
  `Domains/` because they satisfy the same domain interface as ordinary solver
  domains.

## Execution model

NPA solving is serial. Newton initial-value construction, per-round RHS
construction, SCC traversal, SCC-local worklists, tensor solving, and LLVM
interprocedural artifact construction and propagation all execute on the
calling thread. `AdaptiveScc` still chooses `Direct`, `Worklist`, or `Tensor`
per SCC, but processes those components deterministically in condensation-DAG
order.

Domains with `approx_equal` use it by default. A solve can instead select
`ConvergencePolicy::Exact`, which uses `equal` throughout that solve and reports
exact convergence status. A `max_fixpoint_iters` value of zero permits zero
updates; negative values remain unlimited.

## Related Work

- Compositional Recurrence Analysis Revisited. PLDI 17.
- Newtonian Program Analysis via Tensor Product. POPL 16.
- Newtonian Program Analysis, JACM 10.
