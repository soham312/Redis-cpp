#include "persistence/rdb.h"

#include <chrono>
#include <cstdint>
#include <cstdio>   // std::rename, std::remove
#include <cstring>  // std::memcmp
#include <fstream>
#include <iterator>

namespace goredis {

namespace {

constexpr char kMagic[4] = {'G', 'R', 'D', 'B'};
constexpr std::uint32_t kFormatVersion = 1;
constexpr std::uint8_t kTypeString = 0;
constexpr std::uint8_t kTypeList = 1;

// ByteWriter builds a serialized image in memory, one primitive at a
// time. All multi-byte integers are written explicitly little-endian
// (via shifts, byte by byte) rather than a raw struct/memcpy of the
// host's native representation — this file format needs to be readable
// back correctly regardless of which architecture wrote it, and a raw
// memcpy would silently bake in the writing machine's endianness (every
// mainstream target today is little-endian in practice, but nothing
// here should quietly depend on that).
class ByteWriter {
 public:
  void WriteBytes(const char* data, std::size_t len) { buf_.append(data, len); }
  void WriteU8(std::uint8_t v) { buf_.push_back(static_cast<char>(v)); }

  void WriteU32(std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
      buf_.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
    }
  }

  void WriteU64(std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
      buf_.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
    }
  }

  void WriteI64(std::int64_t v) { WriteU64(static_cast<std::uint64_t>(v)); }

  void WriteString(const std::string& s) {
    WriteU32(static_cast<std::uint32_t>(s.size()));
    buf_.append(s);
  }

  const std::string& bytes() const { return buf_; }

 private:
  std::string buf_;
};

// ByteReader is ByteWriter's mirror image: reads primitives out of a
// buffer it doesn't own, advancing an internal cursor. Every Read method
// returns false (leaving *out untouched) on truncation instead of
// throwing or reading out of bounds — LoadRdb treats any parse failure
// the same way, as "this file is unusable," never as a crash.
class ByteReader {
 public:
  explicit ByteReader(const std::string& buf) : buf_(buf) {}

  bool ReadBytes(char* out, std::size_t len) {
    if (pos_ + len > buf_.size()) {
      return false;
    }
    std::memcpy(out, buf_.data() + pos_, len);
    pos_ += len;
    return true;
  }

  bool ReadU8(std::uint8_t* out) {
    if (pos_ + 1 > buf_.size()) {
      return false;
    }
    *out = static_cast<std::uint8_t>(buf_[pos_]);
    pos_ += 1;
    return true;
  }

  bool ReadU32(std::uint32_t* out) {
    if (pos_ + 4 > buf_.size()) {
      return false;
    }
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
      v |= static_cast<std::uint32_t>(static_cast<unsigned char>(buf_[pos_ + i])) << (8 * i);
    }
    pos_ += 4;
    *out = v;
    return true;
  }

  bool ReadU64(std::uint64_t* out) {
    if (pos_ + 8 > buf_.size()) {
      return false;
    }
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
      v |= static_cast<std::uint64_t>(static_cast<unsigned char>(buf_[pos_ + i])) << (8 * i);
    }
    pos_ += 8;
    *out = v;
    return true;
  }

  bool ReadI64(std::int64_t* out) {
    std::uint64_t v;
    if (!ReadU64(&v)) {
      return false;
    }
    *out = static_cast<std::int64_t>(v);
    return true;
  }

  bool ReadString(std::string* out) {
    std::uint32_t len;
    if (!ReadU32(&len)) {
      return false;
    }
    if (pos_ + len > buf_.size()) {
      return false;
    }
    *out = buf_.substr(pos_, len);
    pos_ += len;
    return true;
  }

 private:
  const std::string& buf_;
  std::size_t pos_ = 0;
};

// Fnv1a64 detects corruption/truncation, not tampering (it's not
// cryptographic) — the same algorithm, and the same reasoning for
// choosing it, as HashTable::HashKey: explicit and explainable rather
// than opaque, which is what a portfolio project's own file format
// should be.
std::uint64_t Fnv1a64(const std::string& data) {
  constexpr std::uint64_t kOffsetBasis = 14695981039346656037ULL;
  constexpr std::uint64_t kPrime = 1099511628211ULL;

  std::uint64_t hash = kOffsetBasis;
  for (unsigned char c : data) {
    hash ^= static_cast<std::uint64_t>(c);
    hash *= kPrime;
  }
  return hash;
}

}  // namespace

