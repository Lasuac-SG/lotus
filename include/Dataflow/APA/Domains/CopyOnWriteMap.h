#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace elimination {

// Indexed, paged map for sparse value domains. A fact copy shares all pages;
// updating one value clones only the page that contains it.
template <typename KeyT, typename ValueT> class CopyOnWriteMap {
public:
  using key_type = KeyT;
  using mapped_type = ValueT;
  using value_type = std::pair<KeyT, ValueT>;

  struct Universe {
    std::unordered_map<key_type, std::size_t> Indices;
    std::vector<key_type> Keys;

    std::size_t intern(const key_type &Key) {
      auto It = Indices.find(Key);
      if (It != Indices.end())
        return It->second;
      const auto Index = Keys.size();
      Keys.push_back(Key);
      Indices.emplace(Key, Index);
      return Index;
    }

    std::size_t find(const key_type &Key) const {
      auto It = Indices.find(Key);
      return It == Indices.end() ? npos : It->second;
    }

    static constexpr std::size_t npos = static_cast<std::size_t>(-1);
  };

  using universe_ptr = std::shared_ptr<Universe>;

private:
  static constexpr std::size_t PAGE_SIZE = 64;

  struct Page {
    std::array<mapped_type, PAGE_SIZE> Values{};
    std::uint64_t Present = 0;
  };

  using page_ptr = std::shared_ptr<Page>;
  using page_vector = std::vector<page_ptr>;

public:
  class const_iterator {
  public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = typename CopyOnWriteMap::value_type;
    using difference_type = std::ptrdiff_t;
    using pointer = const value_type *;
    using reference = value_type;

    const_iterator() = default;
    value_type operator*() const {
      return {Owner->U->Keys[Index], Owner->valueAt(Index)};
    }
    pointer operator->() const {
      Cache.emplace(Owner->U->Keys[Index], Owner->valueAt(Index));
      return &*Cache;
    }
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
    friend class CopyOnWriteMap;
    const_iterator(const CopyOnWriteMap *Owner, std::size_t Index)
        : Owner(Owner), Index(Index) {
      advance();
    }
    void advance() {
      if (!Owner)
        return;
      while (Index < Owner->U->Keys.size() && !Owner->presentAt(Index))
        ++Index;
    }

    const CopyOnWriteMap *Owner = nullptr;
    std::size_t Index = 0;
    mutable std::optional<value_type> Cache;
  };

  using iterator = const_iterator;

  class value_proxy {
  public:
    value_proxy(CopyOnWriteMap &Owner, key_type Key)
        : Owner(Owner), Key(std::move(Key)) {}
    value_proxy &operator=(const mapped_type &Value) {
      Owner.set(Key, Value);
      return *this;
    }
    value_proxy &operator=(mapped_type &&Value) {
      Owner.set(Key, std::move(Value));
      return *this;
    }
    operator mapped_type() const {
      auto *Value = Owner.lookup(Key);
      return Value != nullptr ? *Value : mapped_type{};
    }

  private:
    CopyOnWriteMap &Owner;
    key_type Key;
  };

  CopyOnWriteMap() : CopyOnWriteMap(std::make_shared<Universe>()) {}
  explicit CopyOnWriteMap(universe_ptr U)
      : U(U ? std::move(U) : std::make_shared<Universe>()),
        Pages(std::make_shared<page_vector>()) {}
  CopyOnWriteMap(std::initializer_list<value_type> Init) : CopyOnWriteMap() {
    for (const auto &Entry : Init)
      set(Entry.first, Entry.second);
  }

  static universe_ptr makeUniverse() { return std::make_shared<Universe>(); }
  universe_ptr universe() const { return U; }

  bool empty() const { return size() == 0; }
  std::size_t size() const {
    std::size_t Count = 0;
    for (const auto &P : *Pages)
      if (P)
        Count += static_cast<std::size_t>(__builtin_popcountll(P->Present));
    return Count;
  }
  std::size_t count(const key_type &Key) const {
    const auto Index = U->find(Key);
    return Index != Universe::npos && presentAt(Index) ? 1u : 0u;
  }

  const_iterator begin() const { return const_iterator(this, 0); }
  const_iterator end() const { return const_iterator(this, U->Keys.size()); }
  const_iterator begin() { return const_iterator(this, 0); }
  const_iterator end() { return const_iterator(this, U->Keys.size()); }

  const_iterator find(const key_type &Key) const {
    const auto Index = U->find(Key);
    return Index != Universe::npos && presentAt(Index)
               ? const_iterator(this, Index)
               : end();
  }
  const_iterator find(const key_type &Key) {
    return static_cast<const CopyOnWriteMap &>(*this).find(Key);
  }

  const mapped_type *lookup(const key_type &Key) const {
    const auto Index = U->find(Key);
    return Index != Universe::npos && presentAt(Index) ? &valueAt(Index)
                                                       : nullptr;
  }

  value_proxy operator[](const key_type &Key) {
    return value_proxy(*this, Key);
  }
  const mapped_type &at(const key_type &Key) const {
    auto *Value = lookup(Key);
    assert(Value && "key is not present");
    return *Value;
  }

  void set(const key_type &Key, const mapped_type &Value) {
    setImpl(Key, Value);
  }
  void set(const key_type &Key, mapped_type &&Value) {
    setImpl(Key, std::move(Value));
  }

  std::pair<const_iterator, bool> insert(const value_type &Value) {
    const bool Inserted = count(Value.first) == 0;
    set(Value.first, Value.second);
    return {find(Value.first), Inserted};
  }
  template <typename It> void insert(It Begin, It End) {
    for (; Begin != End; ++Begin)
      set(Begin->first, Begin->second);
  }

  std::size_t erase(const key_type &Key) {
    const auto Index = U->find(Key);
    if (Index == Universe::npos || !presentAt(Index))
      return 0;
    auto Page = mutablePage(Index / PAGE_SIZE);
    Page->Present &= ~(std::uint64_t{1} << (Index % PAGE_SIZE));
    return 1;
  }

  void clear() { Pages = std::make_shared<page_vector>(); }

private:
  bool presentAt(std::size_t Index) const {
    const auto PageIndex = Index / PAGE_SIZE;
    return PageIndex < Pages->size() && (*Pages)[PageIndex] &&
           (((*Pages)[PageIndex]->Present >> (Index % PAGE_SIZE)) & 1u) != 0;
  }
  const mapped_type &valueAt(std::size_t Index) const {
    return (*Pages)[Index / PAGE_SIZE]->Values[Index % PAGE_SIZE];
  }

  page_ptr mutablePage(std::size_t PageIndex) {
    if (Pages.use_count() != 1)
      Pages = std::make_shared<page_vector>(*Pages);
    if (Pages->size() <= PageIndex)
      Pages->resize(PageIndex + 1);
    auto &P = (*Pages)[PageIndex];
    if (!P)
      P = std::make_shared<Page>();
    else if (P.use_count() != 1)
      P = std::make_shared<Page>(*P);
    return P;
  }

  template <typename V> void setImpl(const key_type &Key, V &&Value) {
    const auto Index = U->intern(Key);
    auto Page = mutablePage(Index / PAGE_SIZE);
    Page->Values[Index % PAGE_SIZE] = std::forward<V>(Value);
    Page->Present |= std::uint64_t{1} << (Index % PAGE_SIZE);
  }

  universe_ptr U;
  std::shared_ptr<page_vector> Pages;
};

} // namespace elimination
