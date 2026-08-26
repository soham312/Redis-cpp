// RDB-style whole-store snapshot persistence: SaveRdb/LoadRdb serialize
// Store's entire live dataset to/from a single file, this project's own
// binary format (see rdb.cpp for the exact byte layout).
//
// RDB (a full point-in-time snapshot), not AOF (a log of every write
// command, replayed on startup), was chosen for this stage deliberately:
// AOF logs *commands*, but there's no command parser/dispatcher yet —
// that's Stage 5 (networking/RESP). RDB instead operates directly on
// Store's existing data via Store::Snapshot()/LoadSnapshot(), so it fits
// cleanly at this point in the project without needing to invent a
// command-log format ahead of the command layer that would produce it.
// AOF remains a natural, separate addition once Stage 5 exists.
//
// Deliberately free functions, not a class: there's no state to hold
// between a save and a load (each call reads/writes one self-contained
// file), so a class would just be ceremony around two operations.
#pragma once

#include <string>

#include "store/store.h"

namespace goredis {

// SaveRdb writes a complete snapshot of store's current contents to
// path. Writes to a temporary file first and atomically renames it into
// place on success (see rdb.cpp) — a crash or write failure partway
// through never leaves a corrupt or truncated file at `path`, only (at
// worst) a stray temp file. Returns false, with `path` left completely
// untouched, if the write couldn't be completed for any reason (e.g. an
// unwritable directory) — treat this as "nothing was persisted this
// time," not a fatal error; the caller can simply retry later, matching
// how a failed BGSAVE doesn't crash a running Redis server.
bool SaveRdb(const Store& store, const std::string& path);

// LoadRdb replaces store's entire contents with what's recorded in the
// file at path. The file is fully read and validated (magic bytes,
// format version, checksum) before anything is applied to store, so a
// missing, truncated, or corrupted file always leaves store completely
// untouched and this returns false — there's no partially-loaded state
// to worry about. A caller can treat "returned false" uniformly as
// "start from an empty store."
bool LoadRdb(Store& store, const std::string& path);

}  // namespace goredis
