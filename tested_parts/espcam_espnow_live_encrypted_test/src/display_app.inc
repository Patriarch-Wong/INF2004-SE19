// Board 2: ST7789 240 x 240 display. Included by main.cpp inside its
// namespace.

Adafruit_ST7789 lcd(&SPI, app_config::lcd_cs_pin, app_config::lcd_dc_pin,
    app_config::lcd_reset_pin);

constexpr size_t screen_pixels =
    static_cast<size_t>(app_config::lcd_width) * app_config::lcd_height;

uint8_t *assembly = nullptr;       // Packets reassembled here, PSRAM.
uint8_t *jpeg[2] = {};             // Complete frames: newest ready + spare.
size_t jpeg_size[2] = {};
size_t ready_slot = 0;
bool frame_ready = false;
uint16_t *decoded = nullptr;       // RGB565 at camera size, PSRAM.
uint16_t *scaled = nullptr;        // RGB565 at screen size, PSRAM.
video_link::receiver *packets = nullptr;
uint32_t last_status_ms = 0;
uint16_t shown_width = 0;
uint16_t shown_height = 0;
uint16_t camera_width = 0;
uint16_t camera_height = 0;
bool showing = false;
bool big_endian = app_config::pixels_big_endian;
bool inverted = app_config::lcd_inverted;
uint32_t spi_hz = app_config::lcd_spi_hz;

// Totals.
uint32_t frames_shown = 0;
uint32_t bad_frames = 0;     // Complete, but not a JPEG we can show.
uint32_t frames_skipped = 0; // Complete but replaced before drawing.

// Stats since the last line.
uint32_t window_shown = 0;
uint32_t window_bytes = 0;
uint32_t window_decode_ms = 0;
uint32_t window_draw_ms = 0;

void print_lcd_settings()
{
    Serial.printf("LCD bytes %s | invert %s | SPI %lu MHz\n",
        big_endian ? "high first" : "low first", inverted ? "on" : "off",
        static_cast<unsigned long>(spi_hz / 1000000));
}

void boot_screen(const char *line)
{
    lcd.fillScreen(ST77XX_BLACK);
    lcd.setTextColor(ST77XX_WHITE);
    lcd.setTextSize(2);
    lcd.setCursor(8, 90);
    lcd.print(line);
    lcd.setTextSize(1);
    lcd.setCursor(8, 130);
    const uint8_t *mac = radio::own_mac();
    lcd.printf("Board 2  %02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1],
        mac[2], mac[3], mac[4], mac[5]);
}

bool role_setup()
{
    assembly = static_cast<uint8_t *>(ps_malloc(video_link::max_frame));
    jpeg[0] = static_cast<uint8_t *>(ps_malloc(video_link::max_frame));
    jpeg[1] = static_cast<uint8_t *>(ps_malloc(video_link::max_frame));
    decoded = static_cast<uint16_t *>(ps_malloc(screen_pixels * 2));
    scaled = static_cast<uint16_t *>(ps_malloc(screen_pixels * 2));
    if (!assembly || !jpeg[0] || !jpeg[1] || !decoded || !scaled)
    {
        return fail("PSRAM buffers");
    }
    static video_link::receiver receiver(assembly, video_link::max_frame);
    packets = &receiver;

    // Set the level before enabling the output so BLK never glitches low.
    digitalWrite(app_config::lcd_backlight_pin, HIGH);
    pinMode(app_config::lcd_backlight_pin, OUTPUT);
    SPI.begin(app_config::lcd_sclk_pin, -1, app_config::lcd_mosi_pin,
        app_config::lcd_cs_pin);
    lcd.init(app_config::lcd_width, app_config::lcd_height, SPI_MODE3);
    lcd.setSPISpeed(spi_hz);
    lcd.setRotation(app_config::lcd_rotation);
    lcd.invertDisplay(inverted);
    boot_screen("Waiting for camera");
    print_lcd_settings();
    Serial.println("DISPLAY ST7789 240x240 | waiting for board 1");
    Serial.println("Commands: s status | b swap pixel bytes | i invert | "
                   "f SPI 40/20/10 MHz");
    return true;
}

void role_session_event(session::event result,
    const control_frame::message &response, uint32_t now)
{
    (void)now;
    if (result == session::event::replied)
    {
        send_control(peer_mac, response);
    }
}

// New session: the camera restarted or re-keyed, and its sequence starts
// again at 1.
void role_new_session()
{
    packets->reset();
    frame_ready = false;
}

