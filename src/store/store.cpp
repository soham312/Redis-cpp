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

void Store::Set(const std::string& key, const std::string& value) {
  std::unique_lock lock(mu_);
  Value v;
  v.type = ValueType::kString;
  v.str_value = value;
  data_.Set(key, std::move(v));
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
    if (IsExpired(*v)) {
      data_.Delete(key);  // sweep the stale tombstone, but it doesn't count as a logical removal.
      continue;
    }
    data_.Delete(key);
    ++removed;
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
}

bool Store::Expire(const std::string& key, long long seconds) {
  std::unique_lock lock(mu_);

  Value* v = data_.Get(key);
  if (v == nullptr) {
    return false;
  }
  if (IsExpired(*v)) {
    data_.Delete(key);
    return false;
  }
  if (seconds <= 0) {
    // Matches Redis's own EXPIRE: a non-positive TTL deletes the key
    // immediately rather than setting an expiry time in the past.
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
    data_.Delete(key);
    v = nullptr;
  }
  if (v == nullptr) {
    Value fresh;
    fresh.type = ValueType::kList;
    data_.Set(key, std::move(fresh));
    return data_.Get(key);  // re-fetch: the Value moved into the table is
                             // what's now owned by the Node, not `fresh`.
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

  // LPUSH pushes each argument in order, so the *last* argument ends up
  // at the head — matching Redis's own LPUSH semantics.
  for (const auto& value : values) {
    list->list_value.insert(list->list_value.begin(), value);
  }
  return Result<int>{static_cast<int>(list->list_value.size()), StoreError::kNone};
}

Result<int> Store::RPush(const std::string& key, const std::vector<std::string>& values) {
  std::unique_lock lock(mu_);

  StoreError error = StoreError::kNone;
  Value* list = GetOrCreateList(key, &error);
  if (list == nullptr) {
    return Result<int>{std::nullopt, error};
  }

  list->list_value.insert(list->list_value.end(), values.begin(), values.end());
  return Result<int>{static_cast<int>(list->list_value.size()), StoreError::kNone};
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

}  // namespace goredis
