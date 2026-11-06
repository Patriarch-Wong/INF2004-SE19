// Board 1: OV5640 camera. Included by main.cpp inside its namespace.

struct frame_size_choice
{
    framesize_t size;
    const char *name;
};

// Small sizes only: the LCD is 240 x 240 and every byte costs air time.
constexpr frame_size_choice frame_sizes[] = {
    {FRAMESIZE_96X96, "96x96"},
    {FRAMESIZE_QQVGA, "160x120"},
    {FRAMESIZE_HQVGA, "240x176"},
    {FRAMESIZE_240X240, "240x240"},
};
constexpr size_t frame_size_count =
    sizeof(frame_sizes) / sizeof(frame_sizes[0]);
constexpr size_t default_size = 3;  // 240x240, the whole screen

size_t size_index = default_size;
int quality = app_config::jpeg_quality;
uint16_t frame_id = 0;
uint32_t last_hello_ms = 0;
uint32_t last_heard_ms = 0;  // Session made, or last valid status.
uint32_t last_frame_ms = 0;
video_frame::status_report display_report = {};

// Stats since the last line.
uint32_t frames_sent = 0;
uint32_t bytes_sent = 0;
uint32_t chunks_sent = 0;
uint32_t ack_failures = 0;
uint32_t min_frame = UINT32_MAX;
uint32_t max_frame = 0;
uint32_t capture_wait_ms = 0;  // Time in esp_camera_fb_get().
uint32_t send_time_ms = 0;     // Time sealing and queueing the chunks.

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
    config.frame_size = frame_sizes[frame_size_count - 1].size;
    config.jpeg_quality = quality;
    // Buffers are filled only once returned. Continuous capture
    // (GRAB_LATEST) kept the camera task, which shares core 0 and priority
    // with Wi-Fi, busy at full sensor rate and delayed the radio. Two
    // buffers let the next capture run while this frame is sent; the
    // capture time in each chunk keeps playback timing true.
    config.fb_count = 2;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
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
    Serial.printf("CAMERA OV5640 | %s | JPEG quality %d\n",
        frame_sizes[size_index].name, quality);
    Serial.println("Commands: + better picture | - smaller frames | "
                   "1 96x96 2 160x120 3 240x176 4 240x240 | s stats");
    return true;
}

bool link_up(uint32_t now)
{
    return link.established() && peer_known &&
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
        // Broadcast: the display's MAC may not be known yet.
        send_control(video_radio::broadcast_mac, hello);
    }
    last_hello_ms = now;
}

void role_session_event(const video_radio::received_frame &frame,
    session::event result, const control_frame::message &response)
{
    if (result != session::event::established)
    {
        return;
    }
    // The reply carried a tag only the pair key holder can make.
    learn_peer(frame.mac);
    send_control(peer_mac, response);  // confirm
    last_heard_ms = millis();
}

void role_new_session()
{
}

void role_data(const video_radio::received_frame &frame, uint8_t type,
    const uint8_t *payload, size_t size, uint32_t now)
{
    (void)frame;
    if (type == video_frame::type_status &&
        video_frame::unpack_status(payload, size, &display_report))
    {
        last_heard_ms = now;
    }
}

void send_frame()
{
    const uint32_t started_ms = millis();
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb)
    {
        return;
    }
    const uint32_t got_ms = millis();
    // Capture time from the driver (time since boot at the frame's start),
    // not when this loop got round to it.
    const uint32_t capture_ms = static_cast<uint32_t>(
        fb->timestamp.tv_sec * 1000LL + fb->timestamp.tv_usec / 1000);
    const size_t count = video_frame::chunk_count(fb->len);
    if (count == 0)
    {
        esp_camera_fb_return(fb);
        return;
    }
    ++frame_id;
    uint8_t chunk[secure_message::max_payload_size];
    for (size_t i = 0; i < count; ++i)
    {
        const size_t size = video_frame::make_chunk(frame_id, capture_ms,
            fb->buf, fb->len, i, chunk, sizeof(chunk));
        if (!send_sealed(video_frame::type_video, chunk, size))
        {
            ++ack_failures;
        }
    }
    const uint32_t len = static_cast<uint32_t>(fb->len);
    esp_camera_fb_return(fb);
    capture_wait_ms += got_ms - started_ms;
    send_time_ms += millis() - got_ms;
    ++frames_sent;
    bytes_sent += len;
    chunks_sent += count;
    min_frame = len < min_frame ? len : min_frame;
    max_frame = len > max_frame ? len : max_frame;
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
    if (!link_up(millis()))
    {
        Serial.printf("TX waiting for board 2 | rejected %lu | other "
                      "boards' frames %lu\n",
            static_cast<unsigned long>(rejected_count),
            static_cast<unsigned long>(foreign_count));
    }
    else
    {
        const video_radio::send_counts sends = video_radio::send_failures();
        Serial.printf("TX %.1f fps | frame %lu B avg (%lu-%lu) | %.1f chunks"
                      " | %.1f KB/s | capture %lu ms send %lu ms | not sent"
                      " %lu (total no-ack %lu slot-timeout %lu error %lu) |"
                      " %s q%d | display shown %lu incomplete %lu rejected "
                      "%lu\n",
            frames_sent / seconds,
            static_cast<unsigned long>(frames_sent ? bytes_sent / frames_sent
                                                   : 0),
            static_cast<unsigned long>(frames_sent ? min_frame : 0),
            static_cast<unsigned long>(max_frame),
            frames_sent ? static_cast<float>(chunks_sent) / frames_sent : 0,
            bytes_sent / 1024.0f / seconds,
            static_cast<unsigned long>(frames_sent ? capture_wait_ms /
                                                         frames_sent
                                                   : 0),
            static_cast<unsigned long>(frames_sent ? send_time_ms /
                                                         frames_sent
                                                   : 0),
            static_cast<unsigned long>(ack_failures),
            static_cast<unsigned long>(sends.no_ack),
            static_cast<unsigned long>(sends.timeouts),
            static_cast<unsigned long>(sends.errors),
            frame_sizes[size_index].name, quality,
            static_cast<unsigned long>(display_report.frames_shown),
            static_cast<unsigned long>(display_report.frames_incomplete),
            static_cast<unsigned long>(display_report.rejected));
    }
    frames_sent = bytes_sent = chunks_sent = ack_failures = 0;
    capture_wait_ms = send_time_ms = 0;
    min_frame = UINT32_MAX;
    max_frame = 0;
}

void role_command(int c)
{
    sensor_t *sensor = esp_camera_sensor_get();
    if (c == '+' || c == '-')
    {
        // Lower number = better picture, bigger frames.
        quality += c == '+' ? -2 : 2;
        quality = quality < 4 ? 4 : (quality > 63 ? 63 : quality);
        sensor->set_quality(sensor, quality);
        Serial.printf("JPEG quality %d (4 best .. 63 smallest)\n", quality);
    }
    else if (c >= '1' && c <= '4')
    {
        size_index = static_cast<size_t>(c - '1');
        sensor->set_framesize(sensor, frame_sizes[size_index].size);
        Serial.printf("Frame size %s\n", frame_sizes[size_index].name);
    }
    else if (c == 's')
    {
        Serial.printf("Session %08lX | peer %s | frame ID %u | queue drops "
                      "%lu\n",
            static_cast<unsigned long>(link.session_id()),
            peer_known ? "known" : "unknown", frame_id,
            static_cast<unsigned long>(video_radio::queue_drops()));
    }
}
