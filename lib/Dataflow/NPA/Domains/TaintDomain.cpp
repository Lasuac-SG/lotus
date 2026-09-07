/*
 *
 * Author: rainoftime
 */
#include "Dataflow/NPA/Domains/TaintDomain.h"

#include <utility>
#include <vector>

namespace npa {

TaintTransformer::value_type TaintTransformer::zero() { return {}; }

TaintTransformer::value_type TaintTransformer::one() {
  value_type result;
  result.identity = true;
  return result;
}

TaintTransformer::fact_type TaintTransformer::row(
    const value_type &transfer, unsigned input) {
  if (const fact_type *override = transfer.rows.find(input))
    return *override;
  fact_type result;
  if (transfer.identity)
    result.set(input);
  return result;
}

void TaintTransformer::setRow(value_type &transfer, unsigned input,
                              fact_type newRow) {
  fact_type defaultRow;
  if (transfer.identity)
    defaultRow.set(input);
  if (newRow == defaultRow) {
    transfer.rows.erase(input);
    return;
  }
  transfer.rows.set(input, newRow);
}

bool TaintTransformer::equal(const value_type &a, const value_type &b) {
  return a == b;
}

TaintTransformer::value_type
TaintTransformer::combine(const value_type &a, const value_type &b) {
  if (equal(a, b))
    return a;
  if (!a.identity && a.rows.empty() && a.gen.empty())
    return b;
  if (!b.identity && b.rows.empty() && b.gen.empty())
    return a;

  const bool result_identity = a.identity || b.identity;
  const value_type *base = nullptr;
  if (a.identity == result_identity && b.identity == result_identity)
    base = a.rows.size() >= b.rows.size() ? &a : &b;
  else
    base = a.identity == result_identity ? &a : &b;
  value_type result = *base;
  result.identity = result_identity;
  result.gen = a.gen;
  result.gen |= b.gen;

  auto combine_row = [&](unsigned input) {
    fact_type joined = row(a, input);
    joined |= row(b, input);
    setRow(result, input, std::move(joined));
  };
  a.rows.forEach(
      [&](unsigned input, const fact_type &) { combine_row(input); });
  b.rows.forEach([&](unsigned input, const fact_type &) {
    if (!a.rows.find(input))
      combine_row(input);
  });
  return result;
}

TaintTransformer::value_type
TaintTransformer::ndetCombine(const value_type &a, const value_type &b) {
  return combine(a, b);
}

TaintTransformer::value_type
TaintTransformer::condCombine(bool phi, const value_type &t,
                              const value_type &e) {
  return phi ? t : e;
}

TaintTransformer::fact_type TaintTransformer::applyRelation(
    const value_type &transfer, const fact_type &input) {
  fact_type result;
  for (unsigned source : input) {
    const fact_type *outputs = transfer.rows.find(source);
    if (outputs) {
      result |= *outputs;
      continue;
    }
    if (transfer.identity)
      result.set(source);
  }
  return result;
}

TaintTransformer::value_type
TaintTransformer::extend(const value_type &a, const value_type &b) {
  // a after b: a composed with b.
  if (!a.identity && a.rows.empty() && a.gen.empty())
    return zero();
  if (a.identity && a.rows.empty() && a.gen.empty())
    return b;
  if (b.identity && b.rows.empty() && b.gen.empty())
    return a;

  const bool result_identity = a.identity && b.identity;
  value_type result;
  result.identity = result_identity;
  if (b.identity == result_identity)
    result.rows = b.rows;

  auto compose_row = [&](unsigned input) {
    fact_type composed = applyRelation(a, row(b, input));
    setRow(result, input, std::move(composed));
  };
  b.rows.forEach(
      [&](unsigned input, const fact_type &) { compose_row(input); });
  if (b.identity) {
    a.rows.forEach([&](unsigned input, const fact_type &) {
      if (!b.rows.find(input))
        compose_row(input);
    });
  }

  result.gen = applyRelation(a, b.gen);
  result.gen |= a.gen;
  return result;
}

TaintTransformer::value_type
TaintTransformer::extend_lin(const value_type &a, const value_type &b) {
  return extend(a, b);
}

TaintTransformer::value_type
TaintTransformer::subtract(const value_type &a, const value_type &) {
  return a;
}

TaintTransformer::value_type
TaintTransformer::star(const value_type &value) {
  value_type closure = one();
  for (;;) {
    value_type next = combine(closure, extend(value, closure));
    if (equal(next, closure))
      return next;
    closure = std::move(next);
  }
}

TaintTransformer::fact_type
TaintTransformer::apply(const value_type &transfer, const fact_type &input) {
  fact_type result = applyRelation(transfer, input);
  result |= transfer.gen;
  return result;
}

void TaintTransformer::addEdge(value_type &transfer, unsigned from,
                               unsigned to) {
  fact_type outputs = row(transfer, from);
  outputs.set(to);
  setRow(transfer, from, std::move(outputs));
}

void TaintTransformer::addGen(value_type &transfer, unsigned bit) {
  transfer.gen.set(bit);
}

void TaintTransformer::clearInput(value_type &transfer, unsigned input) {
  setRow(transfer, input, {});
}

void TaintTransformer::clearOutput(value_type &transfer, unsigned output) {
  std::vector<std::pair<unsigned, fact_type>> updates;
  transfer.rows.forEach([&](unsigned input, const fact_type &outputs) {
    if (!outputs.test(output))
      return;
    fact_type updated = outputs;
    updated.reset(output);
    updates.emplace_back(input, std::move(updated));
  });
  for (auto &update : updates)
    setRow(transfer, update.first, std::move(update.second));

  if (transfer.identity) {
    fact_type outputRow = row(transfer, output);
    outputRow.reset(output);
    setRow(transfer, output, std::move(outputRow));
  }
  transfer.gen.reset(output);
}

void TaintTransformer::kill(value_type &transfer, unsigned bit) {
  clearInput(transfer, bit);
  clearOutput(transfer, bit);
}

} // namespace npa
