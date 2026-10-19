#include "player.h"
#include "app_config.h"
#include "media_format.h"

#include <Arduino.h>
#include <SPI.h>
#include <SD_MMC.h>
#include <Adafruit_ST7789.h>
#include <TJpg_Decoder.h>
#include <driver/i2s.h>
#include <esp_heap_caps.h>
#include <atomic>
#include <memory>

namespace
{
constexpr size_t jpeg_capacity = 32768;
constexpr size_t audio_block_samples = 256;
constexpr size_t dma_buffers = 8;
constexpr size_t queued_samples = dma_buffers * audio_block_samples;
Adafruit_ST7789 lcd(&SPI, -1, app_config::lcd_dc, app_config::lcd_reset);
File video;
media_format::bundle_header bundle = {};
uint8_t jpeg[jpeg_capacity];
uint16_t width = 0;
uint16_t height = 0;
uint16_t fps = 0;
uint32_t frame_count = 0;
uint32_t frame_index = 0;
uint32_t frame_start = 0;
uint32_t maximum_jpeg = 0;
uint32_t decoded = 0;
uint32_t skipped = 0;
uint64_t video_cycle = 0;
bool driver_installed = false;
bool audio_task_running = false;
std::atomic<bool> stopping{false};
std::atomic<bool> paused{false};
std::atomic<bool> audio_failed{false};
portMUX_TYPE clock_lock = portMUX_INITIALIZER_UNLOCKED;
uint64_t presented_samples = 0;
StaticSemaphore_t done_storage;
SemaphoreHandle_t audio_done = nullptr;

struct psram_deleter
{
    void operator()(uint8_t *pointer) const
    {
        heap_caps_free(pointer);
    }
};
std::unique_ptr<uint8_t, psram_deleter> audio_data;

uint64_t audio_clock()
{
    portENTER_CRITICAL(&clock_lock);
    const uint64_t result = presented_samples;
    portEXIT_CRITICAL(&clock_lock);
    return result;
}

void publish_clock(uint64_t samples)
{
    portENTER_CRITICAL(&clock_lock);
    presented_samples = samples;
    portEXIT_CRITICAL(&clock_lock);
}

void audio_worker(void *)
{
    int16_t pcm[audio_block_samples];
    uint32_t input_offset = 0;
    uint64_t submitted = 0;
    while (!stopping.load())
    {
        if (paused.load())
        {
            if (i2s_stop(I2S_NUM_0) != ESP_OK)
            {
                audio_failed.store(true);
                break;
            }
            while (paused.load() && !stopping.load())
            {
                vTaskDelay(pdMS_TO_TICKS(5));
            }
            if (stopping.load())
            {
                break;
            }
            if (i2s_start(I2S_NUM_0) != ESP_OK)
            {
                audio_failed.store(true);
                break;
            }
        }
        for (size_t i = 0; i < audio_block_samples; ++i)
        {
            const int32_t sample = media_format::decode_mulaw(
                audio_data.get()[input_offset]);
            pcm[i] = static_cast<int16_t>(
                sample * app_config::audio_gain_percent / 100);
            if (++input_offset == bundle.audio_samples)
            {
                input_offset = 0;
            }
        }
        size_t written = 0;
        if (i2s_write(I2S_NUM_0, pcm, sizeof(pcm), &written,
                      pdMS_TO_TICKS(1000)) != ESP_OK || written != sizeof(pcm))
        {
            audio_failed.store(true);
            break;
        }
        submitted += audio_block_samples;
        // I2S write backpressure supplies the clock; account for queued DMA.
        publish_clock(submitted > queued_samples ?
                      submitted - queued_samples : 0);
    }
    if (xSemaphoreGive(audio_done) != pdTRUE)
    {
        audio_failed.store(true);
    }
    vTaskDelete(nullptr);
}

bool start_audio()
{
    i2s_config_t config = {};
    config.mode = static_cast<i2s_mode_t>(
        I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_PDM);
    config.sample_rate = bundle.audio_rate;
    config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    config.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
    config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    config.dma_buf_count = dma_buffers;
    config.dma_buf_len = audio_block_samples;
    config.tx_desc_auto_clear = true;
    if (i2s_driver_install(I2S_NUM_0, &config, 0, nullptr) != ESP_OK)
    {
        return false;
    }
    driver_installed = true;
    i2s_pin_config_t pins = {};
    pins.mck_io_num = I2S_PIN_NO_CHANGE;
    pins.bck_io_num = I2S_PIN_NO_CHANGE;
    pins.ws_io_num = I2S_PIN_NO_CHANGE;
    pins.data_in_num = I2S_PIN_NO_CHANGE;
    pins.data_out_num = app_config::speaker;
    if (i2s_set_pin(I2S_NUM_0, &pins) != ESP_OK ||
        i2s_zero_dma_buffer(I2S_NUM_0) != ESP_OK)
    {
        return false;
    }
    stopping.store(false);
    paused.store(false);
    audio_failed.store(false);
    publish_clock(0);
    if (xTaskCreatePinnedToCore(audio_worker, "audio", 4096, nullptr, 4,
                                nullptr, 0) != pdPASS)
    {
        return false;
    }
    audio_task_running = true;
    return true;
}

bool draw_block(int16_t x, int16_t y, uint16_t w, uint16_t h,
                uint16_t *pixels)
{
    if (x < 0 || y < 0 || x + w > lcd.width() || y + h > lcd.height())
    {
        return false;
    }
    lcd.setAddrWindow(x, y, w, h);
    lcd.writePixels(pixels, static_cast<uint32_t>(w) * h);
    return true;
}

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

void fail(const char *message)
{
    player::stop();
    player::message(message);
    Serial.printf("ERROR player: %s\n", message);
}
}

