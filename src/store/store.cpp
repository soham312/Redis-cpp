#include "store.h"

#include <algorithm>
#include <mutex>  // std::unique_lock — std::shared_lock comes from <shared_mutex>, pulled in via store.h

namespace goredis {

namespace {

// NormalizeIndex converts a possibly-negative Redis-style index (-1 = last
// element) into a plain, possibly-still-out-of-range index into a
// sequence of length n. Clamping to the valid range is the caller's job
// (see LRange) — this only handles the sign.
int NormalizeIndex(int index, int n) {
  if (index < 0) {
    index = n + index;
  }
  return index;
}

}  // namespace

Store::Store(std::chrono::milliseconds sweep_interval) : sweep_interval_(sweep_interval) {
  sweeper_thread_ = std::thread(&Store::SweeperRun, this);
}

Store::~Store() {
  {
    std::lock_guard<std::mutex> lock(sweeper_mu_);
    stop_sweeper_ = true;
  }
  sweeper_cv_.notify_one();
  sweeper_thread_.join();
}

bool Store::IsExpired(const Value& v) {
  return v.expires_at.has_value() && Clock::now() >= *v.expires_at;
}

bool Store::DeleteIfExpired(const std::string& key) const {
  std::unique_lock lock(mu_);
  const Value* v = data_.Get(key);
  if (v != nullptr && IsExpired(*v)) {
    used_memory_bytes_ -= EstimateEntryBytes(key, *v);
    data_.Delete(key);
    return true;
  }
  return false;
}

void Store::SweepExpired() {
  // Phase 1: find candidates under a shared lock, so the (potentially
  // long, O(n)) scan doesn't block other readers or writers.
  std::vector<std::string> expired;
  {
    std::shared_lock lock(mu_);
    data_.ForEach([&](const std::string& key, const Value& v) {
      if (IsExpired(v)) {
        expired.push_back(key);
      }
    });
  }
  if (expired.empty()) {
    return;
  }

  // Phase 2: delete the candidates under a write lock, re-checking each
  // one first — a key could have been refreshed (EXPIRE'd further out),
  // overwritten by a new SET, or already deleted by a concurrent reader's
  // own lazy-expiry path between phase 1 and now.
  std::unique_lock lock(mu_);
  for (const auto& key : expired) {
    const Value* v = data_.Get(key);
    if (v != nullptr && IsExpired(*v)) {
      used_memory_bytes_ -= EstimateEntryBytes(key, *v);
      data_.Delete(key);
    }
  }
}

void Store::SweeperRun() {
  std::unique_lock<std::mutex> lock(sweeper_mu_);
  while (!stop_sweeper_) {
    // wait_for returns early the moment the destructor sets stop_sweeper_
    // and notifies, rather than always sleeping the full interval — so
    // destroying a Store doesn't have to wait out an in-progress sleep.
    sweeper_cv_.wait_for(lock, sweep_interval_, [this] { return stop_sweeper_; });
    if (stop_sweeper_) {
      break;
    }
    lock.unlock();
    SweepExpired();
    lock.lock();
  }
}

std::size_t Store::EstimateEntryBytes(const std::string& key, const Value& v) {
  std::size_t bytes = key.size() + kPerEntryOverheadBytes;
  switch (v.type) {
    case ValueType::kString:
      bytes += v.str_value.size();
      break;
    case ValueType::kList:
      for (const auto& elem : v.list_value) {
        bytes += elem.size() + kPerListElementOverheadBytes;
      }
      break;
  }
  return bytes;
}

void Store::Touch(const Value& v) const {
  v.last_accessed_seq.store(access_clock_.fetch_add(1, std::memory_order_relaxed), std::memory_order_relaxed);
}

void Store::EvictIfOverBudget() {
  if (max_memory_bytes_ == 0) {
    return;
  }
  while (used_memory_bytes_ > max_memory_bytes_ && data_.Len() > 0) {
    auto sample = data_.SampleKeys(kSampleSize);
    if (sample.empty()) {
      break;  // shouldn't happen while Len() > 0, but avoids spinning forever if it somehow does.
    }

    // "Approximate" LRU: rather than maintaining an exact recency-ordered
    // structure (which would need every read to take the exclusive lock
    // to update it safely), just evict whichever of a small random
    // sample has the oldest last_accessed_ms — the same trade-off real
    // Redis's own maxmemory-samples-based eviction makes.
    std::size_t victim_index = 0;
    std::uint64_t oldest = sample[0].second->last_accessed_seq.load(std::memory_order_relaxed);
    for (std::size_t i = 1; i < sample.size(); ++i) {
      std::uint64_t accessed = sample[i].second->last_accessed_seq.load(std::memory_order_relaxed);
      if (accessed < oldest) {
        oldest = accessed;
        victim_index = i;
      }
    }

    const std::string& victim_key = sample[victim_index].first;
    used_memory_bytes_ -= EstimateEntryBytes(victim_key, *sample[victim_index].second);
    data_.Delete(victim_key);
  }
}

void Store::SetMaxMemory(std::size_t max_memory_bytes) {
  std::unique_lock lock(mu_);
  max_memory_bytes_ = max_memory_bytes;
  EvictIfOverBudget();
}

std::size_t Store::UsedMemory() const {
  std::shared_lock lock(mu_);
  return used_memory_bytes_;
}

void Store::Set(const std::string& key, const std::string& value) {
  std::unique_lock lock(mu_);

  const Value* existing = data_.Get(key);
  std::size_t before = existing != nullptr ? EstimateEntryBytes(key, *existing) : 0;

  Value v;
  v.type = ValueType::kString;
  v.str_value = value;
  data_.Set(key, std::move(v));

  const Value* stored = data_.Get(key);
  used_memory_bytes_ -= before;
  used_memory_bytes_ += EstimateEntryBytes(key, *stored);
  Touch(*stored);  // a SET is itself an access — and Value's own
                    // last_accessed_seq default (0) would otherwise leave
                    // a never-since-read key looking like the oldest
                    // thing in the table.
  EvictIfOverBudget();
}

Result<std::string> Store::Get(const std::string& key) const {
  {
    std::shared_lock lock(mu_);
    const Value* v = data_.Get(key);
    if (v == nullptr) {
      return Result<std::string>{};
    }
    if (!IsExpired(*v)) {
      if (v->type != ValueType::kString) {
        return Result<std::string>{std::nullopt, StoreError::kWrongType};
      }
      Touch(*v);
      return Result<std::string>{v->str_value, StoreError::kNone};
    }
  }
  // v was found but had already expired: escalate to remove the stale
  // entry, then report the key as not found, same as if it were already
  // gone.
  DeleteIfExpired(key);
  return Result<std::string>{};
}

int Store::Del(const std::vector<std::string>& keys) {
  std::unique_lock lock(mu_);

  // Already holding the write lock for the whole batch, so no need for
  // the shared-lock/escalate dance used by the const read methods below
  // — just check expiry inline before counting/deleting each key.
  int removed = 0;
  for (const auto& key : keys) {
    Value* v = data_.Get(key);
    if (v == nullptr) {
      continue;
    }
    used_memory_bytes_ -= EstimateEntryBytes(key, *v);
    bool was_expired = IsExpired(*v);
    data_.Delete(key);
    if (!was_expired) {
      ++removed;
    }
  }
  return removed;
}

int Store::Exists(const std::vector<std::string>& keys) const {
  std::vector<std::string> expired_keys;
  int count = 0;
  {
    std::shared_lock lock(mu_);
    for (const auto& key : keys) {
      const Value* v = data_.Get(key);
      if (v == nullptr) {
        continue;
      }
      if (IsExpired(*v)) {
        expired_keys.push_back(key);
        continue;
      }
      ++count;
    }
  }
  for (const auto& key : expired_keys) {
    DeleteIfExpired(key);
  }
  return count;
}

std::vector<std::string> Store::Keys() const {
  std::vector<std::string> live_keys;
  std::vector<std::string> expired_keys;
  {
    std::shared_lock lock(mu_);
    data_.ForEach([&](const std::string& key, const Value& v) {
      if (IsExpired(v)) {
        expired_keys.push_back(key);
      } else {
        live_keys.push_back(key);
      }
    });
  }
  for (const auto& key : expired_keys) {
    DeleteIfExpired(key);
  }
  return live_keys;
}

void Store::FlushAll() {
  std::unique_lock lock(mu_);
  data_.Clear();
  used_memory_bytes_ = 0;
}

bool Store::Expire(const std::string& key, long long seconds) {
  std::unique_lock lock(mu_);

  Value* v = data_.Get(key);
  if (v == nullptr) {
    return false;
  }
  if (IsExpired(*v)) {
    used_memory_bytes_ -= EstimateEntryBytes(key, *v);
    data_.Delete(key);
    return false;
  }
  if (seconds <= 0) {
    // Matches Redis's own EXPIRE: a non-positive TTL deletes the key
    // immediately rather than setting an expiry time in the past.
    used_memory_bytes_ -= EstimateEntryBytes(key, *v);
    data_.Delete(key);
    return true;
  }
  v->expires_at = Clock::now() + std::chrono::seconds(seconds);
  return true;
}

long long Store::TTL(const std::string& key) const {
  {
    std::shared_lock lock(mu_);
    const Value* v = data_.Get(key);
    if (v == nullptr) {
      return -2;
    }
    if (!IsExpired(*v)) {
      if (!v->expires_at.has_value()) {
        return -1;
      }
      auto remaining_ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(*v->expires_at - Clock::now()).count();
      // Round to the nearest second (rather than truncating) so a key
      // with, say, 1999ms left reports 2 — matching Redis's own TTL
      // rounding rather than needlessly undercounting by a second.
      return (remaining_ms + 500) / 1000;
    }
  }
  DeleteIfExpired(key);
  return -2;
}

Value* Store::GetOrCreateList(const std::string& key, StoreError* error) {
  *error = StoreError::kNone;

  Value* v = data_.Get(key);
  if (v != nullptr && IsExpired(*v)) {
    // An expired list is logically gone — drop the stale entry rather
    // than letting a fresh push resurrect old elements alongside it.
    used_memory_bytes_ -= EstimateEntryBytes(key, *v);
    data_.Delete(key);
    v = nullptr;
  }
  if (v == nullptr) {
    Value fresh;
    fresh.type = ValueType::kList;
    data_.Set(key, std::move(fresh));
    v = data_.Get(key);  // re-fetch: the Value moved into the table is
                          // what's now owned by the Node, not `fresh`.
    used_memory_bytes_ += EstimateEntryBytes(key, *v);  // baseline (empty list) size.
    Touch(*v);
    return v;
  }
  if (v->type != ValueType::kList) {
    *error = StoreError::kWrongType;
    return nullptr;
  }
  return v;
}

Result<int> Store::LPush(const std::string& key, const std::vector<std::string>& values) {
  std::unique_lock lock(mu_);

  StoreError error = StoreError::kNone;
  Value* list = GetOrCreateList(key, &error);
  if (list == nullptr) {
    return Result<int>{std::nullopt, error};
  }

  std::size_t before = EstimateEntryBytes(key, *list);
  // LPUSH pushes each argument in order, so the *last* argument ends up
  // at the head — matching Redis's own LPUSH semantics.
  for (const auto& value : values) {
    list->list_value.insert(list->list_value.begin(), value);
  }
  used_memory_bytes_ -= before;
  used_memory_bytes_ += EstimateEntryBytes(key, *list);
  Touch(*list);

  // Compute the reply *before* running eviction, not after: key is now
  // the freshest entry in the table, but if it's large enough that
  // nothing else can be freed to get under budget, key itself can end up
  // as the eviction victim (an accepted, Redis-like edge case for a
  // single oversized value) — EvictIfOverBudget would then free the very
  // Node `list` points into, making any further dereference of `list` a
  // use-after-free.
  int result = static_cast<int>(list->list_value.size());
  EvictIfOverBudget();
  return Result<int>{result, StoreError::kNone};
}

Result<int> Store::RPush(const std::string& key, const std::vector<std::string>& values) {
  std::unique_lock lock(mu_);

  StoreError error = StoreError::kNone;
  Value* list = GetOrCreateList(key, &error);
  if (list == nullptr) {
    return Result<int>{std::nullopt, error};
  }

  std::size_t before = EstimateEntryBytes(key, *list);
  list->list_value.insert(list->list_value.end(), values.begin(), values.end());
  used_memory_bytes_ -= before;
  used_memory_bytes_ += EstimateEntryBytes(key, *list);
  Touch(*list);

  // See the identical comment in LPush: compute the reply before running
  // eviction, since EvictIfOverBudget can free the very entry `list`
  // points into.
  int result = static_cast<int>(list->list_value.size());
  EvictIfOverBudget();
  return Result<int>{result, StoreError::kNone};
}

Result<int> Store::LLen(const std::string& key) const {
  {
    std::shared_lock lock(mu_);
    const Value* v = data_.Get(key);
    if (v == nullptr) {
      return Result<int>{0, StoreError::kNone};
    }
    if (!IsExpired(*v)) {
      if (v->type != ValueType::kList) {
        return Result<int>{std::nullopt, StoreError::kWrongType};
      }
      Touch(*v);
      return Result<int>{static_cast<int>(v->list_value.size()), StoreError::kNone};
    }
  }
  DeleteIfExpired(key);
  return Result<int>{0, StoreError::kNone};
}

Result<std::vector<std::string>> Store::LRange(const std::string& key, int start, int stop) const {
  bool found_expired = false;
  {
    std::shared_lock lock(mu_);
    const Value* v = data_.Get(key);
    if (v == nullptr) {
      return Result<std::vector<std::string>>{std::vector<std::string>{}, StoreError::kNone};
    }
    if (IsExpired(*v)) {
      found_expired = true;
    } else if (v->type != ValueType::kList) {
      return Result<std::vector<std::string>>{std::nullopt, StoreError::kWrongType};
    } else {
      Touch(*v);
      int n = static_cast<int>(v->list_value.size());
      int norm_start = std::max(NormalizeIndex(start, n), 0);
      int norm_stop = std::min(NormalizeIndex(stop, n), n - 1);

      if (norm_start > norm_stop || n == 0) {
        return Result<std::vector<std::string>>{std::vector<std::string>{}, StoreError::kNone};
      }

      // Copy the requested slice rather than exposing the underlying
      // vector directly: callers get their own data, so they can't
      // observe (or race with) further mutation of list state stored in
      // the table after this lock is released.
      std::vector<std::string> out(v->list_value.begin() + norm_start, v->list_value.begin() + norm_stop + 1);
      return Result<std::vector<std::string>>{std::move(out), StoreError::kNone};
    }
  }
  if (found_expired) {
    DeleteIfExpired(key);
  }
  return Result<std::vector<std::string>>{std::vector<std::string>{}, StoreError::kNone};
}

std::vector<SnapshotEntry> Store::Snapshot() const {
  std::vector<SnapshotEntry> out;
  std::shared_lock lock(mu_);
  out.reserve(data_.Len());

  auto steady_now = Clock::now();
  auto wall_now = std::chrono::system_clock::now();

  data_.ForEach([&](const std::string& key, const Value& v) {
    if (IsExpired(v)) {
      return;  // logically absent, same as every other read path — don't persist a tombstone.
    }
    SnapshotEntry entry;
    entry.key = key;
    entry.type = v.type;
    entry.str_value = v.str_value;
    entry.list_value = v.list_value;
    if (v.expires_at.has_value()) {
      // Anchor the remaining steady_clock duration onto wall_now to get a
      // timestamp that still means something after a restart — see the
      // comment on SnapshotEntry::expires_at_wall.
      auto remaining = *v.expires_at - steady_now;
      entry.expires_at_wall = wall_now + std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
    }
    out.push_back(std::move(entry));
  });

  return out;
}

void Store::LoadSnapshot(std::vector<SnapshotEntry> entries) {
  std::unique_lock lock(mu_);

  data_.Clear();
  used_memory_bytes_ = 0;

  auto steady_now = Clock::now();
  auto wall_now = std::chrono::system_clock::now();

  for (auto& entry : entries) {
    if (entry.expires_at_wall.has_value() && *entry.expires_at_wall <= wall_now) {
      continue;  // expired during whatever gap elapsed since it was captured — don't resurrect it.
    }

    Value v;
    v.type = entry.type;
    v.str_value = std::move(entry.str_value);
    v.list_value = std::move(entry.list_value);
    if (entry.expires_at_wall.has_value()) {
      v.expires_at = steady_now + std::chrono::duration_cast<Clock::duration>(*entry.expires_at_wall - wall_now);
    }

    data_.Set(entry.key, std::move(v));
    const Value* stored = data_.Get(entry.key);
    used_memory_bytes_ += EstimateEntryBytes(entry.key, *stored);
    Touch(*stored);
  }

  EvictIfOverBudget();  // in case max_memory_bytes_ is already set below what was just loaded.
}

}  // namespace goredis