bool SaveRdb(const Store& store, const std::string& path) {
  std::vector<SnapshotEntry> entries = store.Snapshot();

  ByteWriter w;
  w.WriteBytes(kMagic, sizeof(kMagic));
  w.WriteU32(kFormatVersion);
  w.WriteU64(static_cast<std::uint64_t>(entries.size()));

  for (const auto& entry : entries) {
    w.WriteString(entry.key);
    w.WriteU8(entry.type == ValueType::kList ? kTypeList : kTypeString);

    w.WriteU8(entry.expires_at_wall.has_value() ? 1 : 0);
    if (entry.expires_at_wall.has_value()) {
      auto ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(entry.expires_at_wall->time_since_epoch()).count();
      w.WriteI64(ms);
    }

    if (entry.type == ValueType::kList) {
      w.WriteU32(static_cast<std::uint32_t>(entry.list_value.size()));
      for (const auto& elem : entry.list_value) {
        w.WriteString(elem);
      }
    } else {
      w.WriteString(entry.str_value);
    }
  }

  // Checksum covers everything written so far (magic through the last
  // entry) but not itself — it's appended after being computed.
  w.WriteU64(Fnv1a64(w.bytes()));

  // Write to a temp file and rename into place, rather than writing
  // straight to `path`: rename() is atomic on POSIX filesystems, so any
  // reader either sees the old complete file or the new complete file,
  // never a half-written one. A crash or disk-full error partway through
  // the write leaves at worst a stray .tmp file — `path` itself is never
  // touched until the write has already fully succeeded.
  std::string tmp_path = path + ".tmp";
  {
    std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
    if (!out) {
      return false;
    }
    out.write(w.bytes().data(), static_cast<std::streamsize>(w.bytes().size()));
    if (!out) {
      out.close();
      std::remove(tmp_path.c_str());
      return false;
    }
  }
  if (std::rename(tmp_path.c_str(), path.c_str()) != 0) {
    std::remove(tmp_path.c_str());
    return false;
  }
  return true;
}

bool LoadRdb(Store& store, const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return false;
  }
  std::string buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

  constexpr std::size_t kMinSize = sizeof(kMagic) + sizeof(std::uint32_t) + sizeof(std::uint64_t) /* count */
                                    + sizeof(std::uint64_t) /* checksum */;
  if (buf.size() < kMinSize) {
    return false;
  }

  // Validate the checksum over everything except itself before parsing
  // anything else — a corrupt file is rejected outright rather than
  // partially parsed.
  std::string payload = buf.substr(0, buf.size() - sizeof(std::uint64_t));
  std::string checksum_bytes = buf.substr(buf.size() - sizeof(std::uint64_t));
  ByteReader checksum_reader(checksum_bytes);
  std::uint64_t stored_checksum = 0;
  if (!checksum_reader.ReadU64(&stored_checksum)) {
    return false;
  }
  if (stored_checksum != Fnv1a64(payload)) {
    return false;
  }

  ByteReader r(payload);
  char magic[sizeof(kMagic)];
  if (!r.ReadBytes(magic, sizeof(magic)) || std::memcmp(magic, kMagic, sizeof(kMagic)) != 0) {
    return false;
  }
  std::uint32_t version;
  if (!r.ReadU32(&version) || version != kFormatVersion) {
    return false;
  }
  std::uint64_t count;
  if (!r.ReadU64(&count)) {
    return false;
  }

  std::vector<SnapshotEntry> entries;
  entries.reserve(count);
  auto wall_now = std::chrono::system_clock::now();

  for (std::uint64_t i = 0; i < count; ++i) {
    SnapshotEntry entry;
    if (!r.ReadString(&entry.key)) {
      return false;
    }
    std::uint8_t type_byte;
    if (!r.ReadU8(&type_byte)) {
      return false;
    }
    entry.type = (type_byte == kTypeList) ? ValueType::kList : ValueType::kString;

    std::uint8_t has_ttl;
    if (!r.ReadU8(&has_ttl)) {
      return false;
    }
    bool already_expired = false;
    if (has_ttl) {
      std::int64_t ms;
      if (!r.ReadI64(&ms)) {
        return false;
      }
      auto expiry_wall = std::chrono::system_clock::time_point(std::chrono::milliseconds(ms));
      if (expiry_wall <= wall_now) {
        // Already expired during whatever gap elapsed since this was
        // saved — noted, but its value payload still has to be read off
        // the wire below to keep the reader's cursor in sync with the
        // rest of the file; it just won't be added to `entries`.
        already_expired = true;
      } else {
        entry.expires_at_wall = expiry_wall;
      }
    }

    if (entry.type == ValueType::kList) {
      std::uint32_t elem_count;
      if (!r.ReadU32(&elem_count)) {
        return false;
      }
      entry.list_value.reserve(elem_count);
      for (std::uint32_t j = 0; j < elem_count; ++j) {
        std::string elem;
        if (!r.ReadString(&elem)) {
          return false;
        }
        entry.list_value.push_back(std::move(elem));
      }
    } else {
      if (!r.ReadString(&entry.str_value)) {
        return false;
      }
    }

    if (!already_expired) {
      entries.push_back(std::move(entry));
    }
  }

  store.LoadSnapshot(std::move(entries));
  return true;
}

}  // namespace goredis
