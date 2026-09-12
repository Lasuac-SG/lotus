#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace elimination {

// A compact set for dataflow facts. Values are interned once in a shared
// universe and facts store only a copy-on-write bitmap. This is intentionally a
// small container rather than a general STL set: APA clients need membership,
// iteration, insertion, erasure, union, and intersection.
template <typename T> class IndexedSet {
public:
  struct Universe {
    std::map<T, std::size_t> Indices;
    std::vector<T> Values;

    std::size_t intern(const T &Value) {
      auto It = Indices.find(Value);
      if (It != Indices.end())
        return It->second;
      const std::size_t Index = Values.size();
      Values.push_back(Value);
      Indices.emplace(Value, Index);
      return Index;
    }

    std::size_t find(const T &Value) const {
      auto It = Indices.find(Value);
      return It == Indices.end() ? npos : It->second;
    }

    static constexpr std::size_t npos = static_cast<std::size_t>(-1);
  };

  using universe_ptr = std::shared_ptr<Universe>;

  class const_iterator {
  public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = T;
    using difference_type = std::ptrdiff_t;
    using pointer = const T *;
    using reference = const T &;

    const_iterator() = default;

    reference operator*() const { return Owner->U->Values[Index]; }
    pointer operator->() const { return &Owner->U->Values[Index]; }

    const_iterator &operator++() {
      ++Index;
      advance();
      return *this;
    }

    const_iterator operator++(int) {
      auto Copy = *this;
      ++*this;
      return Copy;
    }

    friend bool operator==(const const_iterator &Lhs,
                           const const_iterator &Rhs) {
      return Lhs.Owner == Rhs.Owner && Lhs.Index == Rhs.Index;
    }

    friend bool operator!=(const const_iterator &Lhs,
                           const const_iterator &Rhs) {
      return !(Lhs == Rhs);
    }

  private:
    friend class IndexedSet;

    const_iterator(const IndexedSet *Owner, std::size_t Index)
        : Owner(Owner), Index(Index) {
      advance();
    }

    void advance() {
      if (!Owner)
        return;
      const auto Limit = Owner->U->Values.size();
      while (Index < Limit && !Owner->testIndex(Index))
        ++Index;
    }

    const IndexedSet *Owner = nullptr;
    std::size_t Index = 0;
  };

  IndexedSet() : IndexedSet(std::make_shared<Universe>()) {}

  explicit IndexedSet(universe_ptr U)
      : U(U ? std::move(U) : std::make_shared<Universe>()),
        Words(std::make_shared<std::vector<std::uint64_t>>()) {}

  IndexedSet(std::initializer_list<T> Values) : IndexedSet() {
    insert(Values.begin(), Values.end());
  }

  static universe_ptr makeUniverse() { return std::make_shared<Universe>(); }

  static universe_ptr makeUniverse(const std::vector<T> &Values) {
    auto U = makeUniverse();
    for (const auto &Value : Values)
      U->intern(Value);
    return U;
  }

  static IndexedSet full(universe_ptr U) {
    IndexedSet Result(std::move(U));
    Result.ensureWords(Result.U->Values.size());
    std::fill(Result.Words->begin(), Result.Words->end(), ~std::uint64_t{0});
    Result.clearUnusedBits();
    return Result;
  }

  universe_ptr universe() const { return U; }

  bool empty() const { return size() == 0; }

  std::size_t size() const {
    std::size_t Count = 0;
    for (auto Word : *Words)
      Count += static_cast<std::size_t>(__builtin_popcountll(Word));
    return Count;
  }

  std::size_t count(const T &Value) const {
    const auto Index = U->find(Value);
    return Index != Universe::npos && testIndex(Index) ? 1u : 0u;
  }

  const_iterator find(const T &Value) const {
    const auto Index = U->find(Value);
    return Index != Universe::npos && testIndex(Index)
               ? const_iterator(this, Index)
               : end();
  }

  std::pair<const_iterator, bool> insert(const T &Value) {
    const auto Index = U->intern(Value);
    if (testIndex(Index))
      return {const_iterator(this, Index), false};
    ensureWords(Index + 1);
    (*Words)[wordIndex(Index)] |= bitMask(Index);
    return {const_iterator(this, Index), true};
  }

  template <typename It> void insert(It Begin, It End) {
    for (; Begin != End; ++Begin)
      insert(*Begin);
  }

  std::size_t erase(const T &Value) {
    const auto Index = U->find(Value);
    if (Index == Universe::npos || !testIndex(Index))
      return 0;
    detachWords();
    (*Words)[wordIndex(Index)] &= ~bitMask(Index);
    return 1;
  }

  const_iterator erase(const_iterator It) {
    if (It == end())
      return It;
    const auto Index = It.Index;
    detachWords();
    (*Words)[wordIndex(Index)] &= ~bitMask(Index);
    return const_iterator(this, Index + 1);
  }

  void clear() { Words = std::make_shared<std::vector<std::uint64_t>>(); }

  const_iterator begin() const { return const_iterator(this, 0); }
  const_iterator end() const { return const_iterator(this, U->Values.size()); }
  const_iterator begin() { return const_iterator(this, 0); }
  const_iterator end() { return const_iterator(this, U->Values.size()); }

  void unionWith(const IndexedSet &Other) {
    if (U != Other.U) {
      insert(Other.begin(), Other.end());
      return;
    }
    const auto Size = std::max(Words->size(), Other.Words->size());
    ensureWords(Size * bitsPerWord());
    for (std::size_t I = 0; I < Other.Words->size(); ++I)
      (*Words)[I] |= (*Other.Words)[I];
  }

  void intersectWith(const IndexedSet &Other) {
    if (U != Other.U) {
      for (auto It = begin(); It != end();) {
        if (!Other.count(*It))
          It = erase(It);
        else
          ++It;
      }
      return;
    }
    detachWords();
    const auto Common = std::min(Words->size(), Other.Words->size());
    for (std::size_t I = 0; I < Common; ++I)
      (*Words)[I] &= (*Other.Words)[I];
    for (std::size_t I = Common; I < Words->size(); ++I)
      (*Words)[I] = 0;
  }

  void subtract(const IndexedSet &Other) {
    if (U != Other.U) {
      for (const auto &Value : Other)
        erase(Value);
      return;
    }
    detachWords();
    const auto Common = std::min(Words->size(), Other.Words->size());
    for (std::size_t I = 0; I < Common; ++I)
      (*Words)[I] &= ~(*Other.Words)[I];
  }

  friend bool operator==(const IndexedSet &Lhs, const IndexedSet &Rhs) {
    if (Lhs.U != Rhs.U) {
      if (Lhs.size() != Rhs.size())
        return false;
      for (const auto &Value : Lhs)
        if (!Rhs.count(Value))
          return false;
      return true;
    }
    const auto Size = std::max(Lhs.Words->size(), Rhs.Words->size());
    for (std::size_t I = 0; I < Size; ++I) {
      const auto LW = I < Lhs.Words->size() ? (*Lhs.Words)[I] : 0;
      const auto RW = I < Rhs.Words->size() ? (*Rhs.Words)[I] : 0;
      if (LW != RW)
        return false;
    }
    return true;
  }

  friend bool operator!=(const IndexedSet &Lhs, const IndexedSet &Rhs) {
    return !(Lhs == Rhs);
  }

private:
  static constexpr std::size_t bitsPerWord() { return 64; }
  static std::size_t wordIndex(std::size_t Index) {
    return Index / bitsPerWord();
  }
  static std::uint64_t bitMask(std::size_t Index) {
    return std::uint64_t{1} << (Index % bitsPerWord());
  }

  bool testIndex(std::size_t Index) const {
    const auto Word = wordIndex(Index);
    return Word < Words->size() && ((*Words)[Word] & bitMask(Index)) != 0;
  }

  void detachWords() {
    if (Words.use_count() != 1)
      Words = std::make_shared<std::vector<std::uint64_t>>(*Words);
  }

  void ensureWords(std::size_t Bits) {
    const auto Required = (Bits + bitsPerWord() - 1) / bitsPerWord();
    if (Required <= Words->size()) {
      detachWords();
      return;
    }
    detachWords();
    Words->resize(Required, 0);
  }

  void clearUnusedBits() {
    if (Words->empty())
      return;
    const auto Used = U->Values.size() % bitsPerWord();
    if (Used != 0)
      Words->back() &= (std::uint64_t{1} << Used) - 1;
  }

  universe_ptr U;
  std::shared_ptr<std::vector<std::uint64_t>> Words;
};

