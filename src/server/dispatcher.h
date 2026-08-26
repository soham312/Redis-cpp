#pragma once

#include <string>
#include <vector>

#include "resp/resp_value.h"
#include "store/store.h"

namespace goredis {

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
RespValue Dispatch(Store& store, const std::vector<std::string>& args);

}  // namespace goredis
