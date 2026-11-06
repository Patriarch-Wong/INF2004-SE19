// Board 2: ST7789 240 x 240 display. Included by main.cpp inside its
// namespace.

Adafruit_ST7789 lcd(&SPI, app_config::lcd_cs_pin, app_config::lcd_dc_pin,
    app_config::lcd_reset_pin);

uint8_t *frame_buffer = nullptr;  // Reassembled JPEG, PSRAM.
uint8_t *pixels = nullptr;        // Decoded RGB565, big-endian, PSRAM.
video_frame::assembler *frames = nullptr;
video_frame::playout *buffer = nullptr;  // Frames waiting for their time.
uint32_t last_status_ms = 0;
uint16_t shown_width = 0;
uint16_t shown_height = 0;
bool showing = false;
bool big_endian = app_config::pixels_big_endian;
bool inverted = app_config::lcd_inverted;
uint32_t spi_hz = app_config::lcd_spi_hz;

void print_lcd_settings();

// Totals, reported to the camera.
uint32_t frames_shown = 0;
uint32_t bad_frames = 0;  // Complete but not a JPEG that fits the screen.

// Stats since the last line.
uint32_t window_shown = 0;
uint32_t window_bytes = 0;
uint32_t window_decode_ms = 0;
uint32_t window_draw_ms = 0;

void boot_screen(const char *line)
{
    lcd.fillScreen(ST77XX_BLACK);
    lcd.setTextColor(ST77XX_WHITE);
    lcd.setTextSize(2);
    lcd.setCursor(8, 90);
    lcd.print(line);
    lcd.setTextSize(1);
    lcd.setCursor(8, 130);
    const uint8_t *mac = video_radio::own_mac();
    lcd.printf("Board 2  %02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1],
        mac[2], mac[3], mac[4], mac[5]);
}

bool role_setup()
{
    frame_buffer = static_cast<uint8_t *>(
        ps_malloc(video_frame::max_frame_bytes));
    pixels = static_cast<uint8_t *>(
        ps_malloc(app_config::lcd_width * app_config::lcd_height * 2));
    uint8_t *slots = static_cast<uint8_t *>(ps_malloc(
        app_config::playout_slots * app_config::playout_slot_bytes));
    if (!frame_buffer || !pixels || !slots)
    {
        return fail("PSRAM buffers");
    }
    static video_frame::assembler assembler(frame_buffer,
        video_frame::max_frame_bytes);
    frames = &assembler;
    static video_frame::playout playout(slots, app_config::playout_slot_bytes,
        app_config::playout_slots, app_config::playout_delay_ms);
    buffer = &playout;

    // Set the level before enabling the output so BLK never glitches low.
    digitalWrite(app_config::lcd_backlight_pin, HIGH);
    pinMode(app_config::lcd_backlight_pin, OUTPUT);
    SPI.begin(app_config::lcd_sclk_pin, -1, app_config::lcd_mosi_pin,
        app_config::lcd_cs_pin);
    lcd.init(app_config::lcd_width, app_config::lcd_height, SPI_MODE3);
    lcd.setSPISpeed(app_config::lcd_spi_hz);
    lcd.setRotation(app_config::lcd_rotation);
    lcd.invertDisplay(app_config::lcd_inverted);
    boot_screen("Waiting for camera");
    print_lcd_settings();
    Serial.println("DISPLAY ST7789 240x240 | waiting for board 1");
    Serial.println("Commands: s status | b swap pixel bytes | i invert | "
                   "f SPI 40/20/10 MHz | d delay 150/300/500/800 ms");
    return true;
}

void role_session_event(const video_radio::received_frame &frame,
    session::event result, const control_frame::message &response)
{
    (void)frame;
    if (result == session::event::replied)
    {
        // Broadcast: the camera's MAC is learned only once it proves the
        // pair key (confirm, or a frame under the new session).
        send_control(video_radio::broadcast_mac, response);
    }
    else if (result == session::event::confirmed)
    {
        learn_peer(frame.mac);
    }
}

// New session: the camera may have restarted, so its frame IDs and clock
// start over.
void role_new_session()
{
    frames->reset();
    buffer->reset();
}

void show_frame(const uint8_t *jpeg, size_t size)
{
    uint16_t width = 0;
    uint16_t height = 0;
    if (!video_frame::jpeg_dimensions(jpeg, size, &width, &height) ||
        width == 0 || height == 0 || width > app_config::lcd_width ||
        height > app_config::lcd_height)
    {
        ++bad_frames;
        return;
    }
    const uint32_t started_ms = millis();
    if (!jpg2rgb565(jpeg, size, pixels, JPG_SCALE_NONE))
    {
        ++bad_frames;
        return;
    }
    const uint32_t decoded_ms = millis();
    if (!showing || width != shown_width || height != shown_height)
    {
        lcd.fillScreen(ST77XX_BLACK);
        shown_width = width;
        shown_height = height;
        showing = true;
    }
    lcd.startWrite();
    lcd.setAddrWindow((app_config::lcd_width - width) / 2,
        (app_config::lcd_height - height) / 2, width, height);
    lcd.writePixels(reinterpret_cast<uint16_t *>(pixels),
        static_cast<uint32_t>(width) * height, true, big_endian);
    lcd.endWrite();
    ++frames_shown;
    ++window_shown;
    window_bytes += size;
    window_decode_ms += decoded_ms - started_ms;
    window_draw_ms += millis() - decoded_ms;
}

