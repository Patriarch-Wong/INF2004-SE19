#include "speaker.h"
#include <driver/ledc.h>
#include <driver/timer.h>
#include <esp_timer.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <soc/ledc_struct.h>
#include <atomic>
#include <math.h>
#include "app_config.h"
#include "audio_dsp.h"

namespace speaker
{
namespace
{
constexpr ledc_mode_t pwm_mode = LEDC_LOW_SPEED_MODE;
constexpr ledc_channel_t pwm_channel = LEDC_CHANNEL_0;
constexpr ledc_timer_t pwm_timer = LEDC_TIMER_0;
constexpr uint8_t silence = 128;  // Half duty: the sketch's idle level.

// Sample clock: timer group 0, timer 0, from the 80 MHz APB clock / 2.
constexpr timer_group_t clock_group = TIMER_GROUP_0;
constexpr timer_idx_t clock_timer = TIMER_0;
constexpr uint32_t clock_divider = 2;
constexpr uint64_t clock_ticks =
    80000000ull / clock_divider / app_config::audio_rate_hz;  // 2,500.

// Ring between the task and the interrupt: 64 ms. Kept short, like the
// I2S DMA it replaces, so the codec does not decode far ahead of playback
// (that would shorten the time NACKs have to recover a frame).
constexpr uint32_t ring_size = 1024;  // Power of two.
constexpr uint32_t ring_mask = ring_size - 1;
// Peak of the local test tones on the 16-bit scale: about 100 of 127 PWM
// steps at the default volume.
constexpr float test_amplitude = 8500.0f;
constexpr float pi = 3.14159265358979f;

struct frame
{
    int16_t pcm[frame_samples];
};

uint8_t ring[ring_size];
std::atomic<uint32_t> ring_head{0};  // Written by the task.
std::atomic<uint32_t> ring_tail{0};  // Written by the interrupt.
std::atomic<bool> drained{true};  // The interrupt found the ring empty.
std::atomic<uint32_t> ticks{0};  // Interrupts so far.

QueueHandle_t queue = nullptr;
std::atomic<uint8_t> volume_value{app_config::speaker_default_volume};
std::atomic<bool> mute{false};
std::atomic<bool> streaming{false};
std::atomic<bool> clear_request{false};
std::atomic<int> notch_count{0};
std::atomic<float> fb_gain{1.0f};
std::atomic<float> new_notch{0};
std::atomic<float> latest_rms{0};
std::atomic<uint32_t> measured_rate{0};

// Test signal request, written by the loop and read by the task.
portMUX_TYPE test_lock = portMUX_INITIALIZER_UNLOCKED;
struct test_request
{
    bool pending;
    uint32_t from_hz;
    uint32_t to_hz;
    uint32_t frames;
} request = {};
std::atomic<bool> test_active{false};

portMUX_TYPE stats_lock = portMUX_INITIALIZER_UNLOCKED;
counters totals = {};

void count(uint32_t counters::*field)
{
    portENTER_CRITICAL(&stats_lock);
    ++(totals.*field);
    portEXIT_CRITICAL(&stats_lock);
}

// One duty update per audio sample. Register writes only, so it is safe
// in an IRAM interrupt (ledc_set_duty takes a lock and lives in flash). When the ring is empty
// the output slides back to 128 one step per sample instead of jumping.
bool IRAM_ATTR on_tick(void *)
{
    static uint8_t level = silence;
    const uint32_t tail = ring_tail.load(std::memory_order_relaxed);
    if (tail != ring_head.load(std::memory_order_acquire))
    {
        level = ring[tail & ring_mask];
        ring_tail.store(tail + 1, std::memory_order_release);
    }
    else
    {
        drained.store(true, std::memory_order_relaxed);
        level += level < silence ? 1 : level > silence ? -1 : 0;
    }
    // What ledc_set_duty + ledc_update_duty write (hal/ledc_ll.h, which
    // does not compile as C++): duty with 4 fraction bits, start, latch.
    auto &channel = LEDC.channel_group[pwm_mode].channel[pwm_channel];
    channel.duty.duty = static_cast<uint32_t>(level) << 4;
    channel.conf1.duty_start = 1;
    channel.conf0.low_speed_update = 1;
    ticks.fetch_add(1, std::memory_order_relaxed);
    return false;  // Woke no task.
}

uint32_t ring_free()
{
    return ring_size - (ring_head.load() - ring_tail.load());
}

// Fills one frame of the requested tone or sweep; false when it is over.
bool next_test_frame(int16_t *pcm)
{
    static test_request active = {};
    static uint32_t done = 0;
    static float phase = 0;
    portENTER_CRITICAL(&test_lock);
    if (request.pending)
    {
        active = request;
        request.pending = false;
        done = 0;
        phase = 0;
    }
    portEXIT_CRITICAL(&test_lock);
    if (done >= active.frames)
    {
        test_active.store(false);
        return false;
    }
    for (size_t i = 0; i < frame_samples; ++i)
    {
        const float progress = static_cast<float>(done * frame_samples + i) /
            static_cast<float>(active.frames * frame_samples);
        const float hz = active.from_hz +
            (static_cast<float>(active.to_hz) - active.from_hz) * progress;
        phase += 2.0f * pi * hz / app_config::audio_rate_hz;
        if (phase > 2.0f * pi)
        {
            phase -= 2.0f * pi;
        }
        pcm[i] = static_cast<int16_t>(test_amplitude * sinf(phase));
    }
    // Short fades so the test itself does not click.
    if (done == 0)
    {
        audio_dsp::ramp(pcm, frame_samples, 0, 1);
    }
    if (done + 1 == active.frames)
    {
        audio_dsp::ramp(pcm, frame_samples, 1, 0);
    }
    ++done;
    return true;
}

void task(void *)
{
    static frame input;
    audio_dsp::speaker_guard guard(
        static_cast<float>(app_config::audio_rate_hz));
    bool previous_in_stream = false;  // The last frame belonged to a stream.
    int64_t rate_start_us = esp_timer_get_time();
    uint32_t rate_start_ticks = ticks.load();
    for (;;)
    {
        const int64_t now = esp_timer_get_time();
        if (now - rate_start_us >= 1000000)
        {
            const uint32_t t = ticks.load();
            measured_rate.store(static_cast<uint32_t>(
                static_cast<uint64_t>(t - rate_start_ticks) * 1000000ull /
                static_cast<uint64_t>(now - rate_start_us)));
            rate_start_us = now;
            rate_start_ticks = t;
        }
        if (clear_request.exchange(false))
        {
            guard.clear_notches();
            notch_count.store(0);
        }

        bool test = false;
        bool have = false;
        if (test_active.load())
        {
            have = test = next_test_frame(input.pcm);
        }
        if (!have)
        {
            have = xQueueReceive(queue, &input, pdMS_TO_TICKS(5)) == pdTRUE;
        }
        if (!have)
        {
            continue;
        }
        while (ring_free() < frame_samples)
        {
            vTaskDelay(1);
        }
        // The ring ran dry since the last frame, inside a stream.
        if (drained.exchange(false) && streaming.load() &&
            previous_in_stream)
        {
            count(&counters::underruns);
        }
        const int volume = volume_value.load();
        const bool muted = mute.load();
        uint32_t head = ring_head.load();
        for (size_t i = 0; i < frame_samples; ++i)
        {
            ring[head++ & ring_mask] = test ?
                audio_dsp::speaker_guard::plain(input.pcm[i], volume, muted) :
                guard.process(input.pcm[i], volume, muted);
        }
        ring_head.store(head, std::memory_order_release);

        const float added = guard.take_new_notch();
        if (added > 0)
        {
            new_notch.store(added);
        }
        notch_count.store(guard.notches());
        fb_gain.store(guard.feedback_gain());
        latest_rms.store(audio_dsp::measure(input.pcm, frame_samples).rms);
        count(&counters::frames);
        previous_in_stream = streaming.load();
    }
}
}

const char *start()
{
    queue = xQueueCreate(3, sizeof(frame));
    if (!queue)
    {
        return "Speaker queue";
    }
    // The sketch's ledcSetup(0, 78125, 8) + ledcAttachPin(7, 0), idle 128.
    ledc_timer_config_t pwm = {};
    pwm.speed_mode = pwm_mode;
    pwm.duty_resolution = static_cast<ledc_timer_bit_t>(
        app_config::speaker_pwm_bits);
    pwm.timer_num = pwm_timer;
    pwm.freq_hz = app_config::speaker_pwm_hz;
    pwm.clk_cfg = LEDC_AUTO_CLK;
    if (ledc_timer_config(&pwm) != ESP_OK)
    {
        return "Speaker PWM timer";
    }
    ledc_channel_config_t channel = {};
    channel.gpio_num = app_config::speaker_pin;
    channel.speed_mode = pwm_mode;
    channel.channel = pwm_channel;
    channel.intr_type = LEDC_INTR_DISABLE;
    channel.timer_sel = pwm_timer;
    channel.duty = silence;
    channel.hpoint = 0;
    if (ledc_channel_config(&channel) != ESP_OK)
    {
        return "Speaker PWM channel";
    }
    if (xTaskCreatePinnedToCore(task, "speaker", 8192, nullptr,
            app_config::audio_priority, nullptr,
            app_config::audio_core) != pdPASS)
    {
        return "Speaker task";
    }
    timer_config_t clock = {};
    clock.alarm_en = TIMER_ALARM_EN;
    clock.counter_en = TIMER_PAUSE;
    clock.intr_type = TIMER_INTR_LEVEL;
    clock.counter_dir = TIMER_COUNT_UP;
    clock.auto_reload = TIMER_AUTORELOAD_EN;
    clock.divider = clock_divider;
    clock.clk_src = TIMER_SRC_CLK_APB;
    if (timer_init(clock_group, clock_timer, &clock) != ESP_OK ||
        timer_set_counter_value(clock_group, clock_timer, 0) != ESP_OK ||
        timer_set_alarm_value(clock_group, clock_timer, clock_ticks) !=
            ESP_OK ||
        timer_enable_intr(clock_group, clock_timer) != ESP_OK ||
        timer_isr_callback_add(clock_group, clock_timer, on_tick, nullptr,
            ESP_INTR_FLAG_IRAM) != ESP_OK ||
        timer_start(clock_group, clock_timer) != ESP_OK)
    {
        return "Speaker sample clock";
    }
    return nullptr;
}

void set_volume(uint8_t value)
{
    volume_value.store(value < 1 ? 1 :
        value > audio_dsp::speaker_guard::max_volume ?
            audio_dsp::speaker_guard::max_volume : value);
}

uint8_t volume()
{
    return volume_value.load();
}

void set_muted(bool on)
{
    mute.store(on);
}

bool muted()
{
    return mute.load();
}

void clear_notches()
{
    clear_request.store(true);
}

int notches()
{
    return notch_count.load();
}

float feedback_gain()
{
    return fb_gain.load();
}

float take_new_notch()
{
    return new_notch.exchange(0);
}

bool play(const int16_t *pcm, TickType_t wait)
{
    return xQueueSend(queue, pcm, wait) == pdTRUE;
}

unsigned queued()
{
    return uxQueueMessagesWaiting(queue);
}

void set_streaming(bool on)
{
    streaming.store(on);
}

void sweep(uint32_t from_hz, uint32_t to_hz, uint32_t ms)
{
    portENTER_CRITICAL(&test_lock);
    request.pending = true;
    request.from_hz = from_hz;
    request.to_hz = to_hz;
    request.frames = ms / 20 > 0 ? ms / 20 : 1;
    portEXIT_CRITICAL(&test_lock);
    test_active.store(true);
}

void tone(uint32_t hz, uint32_t ms)
{
    sweep(hz, hz, ms);
}

bool testing()
{
    return test_active.load();
}

float level_rms()
{
    return latest_rms.load();
}

uint32_t measured_rate_hz()
{
    return measured_rate.load();
}

counters stats()
{
    portENTER_CRITICAL(&stats_lock);
    const counters copy = totals;
    portEXIT_CRITICAL(&stats_lock);
    return copy;
}
}
