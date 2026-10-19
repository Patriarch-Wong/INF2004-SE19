#pragma once

#include "stream_format.h"

namespace ram_clip
{
// Stop and join the player before begin()/clear() replaces its memory.
bool begin(uint32_t size);
void clear();
bool append(uint32_t offset, const uint8_t *data, uint32_t length);
bool finish(uint32_t expected_crc);
bool has_header();
bool complete();
uint32_t available_samples();
uint32_t received();
const stream_format::header &info();
bool next(uint32_t &cursor, uint32_t kind, stream_format::record &out);
}
