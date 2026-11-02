#include "control_frame.h"
#include <string.h>

namespace control_frame
{
using secure_message::status;

bool is_control(uint8_t type)
{
    return type == type_pairing || type == type_session;
}

status build(const uint8_t *network_key, const secure_message::header &fields,
    const message &content, uint8_t *frame, size_t frame_capacity,
    size_t *frame_size)
{
    if (!network_key || !frame || !frame_size)
    {
        return status::bad_argument;
    }
    if (!is_control(fields.type) || fields.counter == 0)
    {
        return status::bad_header;
    }
    const size_t size = fixed_size + content.body_size;
    if (content.body_size < min_body_size ||
        content.body_size > max_body_size || frame_capacity < size)
    {
        return status::bad_length;
    }
    frame[0] = secure_message::version;
    frame[1] = fields.type;
    frame[2] = fields.source;
    frame[3] = fields.destination;
    for (size_t i = 0; i < 4; ++i)
    {
        frame[4 + i] = static_cast<uint8_t>(fields.counter >> (8 * i));
    }
    frame[secure_message::header_size] = content.step;
    memcpy(frame + secure_message::header_size + step_size, content.body,
        content.body_size);
    *frame_size = size;
    return secure_message::sign_hop(network_key, frame, size);
}

status parse(const uint8_t *network_key, const uint8_t *frame,
    size_t frame_size, secure_message::header *fields, message *content)
{
    if (!content)
    {
        return status::bad_argument;
    }
    secure_message::header decoded = {};
    const status result = secure_message::verify_hop(network_key, frame,
        frame_size, &decoded);
    if (result != status::ok)
    {
        return result;
    }
    if (!is_control(decoded.type))
    {
        return status::bad_header;
    }
    content->step = frame[secure_message::header_size];
    content->body_size = frame_size - fixed_size;
    memcpy(content->body, frame + secure_message::header_size + step_size,
        content->body_size);
    if (fields)
    {
        *fields = decoded;
    }
    return status::ok;
}
}
