#pragma once

#include <stdint.h>

#include "video_link.h"

namespace radio
{
struct received_frame
{
    bool from_camera;
    uint8_t size;
    uint8_t bytes[video_link::max_packet];
};

const char *start();
bool receive(received_frame *frame);
uint32_t queue_drops();
void print_status();
}