namespace player
{
void initialize()
{
    audio_done = xSemaphoreCreateBinaryStatic(&done_storage);
    digitalWrite(app_config::speaker, LOW);
    pinMode(app_config::speaker, OUTPUT);
    digitalWrite(app_config::lcd_backlight, HIGH);
    pinMode(app_config::lcd_backlight, OUTPUT);
    SPI.begin(app_config::lcd_sclk, -1, app_config::lcd_mosi, -1);
    lcd.init(240, 240, SPI_MODE3);
    lcd.setSPISpeed(app_config::lcd_spi_hz);
    lcd.setRotation(app_config::lcd_rotation);
    lcd.invertDisplay(app_config::lcd_inverted);
    TJpgDec.setJpgScale(1);
    TJpgDec.setSwapBytes(false);
    TJpgDec.setCallback(draw_block);
    message("Waiting for clip");
}

void message(const char *text)
{
    lcd.fillScreen(ST77XX_BLACK);
    lcd.setTextSize(2);
    lcd.setTextColor(ST77XX_WHITE);
    lcd.setCursor(8, 16);
    lcd.println(text);
}

void stop()
{
    if (audio_task_running)
    {
        stopping.store(true);
        if (xSemaphoreTake(audio_done, portMAX_DELAY) != pdTRUE)
        {
            Serial.println("ERROR audio task cleanup");
        }
        audio_task_running = false;
    }
    if (driver_installed)
    {
        if (i2s_driver_uninstall(I2S_NUM_0) != ESP_OK)
        {
            Serial.println("ERROR I2S cleanup");
        }
        driver_installed = false;
    }
    pinMode(app_config::speaker, OUTPUT);
    digitalWrite(app_config::speaker, LOW);
    video.close();
    audio_data.reset();
}

bool play(const char *path)
{
    stop();
    video = SD_MMC.open(path, FILE_READ);
    uint8_t header[32] = {};
    if (!video || video.read(header, 32) != 32 ||
        !media_format::parse_bundle(header, 32, video.size(), bundle) ||
        bundle.audio_samples > 4UL * 1024UL * 1024UL)
    {
        fail("Invalid AV file");
        return false;
    }
    audio_data.reset(static_cast<uint8_t *>(heap_caps_malloc(
        bundle.audio_samples, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
    if (!audio_data || !video.seek(bundle.audio_offset))
    {
        fail("PSRAM/SD error");
        return false;
    }
    message("Loading audio...");
    for (uint32_t offset = 0; offset < bundle.audio_samples;)
    {
        const size_t length = min(static_cast<size_t>(4096),
            static_cast<size_t>(bundle.audio_samples - offset));
        if (video.read(audio_data.get() + offset, length) != length)
        {
            fail("Audio read failed");
            return false;
        }
        offset += length;
        delay(1);
    }
    if (!video.seek(bundle.video_offset) || video.read(header, 24) != 24 ||
        memcmp(header, "LCDV0001", 8) != 0)
    {
        fail("Invalid video");
        return false;
    }
    width = media_format::read_u16(header + 8);
    height = media_format::read_u16(header + 10);
    fps = media_format::read_u16(header + 12);
    frame_count = media_format::read_u32(header + 16);
    maximum_jpeg = media_format::read_u32(header + 20);
    const uint64_t expected_samples = fps == 0 ? 0 :
        (static_cast<uint64_t>(frame_count) * bundle.audio_rate + fps / 2) /
        fps;
    if (width == 0 || width > 240 || height == 0 || height > 240 ||
        fps == 0 || fps > 30 || frame_count == 0 ||
        media_format::read_u16(header + 14) != 0 ||
        frame_count > (bundle.video_size - 24) / 8 ||
        maximum_jpeg < 4 || maximum_jpeg > jpeg_capacity ||
        expected_samples != bundle.audio_samples)
    {
        fail("Bad AV dimensions");
        return false;
    }
    frame_start = bundle.video_offset + 24;
    frame_index = 0;
    decoded = 0;
    skipped = 0;
    video_cycle = 0;
    lcd.fillScreen(ST77XX_BLACK);
    if (!start_audio())
    {
        fail("Audio start failed");
        return false;
    }
    Serial.printf("PLAYING AV %ux%u %u fps; audio %lu Hz on GPIO%d\n",
                  width, height, fps,
                  static_cast<unsigned long>(bundle.audio_rate),
                  app_config::speaker);
    return true;
}

void toggle_pause()
{
    if (video)
    {
        paused.store(!paused.load());
        Serial.println(paused.load() ? "PAUSED" : "RESUMED");
    }
}

void status()
{
    Serial.printf("PLAYER %s frame=%lu/%lu decoded=%lu skipped=%lu "
                  "loops=%llu audio_samples=%llu\n",
                  !video ? "stopped" : paused.load() ? "paused" : "playing",
                  static_cast<unsigned long>(frame_index),
                  static_cast<unsigned long>(frame_count),
                  static_cast<unsigned long>(decoded),
                  static_cast<unsigned long>(skipped), video_cycle,
                  audio_clock());
}

void tick()
{
    if (!video)
    {
        return;
    }
    if (audio_failed.load())
    {
        fail("Audio output failed");
        return;
    }
    if (paused.load())
    {
        return;
    }
    const uint64_t samples = audio_clock();
    const uint64_t cycle = samples / bundle.audio_samples;
    if (cycle != video_cycle)
    {
        if (!video.seek(frame_start))
        {
            fail("Video rewind failed");
            return;
        }
        video_cycle = cycle;
        frame_index = 0;
        Serial.printf("AV LOOP %llu\n", cycle);
    }
    const uint32_t target_frame = min(frame_count - 1,
        static_cast<uint32_t>((samples % bundle.audio_samples) * fps /
                              bundle.audio_rate));
    if (frame_index > target_frame)
    {
        return;
    }
    uint8_t length_bytes[4];
    if (video.read(length_bytes, 4) != 4)
    {
        fail("Truncated frame");
        return;
    }
    const uint32_t length = media_format::read_u32(length_bytes);
    if (length < 4 || length > maximum_jpeg ||
        length > video.size() - video.position())
    {
        fail("Bad frame size");
        return;
    }
    if (frame_index++ < target_frame)
    {
        if (!video.seek(video.position() + length))
        {
            fail("Frame skip failed");
            return;
        }
        ++skipped;
        return;
    }
    uint16_t jpeg_width = 0;
    uint16_t jpeg_height = 0;
    if (video.read(jpeg, length) != length ||
        TJpgDec.getJpgSize(&jpeg_width, &jpeg_height, jpeg, length) != JDR_OK ||
        jpeg_width != width || jpeg_height != height)
    {
        fail("JPEG read failed");
        return;
    }
    JRESULT result;
    {
        const lcd_transaction transaction;
        result = TJpgDec.drawJpg((240 - width) / 2, (240 - height) / 2,
                                jpeg, length);
    }
    if (result != JDR_OK)
    {
        fail("JPEG decode failed");
        return;
    }
    ++decoded;
}
}
