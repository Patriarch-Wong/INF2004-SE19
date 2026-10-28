#pragma once

// The network key is the only key compiled into the firmware. Pair keys are
// made by pairing and kept in NVS.
#if __has_include("e2e_keys.h")
#include "e2e_keys.h"
#else
#error "Missing include/e2e_keys.h: run python3 tools/generate_keys.py"
#endif
