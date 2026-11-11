#include "camera_security.h"

#include <string.h>

namespace camera_security
{
namespace
{
constexpr uint8_t camera_id = 1;
constexpr uint8_t display_id = 2;
constexpr uint8_t sealed_video = 5;
}

observer::observer(const uint8_t *network_key, const uint8_t *pair_key)
{
    if (network_key && link.begin(display_id, camera_id, pair_key))
    {
        memcpy(network, network_key, sizeof(network));
    }
}

result observer::handle(const uint8_t *packet, size_t size, bool from_camera,
    uint8_t *video, size_t *video_size)
{
    *video_size = 0;
    if (size < secure_message::overhead ||
        size > secure_message::max_frame_size ||
        packet[0] != secure_message::version ||
        packet[2] != (from_camera ? camera_id : display_id) ||
        packet[3] != (from_camera ? display_id : camera_id))
    {
        return result::ignored;
    }

    const uint8_t type = packet[1];
    if (type == control_frame::type_session)
    {
        if (!link.paired())
        {
            return result::waiting;
        }
        control_frame::message message = {};
        const auto status = control_frame::parse(network, packet, size,
            nullptr, &message);
        if (status != secure_message::status::ok)
        {
            return status == secure_message::status::bad_hop_tag ?
                result::wrong_key : result::ignored;
        }
        const auto step = static_cast<session::step>(message.step);
        if (step != (from_camera ? session::step::confirm : session::step::reply))
        {
            return result::ignored;
        }
        const auto event = link.observe_exchange(message);
        if (event == session::event::bad_tag)
        {
            return result::wrong_key;
        }
        return event == session::event::observed ? result::waiting :
            result::ignored;
    }
    if (!from_camera || type != sealed_video)
    {
        return result::ignored;
    }

    const auto opened = link.open(network, packet, size, nullptr, video,
        secure_message::max_payload_size, video_size);
    switch (opened)
    {
    case session::open_result::accepted:
        return result::video;
    case session::open_result::accepted_new_session:
        return result::new_session;
    case session::open_result::no_session:
    case session::open_result::bad_tag:
        return result::waiting;
    default:
        return result::ignored;
    }
}
}
