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
video_link::receiver *plain_packets = nullptr;
video_link::receiver *sealed_packets = nullptr;
video_frame::assembler *older_packets = nullptr;

enum class packet_format
{
    unknown,
    live_plain,
    live_sealed,
    older_sealed,
};
packet_format format = packet_format::unknown;
uint16_t shown_width = 0;
uint16_t shown_height = 0;
uint16_t camera_width = 0;
uint16_t camera_height = 0;
bool showing = false;
bool shown_encrypted = false;
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

void boot_screen(const char *line, const char *detail = nullptr)
{
    showing = false;
    lcd.fillScreen(ST77XX_BLACK);
    lcd.setTextColor(ST77XX_WHITE);
    lcd.setTextSize(2);
    lcd.setCursor(8, 90);
    lcd.print(line);
    if (detail)
    {
        lcd.setTextSize(1);
        lcd.setCursor(8, 130);
        lcd.print(detail);
    }
}

void init_lcd()
{
    pinMode(app_config::lcd_backlight_pin, OUTPUT);
    digitalWrite(app_config::lcd_backlight_pin, HIGH);
    SPI.begin(app_config::lcd_sclk_pin, -1, app_config::lcd_mosi_pin,
        app_config::lcd_cs_pin);
    lcd.init(app_config::lcd_width, app_config::lcd_height, SPI_MODE3);
    lcd.setSPISpeed(spi_hz);
    lcd.setRotation(app_config::lcd_rotation);
    lcd.invertDisplay(inverted);
    print_lcd_settings();
    Serial.println("VIEWER ST7789 240x240");
    Serial.println("Commands: s status | b swap pixel bytes | i invert | "
                   "f SPI 40/20/10 MHz");
}

bool setup_display()
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
    static video_link::receiver plain(assembly, video_link::max_frame);
    static video_link::receiver sealed(assembly, video_link::max_frame,
        video_link::encrypted_data_max);
    static video_frame::assembler older(assembly, video_link::max_frame);
    plain_packets = &plain;
    sealed_packets = &sealed;
    older_packets = &older;
    return true;
}

void reset_video()
{
    format = packet_format::unknown;
    frame_ready = false;
    plain_packets->reset();
    sealed_packets->reset();
    older_packets->reset();
}

packet_format detect_video_format(const uint8_t *bytes, size_t size,
    bool encrypted)
{
    if (!encrypted)
    {
        return packet_format::live_plain;
    }
    // The encrypted variants use the same message type but different inner
    // headers. A first fragment starts with JPEG SOI after its own header.
    if (size >= video_link::header_size + 2 &&
        bytes[0] == video_link::packet_type && bytes[5] == 0 &&
        bytes[video_link::header_size] == 0xff &&
        bytes[video_link::header_size + 1] == 0xd8)
    {
        return packet_format::live_sealed;
    }
    if (size >= video_frame::chunk_header_size + 2 && bytes[2] == 0 &&
        bytes[video_frame::chunk_header_size] == 0xff &&
        bytes[video_frame::chunk_header_size + 1] == 0xd8)
    {
        return packet_format::older_sealed;
    }
    return packet_format::unknown;
}

void queue_frame(const uint8_t *data, size_t size)
{
    // Drawing runs in this same loop, so the other slot is always free.
    const size_t slot = 1 - ready_slot;
    memcpy(jpeg[slot], data, size);
    jpeg_size[slot] = size;
    if (frame_ready)
    {
        ++frames_skipped;  // Newer frame replaces one not yet drawn.
    }
    ready_slot = slot;
    frame_ready = true;
}

