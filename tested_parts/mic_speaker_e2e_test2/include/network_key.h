#pragma once

// Every key is compiled in: the network key and this board's pair key.
// There is no pairing step; session keys are derived from the pair key at
// every boot.
#if __has_include("e2e_keys.h")
#include "e2e_keys.h"
#else
#error "Missing include/e2e_keys.h: run python3 tools/generate_keys.py"
#endif