void role_data(uint8_t type, const uint8_t *payload, size_t size,
    uint32_t now)
{
    (void)now;
    if (type != video_link::sealed_video)
    {
        return;
    }
    if (packets->add(payload, size) !=
        video_link::receiver::result::complete)
    {
        return;
    }
    // Drawing runs in this same loop, so the other slot is always free.
    const size_t slot = 1 - ready_slot;
    memcpy(jpeg[slot], packets->frame(), packets->frame_size());
    jpeg_size[slot] = packets->frame_size();
    if (frame_ready)
    {
        ++frames_skipped;  // Newer frame replaces one not yet drawn.
    }
    ready_slot = slot;
    frame_ready = true;
}

void draw_ready_frame()
{
    const uint8_t *data = jpeg[ready_slot];
    const size_t size = jpeg_size[ready_slot];
    frame_ready = false;
    uint16_t width = 0;
    uint16_t height = 0;
    if (!video_link::jpeg_dimensions(data, size, &width, &height) ||
        width == 0 || height == 0 || width > app_config::lcd_width ||
        height > app_config::lcd_height)
    {
        ++bad_frames;
        return;
    }
    const uint32_t started_ms = millis();
    if (!jpg2rgb565(data, size, reinterpret_cast<uint8_t *>(decoded),
            JPG_SCALE_NONE))
    {
        ++bad_frames;
        return;
    }
    uint16_t out_width = width;
    uint16_t out_height = height;
    const uint16_t *image = decoded;
    if (width != app_config::lcd_width && height != app_config::lcd_height)
    {
        video_link::scale_to_fit(decoded, width, height,
            app_config::lcd_width, app_config::lcd_height, scaled,
            &out_width, &out_height);
        image = scaled;
    }
    const uint32_t decoded_ms = millis();
    if (!showing || out_width != shown_width || out_height != shown_height)
    {
        lcd.fillScreen(ST77XX_BLACK);
        shown_width = out_width;
        shown_height = out_height;
        showing = true;
    }
    lcd.startWrite();
    lcd.setAddrWindow((app_config::lcd_width - out_width) / 2,
        (app_config::lcd_height - out_height) / 2, out_width, out_height);
    lcd.writePixels(const_cast<uint16_t *>(image),
        static_cast<uint32_t>(out_width) * out_height, true, big_endian);
    lcd.endWrite();
    camera_width = width;
    camera_height = height;
    ++frames_shown;
    ++window_shown;
    window_bytes += size;
    window_decode_ms += decoded_ms - started_ms;
    window_draw_ms += millis() - decoded_ms;
}

void role_loop(uint32_t now)
{
    if (frame_ready)
    {
        draw_ready_frame();
    }
    if (link.established() &&
        now - last_status_ms >= app_config::status_interval_ms)
    {
        last_status_ms = now;
        uint8_t status[video_link::status_size];
        status[0] = video_link::status_type;
        put_u32(status + 1, frames_shown);
        put_u32(status + 5, packets->incomplete_frames());
        put_u32(status + 9, rejected_count);
        send_sealed(video_link::sealed_status, status, sizeof(status));
    }
    if (!frame_ready)
    {
        delay(1);
    }
}

void role_stats(uint32_t elapsed_ms)
{
    const float seconds = elapsed_ms / 1000.0f;
    const uint32_t n = window_shown ? window_shown : 1;
    Serial.printf("RX %.1f fps | camera %ux%u shown %ux%u | frame %lu B | "
                  "%.1f KB/s | open %lu us/packet | decode+scale %lu ms draw"
                  " %lu ms | skipped %lu incomplete %lu bad %lu | rejected "
                  "%lu (replay %lu) | other frames %lu | queue drops %lu\n",
        window_shown / seconds, camera_width, camera_height, shown_width,
        shown_height, static_cast<unsigned long>(window_bytes / n),
        window_bytes / 1024.0f / seconds,
        static_cast<unsigned long>(seal_count ? seal_total_us / seal_count
                                              : 0),
        static_cast<unsigned long>(window_decode_ms / n),
        static_cast<unsigned long>(window_draw_ms / n),
        static_cast<unsigned long>(frames_skipped),
        static_cast<unsigned long>(packets->incomplete_frames()),
        static_cast<unsigned long>(bad_frames),
        static_cast<unsigned long>(rejected_count),
        static_cast<unsigned long>(replay_count),
        static_cast<unsigned long>(foreign_count),
        static_cast<unsigned long>(radio::queue_drops()));
    window_shown = window_bytes = window_decode_ms = window_draw_ms = 0;
}

void role_command(int c)
{
    if (c == 'b')
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
        Serial.printf("Session %08lX | last sequence %lu | shown %lu\n",
            static_cast<unsigned long>(link.session_id()),
            static_cast<unsigned long>(packets->sequence()),
            static_cast<unsigned long>(frames_shown));
    }
}