void role_data(const video_radio::received_frame &frame, uint8_t type,
    const uint8_t *payload, size_t size, uint32_t now)
{
    (void)frame;
    (void)now;
    if (type != video_frame::type_video)
    {
        return;
    }
    if (frames->add(payload, size) ==
        video_frame::assembler::result::complete)
    {
        buffer->push(frames->frame(), frames->frame_size(),
            frames->capture_ms(), millis());
    }
}

void role_loop(uint32_t now)
{
    // Clock read fresh: draining the radio queue took time since `now`.
    const video_frame::playout::frame *next = buffer->due(millis());
    if (next)
    {
        show_frame(next->data, next->size);
        buffer->pop();
    }
    if (link.established() && peer_known &&
        now - last_status_ms >= app_config::status_interval_ms)
    {
        last_status_ms = now;
        const video_frame::status_report report = {frames_shown,
            frames->incomplete_frames(), rejected_count + bad_frames};
        uint8_t payload[video_frame::status_size];
        video_frame::pack_status(report, payload);
        send_sealed(video_frame::type_status, payload, sizeof(payload));
    }
    delay(1);
}

void role_stats(uint32_t elapsed_ms)
{
    const float seconds = elapsed_ms / 1000.0f;
    if (!link.established())
    {
        Serial.printf("RX waiting for board 1 | rejected %lu | other "
                      "boards' frames %lu\n",
            static_cast<unsigned long>(rejected_count),
            static_cast<unsigned long>(foreign_count));
        return;
    }
    Serial.printf("RX %.1f fps | %lux%lu | frame %lu B avg | %.1f KB/s | "
                  "decode %lu ms draw %lu ms | buffer %u delay %lu ms skipped"
                  " %lu overflow %lu resync %lu | incomplete %lu | bad %lu | "
                  "rejected %lu (replay %lu) | other boards %lu | queue drops"
                  " %lu\n",
        window_shown / seconds, static_cast<unsigned long>(shown_width),
        static_cast<unsigned long>(shown_height),
        static_cast<unsigned long>(window_shown ? window_bytes / window_shown
                                                : 0),
        window_bytes / 1024.0f / seconds,
        static_cast<unsigned long>(window_shown ? window_decode_ms /
                                                      window_shown
                                                : 0),
        static_cast<unsigned long>(window_shown ? window_draw_ms /
                                                      window_shown
                                                : 0),
        static_cast<unsigned>(buffer->buffered()),
        static_cast<unsigned long>(buffer->delay()),
        static_cast<unsigned long>(buffer->skipped()),
        static_cast<unsigned long>(buffer->overflows()),
        static_cast<unsigned long>(buffer->resyncs()),
        static_cast<unsigned long>(frames->incomplete_frames()),
        static_cast<unsigned long>(bad_frames),
        static_cast<unsigned long>(rejected_count),
        static_cast<unsigned long>(replay_count),
        static_cast<unsigned long>(foreign_count),
        static_cast<unsigned long>(video_radio::queue_drops()));
    window_shown = window_bytes = window_decode_ms = window_draw_ms = 0;
}

void print_lcd_settings()
{
    Serial.printf("LCD bytes %s | invert %s | SPI %lu MHz\n",
        big_endian ? "high first" : "low first", inverted ? "on" : "off",
        static_cast<unsigned long>(spi_hz / 1000000));
}

void role_command(int c)
{
    if (c == 'd')
    {
        static const uint32_t delays[] = {150, 300, 500, 800};
        size_t next = 0;
        for (size_t i = 0; i < 4; ++i)
        {
            if (delays[i] == buffer->delay())
            {
                next = (i + 1) % 4;
            }
        }
        buffer->set_delay(delays[next]);
        Serial.printf("Playback delay %lu ms\n",
            static_cast<unsigned long>(delays[next]));
    }
    else if (c == 'b')
    {
        big_endian = !big_endian;
        print_lcd_settings();
    }
    else if (c == 'i')
    {
        inverted = !inverted;
        lcd.invertDisplay(inverted);
        print_lcd_settings();
    }
    else if (c == 'f')
    {
        spi_hz = spi_hz >= 40000000 ? 20000000 :
            (spi_hz >= 20000000 ? 10000000 : 40000000);
        lcd.setSPISpeed(spi_hz);
        print_lcd_settings();
    }
    else if (c == 's')
    {
        Serial.printf("Session %08lX | peer %s | shown %lu\n",
            static_cast<unsigned long>(link.session_id()),
            peer_known ? "known" : "unknown",
            static_cast<unsigned long>(frames_shown));
    }
}
