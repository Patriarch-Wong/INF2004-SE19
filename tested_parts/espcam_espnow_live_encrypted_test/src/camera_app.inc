// Board 1: OV5640 camera. Included by main.cpp inside its namespace.

struct frame_size_choice
{
    framesize_t size;
    const char *name;
};

// Small sizes only: the display scales them up to its 240 x 240 screen.
constexpr frame_size_choice frame_sizes[] = {
    {FRAMESIZE_96X96, "96x96"},
    {FRAMESIZE_QQVGA, "160x120"},
    {FRAMESIZE_HQVGA, "240x176"},
    {FRAMESIZE_240X240, "240x240"},
};
constexpr size_t frame_size_count =
    sizeof(frame_sizes) / sizeof(frame_sizes[0]);
constexpr unsigned rates[] = {1, 2, 6, 12, 24};

size_t size_index = app_config::frame_size_index;
int quality = app_config::jpeg_quality;
uint32_t sequence = 0;
uint32_t last_frame_ms = 0;
uint32_t last_hello_ms = 0;
uint32_t last_heard_ms = 0;  // Session made, or last valid status report.
uint32_t display_shown = 0;
uint32_t display_incomplete = 0;
uint32_t display_rejected = 0;

// Stats since the last line.
uint32_t frames_sent = 0;
uint32_t bytes_sent = 0;
uint32_t packets_sent = 0;
uint32_t packets_not_sent = 0;
uint32_t age_total_ms = 0;   // Capture to the start of sending.
uint32_t send_total_ms = 0;  // Sealing and queueing the packets.

bool role_setup()
{
    const app_config::camera_pins &p = app_config::pins;
    camera_config_t config = {};
    config.pin_pwdn = p.pwdn;
    config.pin_reset = p.reset;
    config.pin_xclk = p.xclk;
    config.pin_sccb_sda = p.sda;
    config.pin_sccb_scl = p.scl;
    config.pin_d7 = p.d7;
    config.pin_d6 = p.d6;
    config.pin_d5 = p.d5;
    config.pin_d4 = p.d4;
    config.pin_d3 = p.d3;
    config.pin_d2 = p.d2;
    config.pin_d1 = p.d1;
    config.pin_d0 = p.d0;
    config.pin_vsync = p.vsync;
    config.pin_href = p.href;
    config.pin_pclk = p.pclk;
    config.xclk_freq_hz = static_cast<int>(app_config::xclk_hz);
    config.ledc_timer = LEDC_TIMER_0;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.pixel_format = PIXFORMAT_JPEG;
    // Buffers sized for the largest choice, so every size fits later.
    config.frame_size = frame_sizes[frame_size_count - 1].size;
    config.jpeg_quality = quality;
    // Newest frame always: the sensor keeps filling two buffers and
    // fb_get returns the latest, at most one sensor frame old.
    config.fb_count = 2;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.grab_mode = CAMERA_GRAB_LATEST;
    const esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK)
    {
        Serial.printf("Camera init 0x%x (%s)\n", static_cast<unsigned>(err),
            esp_err_to_name(err));
        return fail("camera init");
    }
    sensor_t *sensor = esp_camera_sensor_get();
    if (!sensor || sensor->id.PID != OV5640_PID)
    {
        return fail("sensor is not an OV5640");
    }
    sensor->set_framesize(sensor, frame_sizes[size_index].size);
    sensor->set_quality(sensor, quality);
    Serial.printf("CAMERA OV5640 | %s | JPEG quality %d | max %lu fps\n",
        frame_sizes[size_index].name, quality,
        static_cast<unsigned long>(app_config::max_fps));
    Serial.println("Commands: 1 96x96 2 160x120 3 240x176 4 240x240 | "
                   "+ sharper | - smaller | r radio rate | s status");
    return true;
}

bool link_up(uint32_t now)
{
    return link.established() &&
        now - last_heard_ms < app_config::link_timeout_ms;
}

void send_hello(uint32_t now)
{
    control_frame::message hello = {};
    bool built = link.hello(&hello);
    if (!built)
    {
        uint8_t nonce[session::nonce_size];
        esp_fill_random(nonce, sizeof(nonce));
        built = link.initiate(nonce, &hello);
        Serial.println("SESSION hello: looking for board 2");
    }
    if (built)
    {
        send_control(peer_mac, hello);
    }
    last_hello_ms = now;
}

void role_session_event(session::event result,
    const control_frame::message &response, uint32_t now)
{
    if (result != session::event::established)
    {
        return;
    }
    send_control(peer_mac, response);  // confirm
    last_heard_ms = now;
}

// New session: frame sequence starts again, so the display's receiver
// (which it resets too) never mixes frames from two sessions.
void role_new_session()
{
    sequence = 0;
}

void role_data(uint8_t type, const uint8_t *payload, size_t size,
    uint32_t now)
{
    if (type != video_link::sealed_status ||
        size != video_link::status_size ||
        payload[0] != video_link::status_type)
    {
        return;
    }
    display_shown = get_u32(payload + 1);
    display_incomplete = get_u32(payload + 5);
    display_rejected = get_u32(payload + 9);
    last_heard_ms = now;
}