template <typename T> class IndexedUnionDomain {
public:
  using value_type = IndexedSet<T>;

  IndexedUnionDomain() : U(value_type::makeUniverse()) {}
  explicit IndexedUnionDomain(typename value_type::universe_ptr U)
      : U(std::move(U)) {}

  value_type bottom() const { return value_type(U); }
  value_type join(const value_type &Lhs, const value_type &Rhs) const {
    value_type Out = Lhs;
    Out.unionWith(Rhs);
    return Out;
  }
  bool equal(const value_type &Lhs, const value_type &Rhs) const {
    return Lhs == Rhs;
  }

  typename value_type::universe_ptr universe() const { return U; }

private:
  typename value_type::universe_ptr U;
};

template <typename T> class IndexedIntersectionDomain {
public:
  using value_type = IndexedSet<T>;

  IndexedIntersectionDomain() : U(value_type::makeUniverse()) {}
  explicit IndexedIntersectionDomain(typename value_type::universe_ptr U)
      : U(std::move(U)) {}
  explicit IndexedIntersectionDomain(const value_type &Universe)
      : U(Universe.universe()) {}

  void setUniverse(const value_type &Universe) { U = Universe.universe(); }

  value_type bottom() const { return value_type::full(U); }
  value_type empty() const { return value_type(U); }
  value_type join(const value_type &Lhs, const value_type &Rhs) const {
    value_type Out = Lhs;
    Out.intersectWith(Rhs);
    return Out;
  }
  bool equal(const value_type &Lhs, const value_type &Rhs) const {
    return Lhs == Rhs;
  }

  typename value_type::universe_ptr universe() const { return U; }

private:
  typename value_type::universe_ptr U;
};

} // namespace elimination
