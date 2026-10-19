#include "player.h"
#include "app_config.h"
#include "ram_clip.h"
#include "esp_h264_dec_sw.h"
#include "esp_aac_dec.h"
#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_ST7789.h>
#include <driver/i2s.h>
#include <esp_heap_caps.h>
#include <atomic>
#include <algorithm>
#include <cstring>
#include <memory>

namespace
{
constexpr uint32_t block_samples = 256;
constexpr uint32_t queued_samples = 8 * block_samples;
Adafruit_ST7789 lcd(&SPI, -1, app_config::lcd_dc, app_config::lcd_reset);
esp_h264_dec_handle_t video_decoder = nullptr;
esp_h264_dec_param_sw_handle_t video_parameters = nullptr;
void *audio_decoder = nullptr;
struct psram_deleter
{
    void operator()(uint8_t *pointer) const
    {
        heap_caps_free(pointer);
    }
};
std::unique_ptr<uint8_t, psram_deleter> video_input;
std::unique_ptr<uint8_t, psram_deleter> audio_input;
std::unique_ptr<uint8_t, psram_deleter> audio_output;
constexpr uint32_t audio_input_capacity = 8192;
constexpr uint32_t audio_output_limit = 8192;
uint32_t audio_output_capacity = 0;
StaticSemaphore_t done_storage;
SemaphoreHandle_t done = nullptr;
bool driver_installed = false;
bool task_running = false;
bool active = false;
bool armed = false;
bool shown_buffering = false;
uint32_t video_cursor = stream_format::header_size;
uint32_t decoded = 0;
uint32_t skipped = 0;
uint32_t loops = 0;
bool have_video = false;
stream_format::record pending_video;
std::atomic<bool> stopping{false};
std::atomic<bool> paused{false};
std::atomic<bool> buffering{true};
std::atomic<bool> audio_failed{false};
std::atomic<bool> audio_ended{false};
std::atomic<uint32_t> clock_samples{0};

bool audio_gate(bool &driver_stopped)
{
    while (!stopping.load() && (paused.load() || buffering.load()))
    {
        if (!driver_stopped)
        {
            const esp_err_t result = i2s_stop(I2S_NUM_0);
            if (result != ESP_OK)
            {
                Serial.printf("ERROR PDM stop: %s\n", esp_err_to_name(result));
                return false;
            }
            driver_stopped = true;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (stopping.load())
    {
        return false;
    }
    if (driver_stopped)
    {
        const esp_err_t result = i2s_start(I2S_NUM_0);
        if (result != ESP_OK)
        {
            Serial.printf("ERROR PDM resume: %s\n", esp_err_to_name(result));
            return false;
        }
        driver_stopped = false;
    }
    return true;
}

bool write_pcm(const int16_t *pcm, uint32_t count, uint32_t &submitted,
               bool &driver_stopped)
{
    if (!audio_gate(driver_stopped))
    {
        return false;
    }
    size_t written = 0;
    const size_t length = count * sizeof(int16_t);
    const esp_err_t result = i2s_write(I2S_NUM_0, pcm, length, &written,
                                      pdMS_TO_TICKS(1000));
    if (result != ESP_OK || written != length)
    {
        Serial.printf("ERROR PDM write: %s bytes=%u/%u sample=%lu\n",
            esp_err_to_name(result), unsigned(written), unsigned(length),
            static_cast<unsigned long>(submitted));
        return false;
    }
    submitted += count;
    clock_samples.store(std::min(ram_clip::info().samples,
        submitted > queued_samples ? submitted - queued_samples : 0));
    return true;
}

bool resize_audio_output(uint32_t needed)
{
    if (!needed || needed > audio_output_limit)
    {
        Serial.printf("ERROR AAC output buffer request %lu exceeds limit\n",
                      static_cast<unsigned long>(needed));
        return false;
    }
    std::unique_ptr<uint8_t, psram_deleter> replacement(
        static_cast<uint8_t *>(heap_caps_malloc(needed,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
    if (!replacement)
    {
        Serial.println("ERROR AAC output PSRAM allocation");
        return false;
    }
    audio_output.swap(replacement);
    audio_output_capacity = needed;
    return true;
}

bool decode_audio_record(const stream_format::record &r, uint32_t &skip,
                         uint32_t &submitted, bool &driver_stopped)
{
    if (r.size > audio_input_capacity)
    {
        Serial.println("ERROR AAC input exceeds ADTS frame limit");
        return false;
    }
    std::memcpy(audio_input.get(), r.data, r.size);
    uint32_t input_offset = 0;
    while (input_offset < r.size && !stopping.load())
    {
        esp_audio_dec_in_raw_t raw = {};
        raw.buffer = audio_input.get() + input_offset;
        raw.len = r.size - input_offset;
        esp_audio_dec_out_frame_t output = {};
        output.buffer = audio_output.get();
        output.len = audio_output_capacity;
        esp_audio_dec_info_t info = {};
        const esp_audio_err_t result = esp_aac_dec_decode(audio_decoder,
            &raw, &output, &info);
        if (result == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH)
        {
            Serial.printf("AAC output buffer: %lu -> %lu bytes\n",
                static_cast<unsigned long>(audio_output_capacity),
                static_cast<unsigned long>(output.needed_size));
            // Retry the same input, as required by the codec API.
            if (output.needed_size <= audio_output_capacity ||
                !resize_audio_output(output.needed_size))
            {
                Serial.println("ERROR AAC cannot resize/retry output");
                return false;
            }
            continue;
        }
        if (result != ESP_AUDIO_ERR_OK || !raw.consumed ||
            raw.consumed > raw.len ||
            output.decoded_size > audio_output_capacity ||
            output.decoded_size % sizeof(int16_t))
        {
            Serial.printf("ERROR AAC decode: code=%d consumed=%lu/%lu "
                          "pcm=%lu needed=%lu pts=%lu\n", int(result),
                static_cast<unsigned long>(raw.consumed),
                static_cast<unsigned long>(raw.len),
                static_cast<unsigned long>(output.decoded_size),
                static_cast<unsigned long>(output.needed_size),
                static_cast<unsigned long>(r.sample));
            return false;
        }
        input_offset += raw.consumed;
        if (!output.decoded_size)
        {
            // Header/initialization consumption can produce no PCM yet.
            continue;
        }
        if (info.sample_rate != ram_clip::info().rate ||
            info.channel != 1 || info.bits_per_sample != 16)
        {
            Serial.printf("ERROR AAC format: rate=%lu channels=%u bits=%u\n",
                static_cast<unsigned long>(info.sample_rate),
                unsigned(info.channel), unsigned(info.bits_per_sample));
            return false;
        }
        int16_t *pcm = reinterpret_cast<int16_t *>(audio_output.get());
        const uint32_t samples = output.decoded_size / sizeof(int16_t);
        const uint32_t discard = std::min(skip, samples);
        skip -= discard;
        uint32_t offset = discard;
        while (offset < samples && submitted < ram_clip::info().samples &&
               !stopping.load())
        {
            const uint32_t count = std::min(block_samples,
                std::min(samples - offset,
                         ram_clip::info().samples - submitted));
            for (uint32_t i = offset; i < offset + count; ++i)
            {
                pcm[i] = int32_t(pcm[i]) * app_config::audio_gain_percent /
                         100;
            }
            if (!write_pcm(pcm + offset, count, submitted, driver_stopped))
            {
                return stopping.load();
            }
            offset += count;
        }
    }
    return true;
}

void audio_worker(void *)
{
    uint32_t cursor = stream_format::header_size;
    uint32_t skip = ram_clip::info().skip_samples;
    uint32_t submitted = 0;
    bool driver_stopped = false;
    bool okay = true;
    while (!stopping.load() && submitted < ram_clip::info().samples)
    {
        if (!audio_gate(driver_stopped))
        {
            okay = stopping.load();
            break;
        }
        stream_format::record r;
        if (!ram_clip::next(cursor, stream_format::audio_kind, r))
        {
            if (ram_clip::complete())
            {
                Serial.printf("ERROR AAC ended early: samples=%lu/%lu\n",
                    static_cast<unsigned long>(submitted),
                    static_cast<unsigned long>(ram_clip::info().samples));
                okay = false;
                break;
            }
            buffering.store(true);
            continue;
        }
        okay = decode_audio_record(r, skip, submitted, driver_stopped);
        if (!okay)
        {
            break;
        }
    }
    // Drain real samples through the DMA pipeline using trailing silence.
    int16_t silence[block_samples] = {};
    for (uint32_t i = 0; okay && !stopping.load() && i < queued_samples;
         i += block_samples)
    {
        okay = write_pcm(silence, block_samples, submitted, driver_stopped);
    }
    if (!stopping.load())
    {
        audio_failed.store(!okay);
        audio_ended.store(okay);
    }
    if (xSemaphoreGive(done) != pdTRUE)
    {
        audio_failed.store(true);
    }
    vTaskDelete(nullptr);
}

bool start_audio()
{
    audio_input.reset(static_cast<uint8_t *>(heap_caps_malloc(
        audio_input_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
    esp_aac_dec_cfg_t aac_config = {};
    aac_config.sample_rate = ram_clip::info().rate;
    aac_config.channel = 1;
    aac_config.bits_per_sample = 16;
    aac_config.no_adts_header = false;
    aac_config.aac_plus_enable = false;
    if (!audio_input || !resize_audio_output(2048) ||
        esp_aac_dec_open(&aac_config, sizeof(aac_config), &audio_decoder) !=
            ESP_AUDIO_ERR_OK)
    {
        return false;
    }
    i2s_config_t config = {};
    config.mode = static_cast<i2s_mode_t>(
        I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_PDM);
    config.sample_rate = ram_clip::info().rate;
    config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    config.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
    config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    config.dma_buf_count = 8;
    config.dma_buf_len = block_samples;
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
    buffering.store(false);
    audio_failed.store(false);
    audio_ended.store(false);
    clock_samples.store(0);
    if (xTaskCreatePinnedToCore(audio_worker, "aac", 20480, nullptr, 4,
                                nullptr, 0) != pdPASS)
    {
        return false;
    }
    task_running = true;
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

uint8_t clamp_color(int value)
{
    return static_cast<uint8_t>(std::max(0, std::min(255, value)));
}

bool draw_frame(const esp_h264_dec_out_frame_t &out)
{
    esp_h264_resolution_t resolution = {};
    if (esp_h264_dec_get_resolution(video_parameters, &resolution) !=
        ESP_H264_ERR_OK)
    {
        return false;
    }
    const uint32_t width = resolution.width;
    const uint32_t height = resolution.height;
    // Some decoders expose the padded macroblock height, others crop it.
    if (width != 240 || (height != 136 && height != 144) || !out.outbuf ||
        out.out_size != width * height * 3 / 2)
    {
        return false;
    }
    const uint8_t *luma = out.outbuf;
    const uint8_t *u = luma + width * height;
    const uint8_t *v = u + width * height / 4;
    uint16_t row[240];
    const lcd_transaction transaction;
    lcd.setAddrWindow(0, (240 - 136) / 2, 240, 136);
    for (uint32_t y = 0; y < 136; ++y)
    {
        for (uint32_t x = 0; x < 240; ++x)
        {
            const int c = int(luma[y * width + x]) - 16;
            const int d = int(u[(y / 2) * (width / 2) + x / 2]) - 128;
            const int e = int(v[(y / 2) * (width / 2) + x / 2]) - 128;
            const uint8_t red = clamp_color((298 * c + 409 * e + 128) >> 8);
            const uint8_t green = clamp_color(
                (298 * c - 100 * d - 208 * e + 128) >> 8);
            const uint8_t blue = clamp_color((298 * c + 516 * d + 128) >> 8);
            row[x] = ((red & 248) << 8) | ((green & 252) << 3) | (blue >> 3);
        }
        lcd.writePixels(row, 240);
    }
    return true;
}

bool decode_video(const stream_format::record &r)
{
    // Decoder APIs accept writable input; protect the retained stream.
    std::memcpy(video_input.get(), r.data, r.size);
    std::memset(video_input.get() + r.size, 0, 64);
    uint32_t offset = 0;
    uint32_t pictures = 0;
    uint32_t stalled = 0;
    while (offset < r.size)
    {
        esp_h264_dec_in_frame_t input = {};
        input.raw_data.buffer = video_input.get() + offset;
        input.raw_data.len = r.size - offset;
        input.pts = r.sample;
        input.dts = r.sample;
        esp_h264_dec_out_frame_t output = {};
        if (esp_h264_dec_process(video_decoder, &input, &output) !=
                ESP_H264_ERR_OK ||
            input.consume > r.size - offset)
        {
            return false;
        }
        if (!input.consume && ++stalled > 4)
        {
            return false;
        }
        if (input.consume)
        {
            stalled = 0;
        }
        offset += input.consume;
        if (output.out_size)
        {
            ++pictures;
            ++decoded;
            // Decode every P-frame to retain references, even when late.
            if (clock_samples.load() > r.sample +
                    ram_clip::info().rate / ram_clip::info().fps &&
                decoded < ram_clip::info().frames)
            {
                ++skipped;
            }
            else if (!draw_frame(output))
            {
                return false;
            }
        }
    }
    return pictures == 1;
}

void fail(const char *reason)
{
    player::stop();
    player::message(reason);
    Serial.printf("ERROR player: %s\n", reason);
}
}

namespace player
{
void initialize()
{
    done = xSemaphoreCreateBinaryStatic(&done_storage);
    digitalWrite(app_config::speaker, LOW);
    pinMode(app_config::speaker, OUTPUT);
    digitalWrite(app_config::lcd_backlight, HIGH);
    pinMode(app_config::lcd_backlight, OUTPUT);
    SPI.begin(app_config::lcd_sclk, -1, app_config::lcd_mosi, -1);
    lcd.init(240, 240, SPI_MODE3);
    lcd.setSPISpeed(app_config::lcd_spi_hz);
    lcd.setRotation(app_config::lcd_rotation);
    lcd.invertDisplay(app_config::lcd_inverted);
    message("Waiting for stream");
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
    armed = false;
    active = false;
    if (task_running)
    {
        stopping.store(true);
        if (xSemaphoreTake(done, portMAX_DELAY) != pdTRUE)
        {
            Serial.println("ERROR audio task cleanup");
        }
        task_running = false;
    }
    if (driver_installed)
    {
        if (i2s_driver_uninstall(I2S_NUM_0) != ESP_OK)
        {
            Serial.println("ERROR I2S cleanup");
        }
        driver_installed = false;
    }
    if (audio_decoder)
    {
        if (esp_aac_dec_close(audio_decoder) != ESP_AUDIO_ERR_OK)
        {
            Serial.println("ERROR AAC cleanup");
        }
        audio_decoder = nullptr;
    }
    if (video_decoder)
    {
        const auto closed = esp_h264_dec_close(video_decoder);
        const auto deleted = esp_h264_dec_del(video_decoder);
        if (closed != ESP_H264_ERR_OK || deleted != ESP_H264_ERR_OK)
        {
            Serial.println("ERROR H264 cleanup");
        }
        video_decoder = nullptr;
        video_parameters = nullptr;
    }
    video_input.reset();
    audio_input.reset();
    audio_output.reset();
    audio_output_capacity = 0;
    digitalWrite(app_config::speaker, LOW);
    pinMode(app_config::speaker, OUTPUT);
}

void arm()
{
    armed = true;
    loops = 0;
    message("Buffering stream...");
}

bool play()
{
    if (!ram_clip::has_header() || (!ram_clip::complete() &&
        ram_clip::available_samples() < app_config::buffer_samples))
    {
        Serial.println("ERROR not enough stream buffered yet");
        return false;
    }
    stop();
    if (!done)
    {
        fail("Audio semaphore failed");
        return false;
    }
    esp_h264_dec_cfg_sw_t config = {};
    config.pic_type = ESP_H264_RAW_FMT_I420;
    video_input.reset(static_cast<uint8_t *>(heap_caps_malloc(
        ram_clip::info().maximum_record + 64,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
    if (!video_input ||
        esp_h264_dec_sw_new(&config, &video_decoder) != ESP_H264_ERR_OK ||
        esp_h264_dec_open(video_decoder) != ESP_H264_ERR_OK ||
        esp_h264_dec_sw_get_param_hd(video_decoder, &video_parameters) !=
            ESP_H264_ERR_OK)
    {
        fail("H264 init failed");
        return false;
    }
    video_cursor = stream_format::header_size;
    have_video = false;
    decoded = 0;
    skipped = 0;
    shown_buffering = false;
    lcd.fillScreen(ST77XX_BLACK);
    if (!start_audio())
    {
        fail("AAC/PDM init failed");
        return false;
    }
    active = true;
    Serial.printf("PLAYING RAM H264/AAC 240x136 12 fps; received=%lu/%lu\n",
                  static_cast<unsigned long>(ram_clip::received()),
                  static_cast<unsigned long>(ram_clip::info().size));
    return true;
}

void toggle_pause()
{
    if (active)
    {
        paused.store(!paused.load());
        Serial.println(paused.load() ? "PAUSED" : "RESUMED");
    }
}

void tick()
{
    if (armed && ram_clip::has_header() && (ram_clip::complete() ||
        ram_clip::available_samples() >= app_config::buffer_samples))
    {
        if (!play())
        {
            return;
        }
    }
    if (!active)
    {
        return;
    }
    if (audio_failed.load())
    {
        fail("AAC/PDM playback failed");
        return;
    }
    if (paused.load())
    {
        return;
    }
    if (buffering.load())
    {
        const uint32_t now = clock_samples.load();
        const uint32_t available = ram_clip::available_samples();
        if (!ram_clip::complete() &&
            (available < now || available - now < app_config::buffer_samples))
        {
            if (!shown_buffering)
            {
                Serial.println("BUFFERING; holding video and audio");
                shown_buffering = true;
            }
            return;
        }
        buffering.store(false);
        shown_buffering = false;
        Serial.println("STREAM RESUMED");
    }
    if (!have_video && decoded < ram_clip::info().frames)
    {
        have_video = ram_clip::next(video_cursor, stream_format::video_kind,
                                    pending_video);
        if (!have_video)
        {
            if (ram_clip::complete())
            {
                fail("Missing video frame");
            }
            else
            {
                buffering.store(true);
            }
            return;
        }
    }
    if (have_video && pending_video.sample <= clock_samples.load())
    {
        if (!decode_video(pending_video))
        {
            fail("H264 decode failed");
            return;
        }
        have_video = false;
    }
    if (audio_ended.load() && decoded == ram_clip::info().frames &&
        ram_clip::complete())
    {
        ++loops;
        Serial.printf("RAM LOOP %lu\n", static_cast<unsigned long>(loops));
        if (!play())
        {
            return;
        }
    }
}

void status()
{
    Serial.printf("PLAYER %s decoded=%lu skipped=%lu sample=%lu "
                  "buffered_until=%lu verified=%u loops=%lu\n",
        !active ? (armed ? "buffering" : "stopped") :
        paused.load() ? "paused" : buffering.load() ? "buffering" : "playing",
        static_cast<unsigned long>(decoded),
        static_cast<unsigned long>(skipped),
        static_cast<unsigned long>(clock_samples.load()),
        static_cast<unsigned long>(ram_clip::available_samples()),
        ram_clip::complete(), static_cast<unsigned long>(loops));
}
}
