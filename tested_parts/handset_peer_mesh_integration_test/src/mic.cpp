#include "mic.h"
#include <driver/adc.h>
#include <esp_adc_cal.h>
#include <esp_timer.h>
#include <freertos/task.h>
#include <atomic>
#include <math.h>
#include <string.h>
#include "app_config.h"
#include "audio_dsp.h"

namespace mic
{
namespace
{
constexpr size_t result_bytes = sizeof(adc_digi_output_data_t);
constexpr size_t block_bytes = app_config::adc_block_samples * result_bytes;
constexpr float raw_to_16_bit = 16.0f;  // 12-bit counts to 16-bit scale.
constexpr uint16_t raw_full_scale = 4095;

QueueHandle_t frame_queue = nullptr;
std::atomic<bool> capturing{false};
std::atomic<float> gain_value{app_config::mic_default_gain};
std::atomic<float> ratio_value{app_config::mic_default_gate_ratio};
std::atomic<float> latest_rms{0};
std::atomic<float> latest_peak{0};
std::atomic<int64_t> last_clip_us{-10000000};
std::atomic<int16_t *> record_buffer{nullptr};
std::atomic<uint32_t> record_wanted{0};
std::atomic<uint32_t> record_count{0};

portMUX_TYPE second_lock = portMUX_INITIALIZER_UNLOCKED;
second finished = {};
bool finished_ready = false;

esp_adc_cal_characteristics_t calibration = {};

// Per-second accumulators, owned by the task.
struct accumulator
{
    uint32_t samples = 0;
    uint32_t invalid = 0;
    uint32_t overruns = 0;
    uint32_t read_errors = 0;
    uint16_t raw_min = UINT16_MAX;
    uint16_t raw_max = 0;
    uint64_t raw_sum = 0;
    uint32_t clipped = 0;
    double energy = 0;
    uint32_t output_samples = 0;
    float peak = 0;
    uint32_t frame_drops = 0;
    uint32_t open_samples = 0;
};

void publish(accumulator &a, audio_dsp::voice_gate &gate, int64_t elapsed_us)
{
    second out = {};
    out.adc_rate_hz = static_cast<uint32_t>(
        static_cast<uint64_t>(a.samples) * 1000000ull /
        static_cast<uint64_t>(elapsed_us));
    out.invalid = a.invalid;
    out.overruns = a.overruns;
    out.read_errors = a.read_errors;
    out.raw_min = a.samples ? a.raw_min : 0;
    out.raw_max = a.raw_max;
    out.raw_mean = a.samples ? static_cast<float>(a.raw_sum) / a.samples : 0;
    out.bias_mv = esp_adc_cal_raw_to_voltage(
        static_cast<uint32_t>(lrintf(out.raw_mean)), &calibration);
    out.clipped = a.clipped;
    out.rms = a.output_samples ?
        static_cast<float>(sqrt(a.energy / a.output_samples)) : 0;
    out.peak = a.peak;
    out.frame_drops = a.frame_drops;
    out.level = gate.take_peak();
    out.noise = gate.noise_floor();
    out.open_level = gate.open_level();
    out.open_ms = a.open_samples / (app_config::audio_rate_hz / 1000);
    out.learning = gate.learning();
    portENTER_CRITICAL(&second_lock);
    finished = out;
    finished_ready = true;
    portEXIT_CRITICAL(&second_lock);
    a = accumulator();
}

void task(void *)
{
    static uint8_t raw[block_bytes];
    static float building[frame_samples];
    static frame done;
    size_t building_count = 0;
    // Readings summed towards the next averaged sample.
    uint32_t sum = 0;
    uint32_t summed = 0;
    audio_dsp::voice_gate gate(static_cast<float>(app_config::audio_rate_hz));
    accumulator a;
    int64_t second_start = esp_timer_get_time();

    for (;;)
    {
        uint32_t got = 0;
        const esp_err_t read = adc_digi_read_bytes(raw, sizeof(raw), &got,
            100);
        if (read == ESP_ERR_INVALID_STATE)
        {
            ++a.overruns;  // Data is still valid; some was lost before it.
        }
        else if (read != ESP_OK)
        {
            ++a.read_errors;
            got = 0;
        }
        gate.set_ratio(ratio_value.load());
        const float gain = gain_value.load() * raw_to_16_bit;
        for (uint32_t at = 0; at + result_bytes <= got; at += result_bytes)
        {
            adc_digi_output_data_t result;
            memcpy(&result, raw + at, sizeof(result));
            if (result.type2.unit != 0 ||
                result.type2.channel != app_config::mic_adc_channel)
            {
                ++a.invalid;
                continue;
            }
            const uint16_t value = static_cast<uint16_t>(result.type2.data);
            ++a.samples;
            a.raw_sum += value;
            a.raw_min = value < a.raw_min ? value : a.raw_min;
            a.raw_max = value > a.raw_max ? value : a.raw_max;
            if (value <= app_config::adc_clip_margin ||
                value >= raw_full_scale - app_config::adc_clip_margin)
            {
                ++a.clipped;
                last_clip_us.store(esp_timer_get_time());
            }
            // Average mic_oversample readings into one sample, as the
            // sketch's readFiltered() did.
            sum += value;
            if (++summed < app_config::mic_oversample)
            {
                continue;
            }
            const float averaged = static_cast<float>(sum) /
                app_config::mic_oversample;
            sum = 0;
            summed = 0;
            building[building_count++] = gate.process(averaged) * gain;
            a.open_samples += gate.open();
            if (building_count < frame_samples)
            {
                continue;
            }
            building_count = 0;
            for (size_t k = 0; k < frame_samples; ++k)
            {
                done.pcm[k] = audio_dsp::limit(building[k]);
            }
            const audio_dsp::level level =
                audio_dsp::measure(done.pcm, frame_samples);
            latest_rms.store(level.rms);
            latest_peak.store(level.peak);
            a.energy += static_cast<double>(level.rms) * level.rms *
                frame_samples;
            a.output_samples += frame_samples;
            a.peak = level.peak > a.peak ? level.peak : a.peak;
            if (capturing.load() &&
                xQueueSend(frame_queue, &done, 0) != pdTRUE)
            {
                ++a.frame_drops;
            }
            int16_t *buffer = record_buffer.load();
            const uint32_t wanted = record_wanted.load();
            uint32_t count = record_count.load();
            if (buffer && count < wanted)
            {
                const uint32_t n = wanted - count < frame_samples ?
                    wanted - count : frame_samples;
                memcpy(buffer + count, done.pcm, n * sizeof(int16_t));
                record_count.store(count + n);
            }
        }
        const int64_t now = esp_timer_get_time();
        if (now - second_start >= 1000000)
        {
            publish(a, gate, now - second_start);
            second_start = now;
        }
    }
}
}

const char *start()
{
    frame_queue = xQueueCreate(4, sizeof(frame));
    if (!frame_queue)
    {
        return "Microphone frame queue";
    }
    esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_12, ADC_WIDTH_BIT_12,
        1100, &calibration);
    adc_digi_init_config_t init = {};
    init.max_store_buf_size = block_bytes * 4;
    init.conv_num_each_intr = block_bytes;
    init.adc1_chan_mask = 1u << app_config::mic_adc_channel;
    init.adc2_chan_mask = 0;
    if (adc_digi_initialize(&init) != ESP_OK)
    {
        return "ADC DMA initialise";
    }
    static adc_digi_pattern_config_t pattern = {};
    pattern.atten = ADC_ATTEN_DB_12;  // ~0-3.1 V; MAX9814 sits at 1.25 V.
    pattern.channel = app_config::mic_adc_channel;
    pattern.unit = 0;  // ADC1.
    pattern.bit_width = SOC_ADC_DIGI_MAX_BITWIDTH;
    adc_digi_configuration_t config = {};
    config.conv_limit_en = false;
    config.conv_limit_num = 250;
    config.pattern_num = 1;
    config.adc_pattern = &pattern;
    config.sample_freq_hz = app_config::adc_rate_hz;
    config.conv_mode = ADC_CONV_SINGLE_UNIT_1;
    config.format = ADC_DIGI_OUTPUT_FORMAT_TYPE2;
    if (adc_digi_controller_configure(&config) != ESP_OK)
    {
        return "ADC DMA configure";
    }
    if (adc_digi_start() != ESP_OK)
    {
        return "ADC DMA start";
    }
    if (xTaskCreatePinnedToCore(task, "mic", 8192, nullptr,
            app_config::audio_priority, nullptr,
            app_config::audio_core) != pdPASS)
    {
        return "Microphone task";
    }
    return nullptr;
}

