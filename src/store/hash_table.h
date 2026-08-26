// A hand-rolled hash table. std::unordered_map (and any other hash-map
// library) is intentionally not used here: the point of this class is to
// demonstrate, and be able to explain in an interview, exactly how a hash
// table works under the hood — hashing, collision resolution, and
// resizing, plus (in C++ specifically) how to manage that structure's
// memory correctly without a garbage collector.
//
// Templated only over the value type (V) — the key is always
// std::string, matching this project's original Go implementation
// (goredis's HashTable[V any] was likewise generic over the value but not
// the key). Keeping the key type fixed avoids needing a separate Hash<K>
// trait/functor for a case this project never actually needs.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace goredis {

template <typename V>
class HashTable {
 public:
  HashTable() : buckets_(kInitialCapacity) {}

  // Non-copyable: a HashTable owns a chain of heap-allocated Nodes via
  // std::unique_ptr, and unique_ptr itself can't be copied — copying this
  // class correctly would mean deep-cloning every node, which nothing in
  // this project needs, so it's simplest and safest to just delete it
  // rather than write (and have to maintain) a deep-copy constructor
  // nobody calls. Moving is fine and falls out for free: moving a
  // std::vector of unique_ptrs is just moving pointers.
  HashTable(const HashTable&) = delete;
  HashTable& operator=(const HashTable&) = delete;
  HashTable(HashTable&&) = default;
  HashTable& operator=(HashTable&&) = default;

  // Set inserts or updates the value for key. Returns true if this
  // updated an existing key (false if it was a new insertion) — this
  // mirrors what Store needs to know without a second lookup (e.g.
  // whether to adjust an approximate byte-usage counter by a delta or by
  // the full new size).
  bool Set(const std::string& key, V value) {
    std::size_t index = BucketIndex(key);

    // Walk the chain first: if the key already exists, update in place
    // rather than appending a duplicate node.
    for (Node* cur = buckets_[index].get(); cur != nullptr;
         cur = cur->next.get()) {
      if (cur->key == key) {
        cur->value = std::move(value);
        return true;
      }
    }

    // New key: prepend to the chain. Prepending (rather than appending)
    // is O(1) because there's no need to walk to the end — order within
    // a bucket has no semantic meaning, so insertion position doesn't
    // matter. The new node takes ownership of whatever the bucket used to
    // own (its unique_ptr is moved into new_node->next), and the bucket's
    // slot then takes ownership of the new node — a clean, leak-free
    // transfer of ownership with no raw new/delete anywhere.
    auto new_node = std::make_unique<Node>(key, std::move(value));
    new_node->next = std::move(buckets_[index]);
    buckets_[index] = std::move(new_node);
    ++num_entries_;

    // Resize check happens *after* insertion so the new entry is already
    // counted in num_entries_ when deciding the load factor.
    if (LoadFactor() > kMaxLoadFactor) {
      Resize();
    }
    return false;
  }

  // Get returns a pointer to the stored value, or nullptr if key isn't
  // present. The pointer is non-owning and aliases memory this table
  // owns — it stays valid only as long as the table isn't mutated (and,
  // at the Store layer above, only for as long as the caller holds the
  // lock guarding the table). This mirrors the original Go
  // implementation's Get, which handed back a live pointer into
  // table-owned memory for the same reason: Store always copies out
  // whatever it needs from the pointee before releasing its lock, rather
  // than letting this pointer escape to a caller outside that lock.
  V* Get(const std::string& key) {
    Node* cur = buckets_[BucketIndex(key)].get();
    while (cur != nullptr) {
      if (cur->key == key) {
        return &cur->value;
      }
      cur = cur->next.get();
    }
    return nullptr;
  }

  const V* Get(const std::string& key) const {
    const Node* cur = buckets_[BucketIndex(key)].get();
    while (cur != nullptr) {
      if (cur->key == key) {
        return &cur->value;
      }
      cur = cur->next.get();
    }
    return nullptr;
  }

  // Delete removes key if present and reports whether anything was
  // removed.
  //
  // The node is freed with no explicit `delete` anywhere: prev_link
  // tracks a pointer to whichever unique_ptr currently *owns* the node
  // being examined (either the bucket's head slot, or the previous
  // node's `next` member). Move-assigning `*prev_link = std::move(node's
  // next)` does two things atomically — it makes the owning slot now own
  // the rest of the chain (unlinking `node`), and it destroys whatever
  // `*prev_link` used to own (which was `node` itself, now that nothing
  // else references it), running Node's destructor and freeing its
  // memory. One move-assignment, correctly unlinks AND frees, no manual
  // delete, no leak, no dangling pointer.
  bool Delete(const std::string& key) {
    std::size_t index = BucketIndex(key);
    std::unique_ptr<Node>* prev_link = &buckets_[index];

    while (*prev_link != nullptr) {
      Node* cur = prev_link->get();
      if (cur->key == key) {
        *prev_link = std::move(cur->next);
        --num_entries_;
        return true;
      }
      prev_link = &cur->next;
    }
    return false;
  }

  std::size_t Len() const { return num_entries_; }