void accept_video_packet(const uint8_t *bytes, size_t size, bool encrypted)
{
    if (format == packet_format::unknown)
    {
        format = detect_video_format(bytes, size, encrypted);
    }
    if (format == packet_format::unknown ||
        encrypted != (format != packet_format::live_plain))
    {
        return;
    }
    if (format == packet_format::older_sealed)
    {
        if (older_packets->add(bytes, size) == video_frame::assembler::result::complete)
        {
            queue_frame(older_packets->frame(), older_packets->frame_size());
        }
        return;
    }
    auto &live = encrypted ? *sealed_packets : *plain_packets;
    if (live.add(bytes, size) == video_link::receiver::result::complete)
    {
        queue_frame(live.frame(), live.frame_size());
    }
}

bool draw_ready_frame()
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
        return false;
    }
    const uint32_t started_ms = millis();
    if (!jpg2rgb565(data, size, reinterpret_cast<uint8_t *>(decoded),
            JPG_SCALE_NONE))
    {
        ++bad_frames;
        return false;
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
    const bool encrypted = format == packet_format::live_sealed ||
        format == packet_format::older_sealed;
    const bool layout_changed = !showing || out_width != shown_width ||
        out_height != shown_height || encrypted != shown_encrypted;
    if (layout_changed)
    {
        lcd.fillScreen(ST77XX_BLACK);
        shown_width = out_width;
        shown_height = out_height;
        shown_encrypted = encrypted;
        showing = true;
    }
    const uint16_t image_y = (app_config::lcd_height - out_height) / 2;
    lcd.startWrite();
    lcd.setAddrWindow((app_config::lcd_width - out_width) / 2,
        image_y, out_width, out_height);
    lcd.writePixels(const_cast<uint16_t *>(image),
        static_cast<uint32_t>(out_width) * out_height, true, big_endian);
    lcd.endWrite();
    // Leave the label in the lower margin. Redraw it when a taller picture
    // covers that area or when the display layout changes.
    constexpr uint16_t label_y = app_config::lcd_height - 24;
    if (encrypted && (layout_changed || image_y + out_height > label_y))
    {
        lcd.setTextSize(2);
        lcd.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
        lcd.setCursor(8, label_y);
        lcd.print("Encrypted");
    }
    camera_width = width;
    camera_height = height;
    ++frames_shown;
    ++window_shown;
    window_bytes += size;
    window_decode_ms += decoded_ms - started_ms;
    window_draw_ms += millis() - decoded_ms;
    return true;
}

void print_stats(uint32_t elapsed_ms)
{
    const float seconds = elapsed_ms / 1000.0f;
    const uint32_t n = window_shown ? window_shown : 1;
    const uint32_t incomplete = plain_packets->incomplete_frames() +
        sealed_packets->incomplete_frames() + older_packets->incomplete_frames();
    Serial.printf("RX %.1f fps | camera %ux%u shown %ux%u | frame %lu B | "
                  "%.1f KB/s | decode+scale %lu ms draw %lu ms | skipped %lu"
                  " incomplete %lu bad %lu | queue drops "
                  "%lu\n",
        window_shown / seconds, camera_width, camera_height, shown_width,
        shown_height, static_cast<unsigned long>(window_bytes / n),
        window_bytes / 1024.0f / seconds,
        static_cast<unsigned long>(window_decode_ms / n),
        static_cast<unsigned long>(window_draw_ms / n),
        static_cast<unsigned long>(frames_skipped),
        static_cast<unsigned long>(incomplete),
        static_cast<unsigned long>(bad_frames),
        static_cast<unsigned long>(radio::queue_drops()));
    radio::print_status();
    window_shown = window_bytes = window_decode_ms = window_draw_ms = 0;
}

void handle_command(int c)
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
        uint32_t sequence = plain_packets->sequence();
        if (format == packet_format::live_sealed)
        {
            sequence = sealed_packets->sequence();
        }
        else if (format == packet_format::older_sealed)
        {
            sequence = older_packets->frame_id();
        }
        Serial.printf("Last sequence %lu | shown %lu\n",
            static_cast<unsigned long>(sequence),
            static_cast<unsigned long>(frames_shown));
    }
}
