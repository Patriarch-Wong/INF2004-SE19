#pragma once

#include <stdint.h>
#include "frame_decode.h"
#include "sniffer_config.h"

// LoRa capture on the Core1121 (LR1121). One FreeRTOS task owns the radio
// and SPI; DIO9 only wakes it. Receive only: the task never calls
// transmit. If the radio does not answer, the task records why and tries
// again every sniffer_config::lora_retry_ms; ESP-NOW capture carries on.
namespace lora_rx
{
enum class state : uint8_t
{
    off,        // begin() not called.
    starting,   // Resetting and configuring.
    listening,
    paused,     // `lora off`: the radio is in standby.
    failed,     // Start-up or a command failed; retried.
};

struct counters
{
    uint32_t starts;       // Start-up attempts, including settings changes.
    uint32_t received;     // Packets queued, CRC good or bad.
    uint32_t crc_errors;   // Of those, CRC or header errors.
    uint32_t read_errors;  // Empty packets and SPI read failures.
    uint32_t queue_drops;  // Print queue full.
};

// Creates the queue and task; the task starts the radio. False only if
// those could not be created.
bool begin(const sniffer_config::lora_settings &initial);

// Restarts the radio with new settings. False if they are not valid.
bool apply(const sniffer_config::lora_settings &next);
sniffer_config::lora_settings settings();

void set_enabled(bool on);
bool enabled();

state current();
const char *state_name(state value);
int16_t last_error();
const char *failed_step();
// BUSY still high after a reset points at wiring or power, not SPI.
bool busy_stuck();

bool receive(frame_decode::lora_capture *out);

counters stats();
}
