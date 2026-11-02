#include "crypto_self_test.h"
#include <stdio.h>
int main() {
  const bool ok = cryptoSelfTest();
  puts(ok ? "PASS: AES-256-GCM vector, both sender IDs, 1/16/32/200-byte round-trips, header/cipher/tag tamper, wrong key" : "FAIL: AES-256-GCM");
  return ok ? 0 : 1;
}
