#pragma once
#include <stdint.h>

namespace storage
{
bool crc_file(const char *path, uint32_t &size, uint32_t &crc);
bool receive_usb(uint32_t size, const char *hash);
}
