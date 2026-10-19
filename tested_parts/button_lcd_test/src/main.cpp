#include <Arduino.h>
#include <SPI.h>
#include <SD_MMC.h>
#include <Adafruit_ST7789.h>
#include <TJpg_Decoder.h>
#include <mbedtls/sha256.h>
#include <cstring>

#include "app_config.h"

namespace
{
constexpr char video_path[] = "/video.lcdv";
constexpr char staging_path[] = "/video.lcdv.part";
constexpr size_t header_size = 24;
constexpr size_t frame_capacity = 32768;
Adafruit_ST7789 lcd(&SPI, app_config::lcd_cs_pin,
                    app_config::lcd_dc_pin, app_config::lcd_reset_pin);
File video;
uint8_t frame[frame_capacity];
uint8_t transfer_buffer[4096];
bool mounted = false;
bool paused = false;
bool raw_pressed = false;
bool stable_pressed = false;
uint32_t raw_changed_ms = 0;
uint16_t video_width = 0;
uint16_t video_height = 0;
uint16_t video_fps = 0;
uint32_t frame_count = 0;
uint32_t max_frame = 0;
uint32_t frame_index = 0;
uint32_t frame_period_us = 0;
uint32_t next_frame_us = 0;
uint32_t decoded_frames = 0;
uint32_t skipped_frames = 0;
uint32_t completed_loops = 0;
uint32_t maximum_decode_us = 0;
char command[128] = {};
size_t command_length = 0;
bool command_overflow = false;

class sha256_context
{
public:
    sha256_context()
    {
        mbedtls_sha256_init(&context);
    }
    ~sha256_context()
    {
        mbedtls_sha256_free(&context);
    }
    sha256_context(const sha256_context &) = delete;
    sha256_context &operator=(const sha256_context &) = delete;
    mbedtls_sha256_context context;
};

class lcd_transaction
{
public:
    lcd_transaction()
    {
        lcd.startWrite();
    }
    ~lcd_transaction()
    {
        lcd.endWrite();
    }
    lcd_transaction(const lcd_transaction &) = delete;
    lcd_transaction &operator=(const lcd_transaction &) = delete;
};

uint16_t read_u16(const uint8_t *data)
{
    return static_cast<uint16_t>(data[0]) |
           (static_cast<uint16_t>(data[1]) << 8);
}

uint32_t read_u32(const uint8_t *data)
{
    return static_cast<uint32_t>(data[0]) |
           (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) |
           (static_cast<uint32_t>(data[3]) << 24);
}

void show_message(const char *message)
{
    lcd.fillScreen(ST77XX_BLACK);
    lcd.setTextSize(2);
    lcd.setTextColor(ST77XX_WHITE);
    lcd.setCursor(8, 16);
    lcd.println(message);
}

void fail(const char *message)
{
    video.close();
    Serial.printf("ERROR %s\n", message);
    show_message(message);
}

bool draw_block(int16_t x, int16_t y, uint16_t width, uint16_t height,
                uint16_t *pixels)
{
    if (x < 0 || y < 0 || x + width > lcd.width() ||
        y + height > lcd.height())
    {
        return false;
    }
    lcd.setAddrWindow(x, y, width, height);
    lcd.writePixels(pixels, static_cast<uint32_t>(width) * height);
    return true;
}

bool play_video()
{
    video.close();
    paused = false;
    video = SD_MMC.open(video_path, FILE_READ);
    uint8_t header[header_size] = {};
    if (!video || video.read(header, sizeof(header)) != sizeof(header) ||
        std::memcmp(header, "LCDV0001", 8) != 0)
    {
        fail("Missing/invalid video");
        return false;
    }
    video_width = read_u16(header + 8);
    video_height = read_u16(header + 10);
    video_fps = read_u16(header + 12);
    frame_count = read_u32(header + 16);
    max_frame = read_u32(header + 20);
    if (video_width == 0 || video_width > lcd.width() ||
        video_height == 0 || video_height > lcd.height() ||
        video_fps == 0 || video_fps > 30 || read_u16(header + 14) != 0 ||
        frame_count == 0 || frame_count > (video.size() - header_size) / 8 ||
        max_frame < 4 || max_frame > frame_capacity)
    {
        fail("Bad video header");
        return false;
    }
    frame_index = 0;
    decoded_frames = 0;
    skipped_frames = 0;
    completed_loops = 0;
    maximum_decode_us = 0;
    frame_period_us = 1000000UL / video_fps;
    lcd.fillScreen(ST77XX_BLACK);
    next_frame_us = micros();
    Serial.printf("PLAYING %ux%u %u fps %lu frames\n", video_width,
                  video_height, video_fps,
                  static_cast<unsigned long>(frame_count));
    return true;
}

void toggle_pause()
{
    if (video)
    {
        paused = !paused;
        next_frame_us = micros();
        Serial.println(paused ? "PAUSED" : "RESUMED");
    }
}

void print_status()
{
    Serial.printf("VIDEO %s frame=%lu/%lu decoded=%lu skipped=%lu "
                  "loops=%lu max_decode_us=%lu\n",
                  !video ? "stopped" : paused ? "paused" : "playing",
                  static_cast<unsigned long>(frame_index),
                  static_cast<unsigned long>(frame_count),
                  static_cast<unsigned long>(decoded_frames),
                  static_cast<unsigned long>(skipped_frames),
                  static_cast<unsigned long>(completed_loops),
                  static_cast<unsigned long>(maximum_decode_us));
}

void tick_video()
{
    if (!video || paused)
    {
        return;
    }
    const uint32_t now_us = micros();
    if (static_cast<int32_t>(now_us - next_frame_us) < 0)
    {
        return;
    }
    if (frame_index == frame_count)
    {
        if (video.position() != video.size() || !video.seek(header_size))
        {
            fail("Bad video end");
            return;
        }
        frame_index = 0;
        ++completed_loops;
        Serial.printf("LOOP %lu decoded=%lu skipped=%lu\n",
                      static_cast<unsigned long>(completed_loops),
                      static_cast<unsigned long>(decoded_frames),
                      static_cast<unsigned long>(skipped_frames));
    }
    uint8_t length_bytes[4] = {};
    if (video.read(length_bytes, 4) != 4)
    {
        fail("Truncated video");
        return;
    }
    const uint32_t length = read_u32(length_bytes);
    if (length < 4 || length > max_frame ||
        length > video.size() - video.position())
    {
        fail("Bad frame length");
        return;
    }
    // Skip overdue frames instead of accumulating playback delay.
    const bool skip = (now_us - next_frame_us) >= frame_period_us;
    next_frame_us += frame_period_us;
    ++frame_index;
    if (skip)
    {
        if (!video.seek(video.position() + length))
        {
            fail("SD seek failed");
            return;
        }
        ++skipped_frames;
        return;
    }
    const uint32_t started_us = micros();
    if (video.read(frame, length) != length)
    {
        fail("SD read failed");
        return;
    }
    uint16_t width = 0;
    uint16_t height = 0;
    if (TJpgDec.getJpgSize(&width, &height, frame, length) != JDR_OK ||
        width != video_width || height != video_height)
    {
        fail("Bad JPEG size");
        return;
    }
    JRESULT result = JDR_OK;
    {
        const lcd_transaction transaction;
        result = TJpgDec.drawJpg((lcd.width() - width) / 2,
                                (lcd.height() - height) / 2, frame, length);
    }
    if (result != JDR_OK)
    {
        fail("JPEG decode failed");
        return;
    }
    ++decoded_frames;
    const uint32_t elapsed_us = micros() - started_us;
    maximum_decode_us = max(maximum_decode_us, elapsed_us);
}

bool check_hash(const char *path, uint32_t size, const char *expected)
{
    File file = SD_MMC.open(path, FILE_READ);
    if (!file || file.size() != size)
    {
        return false;
    }
    sha256_context hash;
    if (mbedtls_sha256_starts_ret(&hash.context, 0) != 0)
    {
        return false;
    }
    uint32_t remaining = size;
    while (remaining != 0)
    {
        const size_t wanted = min(static_cast<size_t>(remaining),
                                  sizeof(transfer_buffer));
        if (file.read(transfer_buffer, wanted) != wanted ||
            mbedtls_sha256_update_ret(&hash.context, transfer_buffer,
                                     wanted) != 0)
        {
            return false;
        }
        remaining -= wanted;
        delay(1);
    }
    uint8_t digest[32] = {};
    if (mbedtls_sha256_finish_ret(&hash.context, digest) != 0)
    {
        return false;
    }
    constexpr char hex[] = "0123456789abcdef";
    char actual[65] = {};
    for (size_t i = 0; i < sizeof(digest); ++i)
    {
        actual[i * 2] = hex[digest[i] >> 4];
        actual[i * 2 + 1] = hex[digest[i] & 15];
    }
    return std::strcmp(actual, expected) == 0;
}

bool receive_video(uint32_t size, const char *expected_hash)
{
    if (size < 24 || size > 64UL * 1024UL * 1024UL ||
        std::strlen(expected_hash) != 64 ||
        std::strspn(expected_hash, "0123456789abcdef") != 64)
    {
        Serial.println("ERROR invalid upload");
        return false;
    }
    if (SD_MMC.exists(video_path))
    {
        const bool matches = check_hash(video_path, size, expected_hash);
        Serial.println(matches ? "EXISTS verified" :
                       "ERROR different video already exists; preserved");
        return matches;
    }
    File file = SD_MMC.open(staging_path, FILE_WRITE);
    if (!file)
    {
        Serial.println("ERROR cannot open staging file");
        return false;
    }
    Serial.println("READY");
    uint32_t received = 0;
    while (received < size)
    {
        const size_t wanted = min(static_cast<size_t>(size - received),
                                  sizeof(transfer_buffer));
        if (Serial.readBytes(transfer_buffer, wanted) != wanted ||
            file.write(transfer_buffer, wanted) != wanted)
        {
            Serial.println("ERROR transfer interrupted");
            return false;
        }
        received += wanted;
        Serial.printf("ACK %lu\n", static_cast<unsigned long>(received));
    }
    // These APIs return no status; verify by reopening and hashing.
    file.flush();
    file.close();
    if (!check_hash(staging_path, size, expected_hash))
    {
        Serial.println("ERROR SD read-back SHA256 mismatch");
        return false;
    }
    if (!SD_MMC.rename(staging_path, video_path))
    {
        Serial.println("ERROR cannot rename verified video");
        return false;
    }
    Serial.printf("SAVED %lu %s\n", static_cast<unsigned long>(size),
                  expected_hash);
    return true;
}

void handle_command()
{
    if (!mounted)
    {
        Serial.println("ERROR SD not mounted");
        return;
    }
    if (std::strcmp(command, "status") == 0)
    {
        Serial.println("SD READY CMD38 CLK39 D0=40");
        print_status();
    }
    else if (std::strcmp(command, "play") == 0)
    {
        if (!play_video())
        {
            return;
        }
    }
    else if (std::strcmp(command, "pause") == 0)
    {
        toggle_pause();
    }
    else if (std::strncmp(command, "put ", 4) == 0)
    {
        unsigned long size = 0;
        char hash[65] = {};
        char trailing = '\0';
        if (std::sscanf(command, "put %lu %64s %c", &size, hash,
                        &trailing) != 2)
        {
            Serial.println("ERROR expected put <bytes> <sha256>");
            return;
        }
        video.close();
        show_message("Copying video\nto SD card...");
        const bool saved = receive_video(size, hash);
        show_message(saved ? "Video saved\nReady to play" :
                             "Upload failed\nCheck USB/SD");
    }
    else
    {
        Serial.println("ERROR unknown command");
    }
}

void poll_serial()
{
    while (Serial.available() > 0)
    {
        const int value = Serial.read();
        if (value < 0)
        {
            break;
        }
        if (value == '\n')
        {
            command[command_length] = '\0';
            if (command_overflow)
            {
                Serial.println("ERROR command too long");
            }
            else if (command_length != 0)
            {
                handle_command();
            }
            command_length = 0;
            command_overflow = false;
        }
        else if (value != '\r' && !command_overflow)
        {
            if (command_length + 1 < sizeof(command))
            {
                command[command_length++] = static_cast<char>(value);
            }
            else
            {
                command_overflow = true;
            }
        }
    }
}

void poll_button()
{
    const bool sampled_pressed = digitalRead(app_config::button_pin) == LOW;
    const uint32_t now_ms = millis();
    if (sampled_pressed != raw_pressed)
    {
        raw_pressed = sampled_pressed;
        raw_changed_ms = now_ms;
    }
    if (raw_pressed != stable_pressed &&
        now_ms - raw_changed_ms >= app_config::debounce_ms)
    {
        stable_pressed = raw_pressed;
        digitalWrite(app_config::led_pin, stable_pressed ? HIGH : LOW);
        if (stable_pressed)
        {
            toggle_pause();
        }
    }
}
}