void send_frame()
{
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb)
    {
        return;
    }
    const uint32_t started_ms = millis();
    const uint32_t captured_ms = static_cast<uint32_t>(
        fb->timestamp.tv_sec * 1000LL + fb->timestamp.tv_usec / 1000);
    const uint32_t jpeg_size = static_cast<uint32_t>(fb->len);
    const size_t count = video_link::packet_count(fb->len);
    if (count == 0)
    {
        esp_camera_fb_return(fb);
        return;
    }
    ++sequence;
    uint8_t packet[video_link::max_packet];
    for (size_t i = 0; i < count; ++i)
    {
        const size_t size = video_link::make_packet(sequence, fb->buf,
            fb->len, i, packet, sizeof(packet));
        if (!send_sealed(video_link::sealed_video, packet, size))
        {
            ++packets_not_sent;
        }
    }
    esp_camera_fb_return(fb);
    ++frames_sent;
    bytes_sent += jpeg_size;
    packets_sent += count;
    age_total_ms += started_ms - captured_ms;
    send_total_ms += millis() - started_ms;
}

void role_loop(uint32_t now)
{
    if (!link_up(now))
    {
        if (now - last_hello_ms >= app_config::hello_interval_ms)
        {
            send_hello(now);
        }
        delay(5);
        return;
    }
    if (app_config::max_fps > 0 &&
        now - last_frame_ms < 1000 / app_config::max_fps)
    {
        delay(1);
        return;
    }
    last_frame_ms = now;
    send_frame();
}

void role_stats(uint32_t elapsed_ms)
{
    const float seconds = elapsed_ms / 1000.0f;
    const uint32_t n = frames_sent ? frames_sent : 1;
    const radio::send_counts sends = radio::send_failures();
    if (!link_up(millis()))
    {
        Serial.printf("TX waiting for board 2 | rejected %lu | other "
                      "frames %lu\n",
            static_cast<unsigned long>(rejected_count),
            static_cast<unsigned long>(foreign_count));
        frames_sent = bytes_sent = packets_sent = packets_not_sent = 0;
        age_total_ms = send_total_ms = 0;
        return;
    }
    Serial.printf("TX %.1f fps | %s q%d | frame %lu B, %.1f packets | "
                  "%.1f KB/s at %u Mbps | frame age %lu ms, send %lu ms, "
                  "seal %lu us/packet | not sent %lu (no-ack %lu total) | "
                  "display shown %lu incomplete %lu rejected %lu\n",
        frames_sent / seconds, frame_sizes[size_index].name, quality,
        static_cast<unsigned long>(bytes_sent / n),
        static_cast<float>(packets_sent) / n,
        bytes_sent / 1024.0f / seconds, radio::rate(),
        static_cast<unsigned long>(age_total_ms / n),
        static_cast<unsigned long>(send_total_ms / n),
        static_cast<unsigned long>(seal_count ? seal_total_us / seal_count
                                              : 0),
        static_cast<unsigned long>(packets_not_sent),
        static_cast<unsigned long>(sends.no_ack),
        static_cast<unsigned long>(display_shown),
        static_cast<unsigned long>(display_incomplete),
        static_cast<unsigned long>(display_rejected));
    frames_sent = bytes_sent = packets_sent = packets_not_sent = 0;
    age_total_ms = send_total_ms = 0;
}

void role_command(int c)
{
    sensor_t *sensor = esp_camera_sensor_get();
    if (c >= '1' && c <= '4')
    {
        size_index = static_cast<size_t>(c - '1');
        sensor->set_framesize(sensor, frame_sizes[size_index].size);
        Serial.printf("Frame size %s\n", frame_sizes[size_index].name);
    }
    else if (c == '+' || c == '-')
    {
        quality += c == '+' ? -2 : 2;
        quality = quality < 4 ? 4 : (quality > 63 ? 63 : quality);
        sensor->set_quality(sensor, quality);
        Serial.printf("JPEG quality %d (4 sharpest .. 63 smallest)\n",
            quality);
    }
    else if (c == 'r')
    {
        size_t next = 0;
        for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); ++i)
        {
            if (rates[i] == radio::rate())
            {
                next = (i + 1) % (sizeof(rates) / sizeof(rates[0]));
            }
        }
        Serial.printf("Radio rate %u Mbps: %s\n", rates[next],
            radio::set_rate(rates[next]) ? "ok" : "failed");
    }
    else if (c == 's')
    {
        Serial.printf("Session %08lX | sequence %lu | rejected %lu "
                      "(replay %lu) | other frames %lu | queue drops %lu\n",
            static_cast<unsigned long>(link.session_id()),
            static_cast<unsigned long>(sequence),
            static_cast<unsigned long>(rejected_count),
            static_cast<unsigned long>(replay_count),
            static_cast<unsigned long>(foreign_count),
            static_cast<unsigned long>(radio::queue_drops()));
    }
}
