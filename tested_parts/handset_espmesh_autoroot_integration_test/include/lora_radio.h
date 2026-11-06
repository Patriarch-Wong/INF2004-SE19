#pragma once

#include <stddef.h>
#include <stdint.h>
#include "secure_message.h"

// Core1121 (LR1121) LoRa radio, owned by one FreeRTOS task (design review:
// "LoRa Transport task owns SPI access; DIO9 ISR only notifies the task").
// The loop hands it whole secure_message frames to send and takes received
// ones from a queue, so a transmission (about 80-130 ms on air) never holds
// up the loop, the screen or the audio.
//
// The radio settings, start-up and send/receive steps are the ones that
// worked in lora_encrypt_test (RadioLib 7.7.1, 125 kHz, SF7, CR 4/5, sync
// word 0x12, HP PA at 0 dBm). The radio sits on the default SPI bus, as
// there; the LCD has its own (HSPI).
//
// Start-up never stops the board: if the radio does not answer (for
// example RadioLib -705, BUSY stuck), the task records why and tries again
// every app_config::lora_retry_ms. Only the loop prints; it reads the state
// and counters from here.
namespace lora_radio
{
enum class state : uint8_t
{
    off,       // begin() not called.
    starting,  // Resetting and configuring the radio.
    ok,        // Listening; sends queued frames.
    failed,    // Start-up or a radio command failed; retried.
};

struct received_frame
{
    uint32_t received_ms;
    float rssi;
    float snr;
    uint8_t size;
    uint8_t bytes[secure_message::max_frame_size];
};

struct counters
{
    uint32_t starts;       // Start-up attempts.
    uint32_t sent;
    uint32_t send_failed;  // The radio refused a transmission.
    uint32_t received;
    uint32_t rx_errors;    // CRC or read errors, bad lengths.
    uint32_t tx_drops;     // Send queue full, or the radio not ready.
    uint32_t rx_drops;     // Receive queue full.
};

// Creates the queues and the task. The task starts the radio. False only
// if the task or queues could not be created.
bool begin();

state current();
const char *state_name(state value);

// The last failure: RadioLib code, the step that failed, and whether BUSY
// was still high after a reset (wiring or power, not SPI).
int16_t last_error();
const char *failed_step();
bool busy_stuck();
// How long BUSY took to go low after the last reset.
uint32_t busy_low_ms();

// Queues a frame to send. False if the radio is not ready or the queue
// is full (counted as tx_drops).
bool send(const uint8_t *frame, size_t size);

// Next received frame, without waiting.
bool receive(received_frame *frame);

counters stats();
}
