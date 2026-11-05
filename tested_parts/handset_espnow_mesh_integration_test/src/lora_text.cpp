#include "lora_text.h"

#include <string.h>

namespace lora_text
{
namespace
{
// Signed difference: survives millis() wrapping.
bool reached(uint32_t now, uint32_t at)
{
    return static_cast<int32_t>(now - at) >= 0;
}
}

bool build_text(uint8_t number, const char *text, uint8_t *payload,
    size_t capacity, size_t *size)
{
    if (!text || !payload || !size || number == 0)
    {
        return false;
    }
    const size_t n = strlen(text);
    if (n == 0 || header_size + n > capacity)
    {
        return false;
    }
    payload[0] = kind_text;
    payload[1] = number;
    memcpy(payload + header_size, text, n);
    *size = header_size + n;
    return true;
}

bool build_receipt(uint8_t number, uint8_t *payload, size_t capacity,
    size_t *size)
{
    if (!payload || !size || number == 0 || capacity < header_size)
    {
        return false;
    }
    payload[0] = kind_receipt;
    payload[1] = number;
    *size = header_size;
    return true;
}

bool parse(const uint8_t *payload, size_t size, message *content)
{
    if (!payload || !content || size < header_size || payload[1] == 0)
    {
        return false;
    }
    const uint8_t kind = payload[0];
    if ((kind == kind_text && size == header_size) ||
        (kind == kind_receipt && size != header_size) ||
        (kind != kind_text && kind != kind_receipt))
    {
        return false;
    }
    content->kind = kind;
    content->number = payload[1];
    content->text = kind == kind_text ? payload + header_size : nullptr;
    content->text_size = size - header_size;
    return true;
}

void sender::begin(uint8_t tries, uint32_t retry_ms, uint32_t jitter_ms,
    uint32_t seed)
{
    *this = sender();
    max_tries = tries > 0 ? tries : 1;
    retry = retry_ms;
    jitter = jitter_ms;
    random_state = seed != 0 ? seed : 0x9E3779B9u;
}

uint32_t sender::next_random()
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}

uint8_t sender::start(uint32_t now)
{
    current = current == 255 ? 1 : current + 1;
    waiting = true;
    sent = 0;
    next_ms = now;
    return current;
}

void sender::cancel()
{
    waiting = false;
}

event sender::receipt(uint8_t number)
{
    if (!waiting || number != current)
    {
        return event::none;
    }
    waiting = false;
    return event::delivered;
}

event sender::poll(uint32_t now)
{
    if (!waiting || !reached(now, next_ms))
    {
        return event::none;
    }
    if (sent >= max_tries)
    {
        waiting = false;
        return event::failed;
    }
    ++sent;
    // After the last try only retry_ms: it is how long a receipt may take.
    next_ms = now + retry +
        (sent < max_tries && jitter > 0 ? next_random() % jitter : 0);
    return event::send;
}
}