  // Keys returns a snapshot of every key currently in the table. Order is
  // unspecified — it depends on bucket layout, same as iterating an
  // unordered_map would give no ordering guarantee either.
  std::vector<std::string> Keys() const {
    std::vector<std::string> keys;
    keys.reserve(num_entries_);
    for (const auto& head : buckets_) {
      for (const Node* cur = head.get(); cur != nullptr;
           cur = cur->next.get()) {
        keys.push_back(cur->key);
      }
    }
    return keys;
  }

  // ForEach calls fn(key, value) for every entry, without allocating an
  // intermediate vector the way Keys() does. fn must not mutate the
  // table — inserting or deleting while ForEach is walking a bucket chain
  // could invalidate the very node being visited. Callers that need to
  // delete based on what they see during a ForEach (e.g. an expiry sweep)
  // should collect keys during the callback and delete them afterward.
  template <typename Fn>
  void ForEach(Fn&& fn) const {
    for (const auto& head : buckets_) {
      for (const Node* cur = head.get(); cur != nullptr;
           cur = cur->next.get()) {
        fn(cur->key, cur->value);
      }
    }
  }

  // Clear resets the table to a fresh, empty state. Reallocating the
  // bucket vector at kInitialCapacity (rather than walking every bucket
  // and resetting each unique_ptr to nullptr in place) means a table that
  // grew to hold millions of entries doesn't stay that large just to
  // become empty — and assigning a freshly-constructed vector over
  // buckets_ destroys every existing Node chain automatically via each
  // outgoing unique_ptr's destructor, again with no manual cleanup code.
  void Clear() {
    buckets_ = std::vector<std::unique_ptr<Node>>(kInitialCapacity);
    num_entries_ = 0;
  }

 private:
  struct Node {
    std::string key;
    V value;
    std::unique_ptr<Node> next;

    Node(std::string k, V v) : key(std::move(k)), value(std::move(v)) {}
  };

  static constexpr std::size_t kInitialCapacity = 16;  // must stay a power of two — see BucketIndex.
  static constexpr double kMaxLoadFactor = 0.75;         // grow when entries/capacity exceeds this.
  static constexpr std::size_t kGrowthMultiplier = 2;

  // HashKey computes a 64-bit FNV-1a hash of key, implemented by hand
  // (rather than std::hash<std::string>, whose exact algorithm isn't
  // specified by the standard and varies by implementation) so the full
  // algorithm is visible and explainable: start from an offset basis, and
  // for every byte, XOR it into the running hash then multiply by a
  // prime. The "a" variant (XOR before multiply) gives slightly better
  // avalanche behavior for short keys, which matters because most command
  // keys here are short strings.
  //
  // FNV-1a is not cryptographically secure — a client that could choose
  // keys could craft collisions, a hash-flooding DoS vector — but that's
  // an accepted, explicitly-named tradeoff for an educational/portfolio
  // store; production Redis uses SipHash for exactly this reason.
  static std::uint64_t HashKey(const std::string& key) {
    constexpr std::uint64_t kOffsetBasis = 14695981039346656037ULL;
    constexpr std::uint64_t kPrime = 1099511628211ULL;

    std::uint64_t hash = kOffsetBasis;
    for (unsigned char c : key) {
      hash ^= static_cast<std::uint64_t>(c);
      hash *= kPrime;
    }
    return hash;
  }

  // BucketIndex maps a key to a bucket index. Capacity is always kept a
  // power of two specifically so this can be `hash & (capacity - 1)`
  // instead of `hash % capacity`: a bitwise AND is a single fast
  // instruction, while integer modulo is comparatively expensive — a
  // classic hash table micro-optimization that's easy to get subtly wrong
  // if capacity isn't a power of two (the mask trick silently produces
  // incorrect bucket indices otherwise).
  std::size_t BucketIndex(const std::string& key) const {
    return static_cast<std::size_t>(HashKey(key)) & (buckets_.size() - 1);
  }

  double LoadFactor() const {
    return static_cast<double>(num_entries_) / static_cast<double>(buckets_.size());
  }

  // Resize doubles bucket capacity and re-links every existing node into
  // the new bucket vector.
  //
  // This is the expensive part of the "amortized O(1) insert" story: any
  // single Set() can trigger an O(n) rehash, but because capacity doubles
  // each time, the total cost of all resizes across n insertions is O(n),
  // so the *average* cost per insertion stays O(1) — the same argument
  // that justifies amortized-O(1) push_back on a std::vector.
  //
  // Nodes are moved, not reallocated: ownership of each already-allocated
  // Node is transferred from the old bucket vector into the new one
  // (relinking unique_ptrs), so resizing costs relinking pointers, not
  // copying or reconstructing every stored value.
  void Resize() {
    std::size_t new_capacity = buckets_.size() * kGrowthMultiplier;
    std::vector<std::unique_ptr<Node>> new_buckets(new_capacity);

    for (auto& head : buckets_) {
      std::unique_ptr<Node> cur = std::move(head);
      while (cur != nullptr) {
        std::unique_ptr<Node> next = std::move(cur->next);  // save before cur is moved out below
        std::size_t index = static_cast<std::size_t>(HashKey(cur->key)) & (new_capacity - 1);
        cur->next = std::move(new_buckets[index]);
        new_buckets[index] = std::move(cur);
        cur = std::move(next);
      }
    }

    buckets_ = std::move(new_buckets);
  }

  std::vector<std::unique_ptr<Node>> buckets_;
  std::size_t num_entries_ = 0;
};

}  // namespace goredis
