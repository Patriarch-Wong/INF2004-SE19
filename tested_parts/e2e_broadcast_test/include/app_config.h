#pragma once

#include <stddef.h>
#include <stdint.h>

namespace app_config
{
constexpr char project_name[] = "ESP-NOW Opus broadcast";
constexpr uint32_t serial_baud = 921600;
constexpr uint32_t serial_wait_ms = 2000;
// Serial output is buffered so printing does not stall the radio path.
constexpr size_t serial_tx_buffer_size = 4096;

// Stage 6's Opus settings: the codec_test configuration, 60 s at 50 frames
// per second.
constexpr uint32_t opus_bitrate_bps = 24000;
constexpr uint8_t opus_complexity = 5;
constexpr uint32_t voice_frames = 3000;
// Live playout depth. Delay is acceptable; delivery is what matters.
constexpr uint32_t playout_depth_us = 200000;
// The player looks at a frame this long after it is due, so one stamped
// just before its slot has been stored by then. Whether it was on time
// still depends only on its arrival stamp.
constexpr uint32_t playout_margin_us = 2000;
// Replays yield to the idle task every this many frames.
constexpr uint32_t replay_yield_frames = 25;
// One task makes every Opus call (the library shares scratch memory). It
// runs on core 0 below Wi-Fi, leaving core 1 to the radio loop.
constexpr uint32_t codec_stack_bytes = 64 * 1024;
constexpr int codec_core = 0;
constexpr unsigned codec_priority = 1;
// After each stream, the slot choice is counted again at these depths from
// the stored arrivals, without decoding.
constexpr uint32_t sweep_depths_us[] = {60000, 100000, 150000, 200000,
    300000};
// A receiver gives up on a stream when nothing from it arrives this long.
constexpr uint32_t stream_timeout_ms = 5000;
// After its last tick, the sender gives the codec this long to finish.
constexpr uint32_t sender_drain_us = 500000;
constexpr uint32_t status_interval_ms = 10000;
}
