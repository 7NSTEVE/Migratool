#pragma once
#include "migratool/gateway.hpp"

namespace mt {
// Select semantic inputs from the recorded inventory. This never enumerates
// the source tree and retains only the bounded project/reference component.
Json roslyn_indexed_context(const Project&, const Json& requested, Cancel* = nullptr);
}
