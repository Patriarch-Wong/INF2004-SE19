#pragma once

#include <stddef.h>
#include <stdint.h>

// LoRa texts ("Hello from Board N", "Bye from Board N") with delivery
// receipts. LoRa is half duplex: a text sent while the other board is
// sending (a heartbeat, its own text) is lost, so each text is numbered,
// the receiver answers every copy with a receipt, and the sender tries up
// to `tries` times until one comes back. The receiver shows a number once.
//
// Sealed as secure_message::type_text under the LoRa session key, so the
// receipt is authenticated like the text. No radio or timing calls, so
// this runs in the host tests.
//
// Payload:
//   offset  size  field
//   0       1     kind (1 text, 2 receipt)
//   1       1     text number, 1..255
//   2       n     text, printable ASCII (texts only; receipts end here)
namespace lora_text
{
constexpr uint8_t kind_text = 1;
constexpr uint8_t kind_receipt = 2;
constexpr size_t header_size = 2;

struct message
{
    uint8_t kind;
    uint8_t number;
    const uint8_t *text;  // Points into the payload; nullptr for receipts.
    size_t text_size;
};

bool build_text(uint8_t number, const char *text, uint8_t *payload,
    size_t capacity, size_t *size);
bool build_receipt(uint8_t number, uint8_t *payload, size_t capacity,
    size_t *size);
// False for anything malformed: unknown kind, number 0, a text with no
// characters, a receipt with any.
bool parse(const uint8_t *payload, size_t size, message *content);

enum class event : uint8_t
{
    none,
    send,       // Send (or send again) the pending text now.
    delivered,  // A receipt for the pending text came back.
    failed,     // No receipt after every try.
};

// One text in flight at a time; a new one replaces it.
class sender
{
public:
    // retry_ms plus a random 0..jitter_ms between tries.
    void begin(uint8_t tries, uint32_t retry_ms, uint32_t jitter_ms,
        uint32_t seed);

    // A new text: returns its number. The first try is due at once.
    uint8_t start(uint32_t now);
    // Stops waiting (disconnected).
    void cancel();
    // A receipt arrived: delivered if it is for the pending text, else
    // none (a late receipt for an earlier try or text).
    event receipt(uint8_t number);
    // send when a try is due (the caller sends it at once), failed once
    // the last try has gone unanswered for retry_ms.
    event poll(uint32_t now);

    bool pending() const
    {
        return waiting;
    }
    uint8_t number() const
    {
        return current;
    }
    uint8_t tries_sent() const
    {
        return sent;
    }

private:
    uint32_t next_random();

    uint8_t max_tries = 3;
    uint32_t retry = 1500;
    uint32_t jitter = 0;
    uint32_t random_state = 1;
    bool waiting = false;
    uint8_t current = 0;  // Last number used; 1..255, never 0.
    uint8_t sent = 0;
    uint32_t next_ms = 0;
};

// Remembers the last number shown, so a repeated try is not shown twice.
class receiver
{
public:
    // A new connection: the next text is new whatever its number.
    void reset()
    {
        last = 0;
    }
    // True the first time a number arrives in a row.
    bool is_new(uint8_t number)
    {
        if (number == last)
        {
            return false;
        }
        last = number;
        return true;
    }

private:
    uint8_t last = 0;
};
}
