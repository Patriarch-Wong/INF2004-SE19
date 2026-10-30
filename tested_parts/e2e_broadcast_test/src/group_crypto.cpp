#include "group_crypto.h"
namespace group_crypto {
secure_message::status seal(const uint8_t *key, const uint8_t *hop,
    uint8_t sender, uint32_t counter, const uint8_t *plain, size_t size,
    uint8_t *out, size_t capacity, size_t *written)
{
    if (sender < 1 || sender > 3) return secure_message::status::bad_header;
    const secure_message::header fields = {secure_message::version,
        secure_message::type_voice, sender, destination, counter};
    return secure_message::seal(key, hop, fields, plain, size, out, capacity, written);
}
bool open(const uint8_t *key, const uint8_t *hop, uint8_t expected_sender,
    replay_window &window, const uint8_t *wire, size_t size,
    uint8_t *plain, size_t capacity, size_t *written)
{
    secure_message::header fields = {};
    if (secure_message::open(key, hop, wire, size, &fields, plain, capacity,
            written) != secure_message::status::ok ||
        expected_sender < 1 || expected_sender > 3 ||
        fields.source != expected_sender || fields.destination != destination ||
        fields.type != secure_message::type_voice ||
        !window.is_fresh(fields.counter)) return false;
    window.accept(fields.counter);
    return true;
}
}