void set_gain(float value)
{
    gain_value.store(value);
}

float gain()
{
    return gain_value.load();
}

void set_gate_ratio(float ratio)
{
    using audio_dsp::voice_gate;
    ratio_value.store(ratio < voice_gate::min_ratio ? voice_gate::min_ratio :
        ratio > voice_gate::max_ratio ? voice_gate::max_ratio : ratio);
}

float gate_ratio()
{
    return ratio_value.load();
}

void set_capture(bool on)
{
    if (on)
    {
        xQueueReset(frame_queue);
    }
    capturing.store(on);
}

QueueHandle_t frames()
{
    return frame_queue;
}

float level_rms()
{
    return latest_rms.load();
}

float level_peak()
{
    return latest_peak.load();
}

bool clipping_recently()
{
    return esp_timer_get_time() - last_clip_us.load() < 500000;
}

bool take_second(second *out)
{
    portENTER_CRITICAL(&second_lock);
    const bool ready = finished_ready;
    if (ready)
    {
        *out = finished;
        finished_ready = false;
    }
    portEXIT_CRITICAL(&second_lock);
    return ready;
}

void record(int16_t *buffer, uint32_t samples)
{
    record_buffer.store(nullptr);
    record_count.store(0);
    record_wanted.store(samples);
    record_buffer.store(buffer);
}

uint32_t recorded()
{
    return record_count.load();
}
}
