#pragma once

// Every key is compiled in: the network key and this board's pair key.
// There is no pairing step; session keys are derived from the pair key at
// every boot.
// The keys live in tested_parts/shared_keys/e2e_keys.h, shared by every
// project (platformio.ini adds that directory to the include path).
#if __has_include("e2e_keys.h")
#include "e2e_keys.h"
#else
#error "Missing shared_keys/e2e_keys.h: run python3 ../shared_keys/generate_keys.py"
#endif