void setup()
{
    digitalWrite(app_config::led_pin, LOW);
    pinMode(app_config::led_pin, OUTPUT);
    pinMode(app_config::button_pin, INPUT_PULLUP);
    Serial.begin(app_config::serial_baud);
    Serial.setTimeout(10000);
    SPI.begin(app_config::lcd_sclk_pin, -1, app_config::lcd_mosi_pin,
              app_config::lcd_cs_pin);
    lcd.init(app_config::lcd_width, app_config::lcd_height, SPI_MODE3);
    lcd.setSPISpeed(app_config::lcd_spi_hz);
    lcd.setRotation(app_config::lcd_rotation);
    lcd.invertDisplay(app_config::lcd_inverted);
    TJpgDec.setJpgScale(1);
    TJpgDec.setSwapBytes(false);
    TJpgDec.setCallback(draw_block);
    show_message("Opening SD...");
    mounted = SD_MMC.setPins(app_config::sd_clk_pin, app_config::sd_cmd_pin,
                              app_config::sd_data_pin) &&
              SD_MMC.begin("/sdcard", true, false,
                           app_config::sd_frequency_khz);
    if (!mounted)
    {
        show_message("SD mount failed\nInsert FAT32 card");
        Serial.println("ERROR SD mount");
    }
    else if (SD_MMC.exists(video_path))
    {
        if (!play_video())
        {
            Serial.println("ERROR startup playback");
        }
    }
    else
    {
        show_message("Upload video\nover USB");
    }
    raw_pressed = digitalRead(app_config::button_pin) == LOW;
    raw_changed_ms = millis();
}

void loop()
{
    poll_serial();
    poll_button();
    tick_video();
    delay(1);
}
