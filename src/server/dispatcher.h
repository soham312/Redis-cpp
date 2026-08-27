#pragma once

#include <string>
#include <vector>

#include "resp/resp_value.h"
#include "store/store.h"

namespace goredis {

// Forward-declared, not #included: Dispatch only ever touches AofWriter
// through a pointer, so the full definition (server/aof.h) isn't needed
// here — keeping this header's own dependency surface minimal, the same
// layering discipline the rest of this project follows.
class AofWriter;

// Dispatch executes one already-parsed command (args[0] is the command
// name, args[1:] its arguments — see CommandParser) against store and
// returns the RESP reply to send back. Never throws: every error path
// (unknown command, wrong arity, a non-integer argument where one's
// required, WRONGTYPE from Store) is represented as a RespValue::Error
// reply, the normal way a real Redis server reports a command-level
// error — as opposed to CommandParser's kProtocolError, which is
// reserved for the wire format itself being malformed. args is never
// empty when this is called (CommandParser never produces an empty
// command).
//
// aof, if non-null, gets every command that actually mutated the store
// appended to it (see server/aof.h) — the single point where "did this
// write succeed" is already known, so logging happens here rather than
// duplicating that same success/failure logic in the caller. Defaulted
// to nullptr so every existing call site (including the whole test
// suite) keeps compiling unchanged when AOF logging isn't in play.
RespValue Dispatch(Store& store, const std::vector<std::string>& args, AofWriter* aof = nullptr);

}  // namespace goredis
