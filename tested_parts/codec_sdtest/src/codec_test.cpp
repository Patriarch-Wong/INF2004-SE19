/** @file codec_test.cpp
 * @brief Generated-audio SD round trip; hardware APIs own their resources.
 */
#include "codec_test.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include "heap_owner.h"
#include <string.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "app_config.h"
#include "codec.h"
#include "sd_card.h"

#define TEST_HEADER_BYTES (32U)
#define TEST_PATH_BYTES (64U)
#define TEST_MAX_FILES (10000U)

#if (APP_CHANNELS != 1U) || (APP_FRAME_MSEC != 20U)
#error "The generated signal and quality checks require mono, 20 ms frames."
#endif

struct test_timing_t
{
    uint64_t total_usec;
    uint32_t max_usec;
    uint32_t over_budget;
};

struct test_context_t
{
    codec_test_log_t pf_log;
    codec_t *p_codec = nullptr;
    sd_card_file_t *p_file = nullptr;
    app_detail::heap_owner_t<int16_t[]> p_input;
    app_detail::heap_owner_t<int16_t[]> p_output;
    app_detail::heap_owner_t<uint8_t[]> p_expected;
    uint32_t lookahead;
    uint32_t frames;
    uint32_t padded_samples;
    size_t stream_bytes;
    uint32_t payload_bytes;
    size_t smallest_packet;
    size_t largest_packet;
    uint32_t flush_usec;
    char path[TEST_PATH_BYTES];
    uint8_t header[TEST_HEADER_BYTES];
    test_timing_t encode;
    test_timing_t decode;
    test_timing_t write;
    test_timing_t read;
    test_timing_t encode_write;
    test_timing_t read_decode;

    test_context_t() = default;
    test_context_t(const test_context_t &) = delete;
    test_context_t &operator=(const test_context_t &) = delete;
    test_context_t(test_context_t &&) = delete;
    test_context_t &operator=(test_context_t &&) = delete;

    ~test_context_t() noexcept
    {
        // Normal cleanup checks close status; this covers early returns.
        (void)sd_card_close(&p_file);
        codec_destroy(&p_codec);
    }
};

static const codec_config_t g_codec_config =
{
    APP_SAMPLE_RATE_HZ,
    APP_BITRATE_BPS,
    APP_CHANNELS,
    APP_COMPLEXITY
};

/** Route bounded diagnostic messages through the application's logger. */
static void
test_log(test_context_t *p_test, const char *p_format, ...)
    __attribute__((format(printf, 2, 3)));

static void
test_log(test_context_t *p_test, const char *p_format, ...)
{
    char message[256];
    va_list args;
    va_start(args, p_format);
    (void)vsnprintf(message, sizeof(message), p_format, args);
    va_end(args);
    p_test->pf_log(message);
}

static bool
test_codec_ok(test_context_t *p_test, codec_status_t status,
              const char *p_operation)
{
    if (CODEC_OK != status)
    {
        test_log(p_test, "FAIL: %s: %s\n", p_operation,
                 codec_status_string(status));
        return (false);
    }
    return (true);
}

static bool
test_sd_ok(test_context_t *p_test, sd_card_status_t status,
           const char *p_operation)
{
    if (SD_CARD_OK != status)
    {
        test_log(p_test, "FAIL: %s: %s\n", p_operation,
                 sd_card_status_string(status));
        return (false);
    }
    return (true);
}

static void
test_timing_add(test_timing_t *p_timing, int64_t started_usec)
{
    const uint32_t elapsed =
        static_cast<uint32_t>(esp_timer_get_time() - started_usec);
    p_timing->total_usec += elapsed;
    if (elapsed > p_timing->max_usec)
    {
        p_timing->max_usec = elapsed;
    }
    if (elapsed > (APP_FRAME_MSEC * 1000U))
    {
        ++p_timing->over_budget;
    }
}

static void
test_timing_print(test_context_t *p_test, const test_timing_t *p_timing,
                  const char *p_label)
{
    test_log(p_test, "%s: avg %.3f ms | max %.3f ms | >20 ms: %u/%u\n",
        p_label, p_timing->total_usec / (1000.0 * p_test->frames),
        p_timing->max_usec / 1000.0, p_timing->over_budget, p_test->frames);
}

/** Encode integer metadata without depending on structure padding. */
static void
test_put_u32(uint8_t *p_output, uint32_t value)
{
    for (uint32_t index = 0U; index < 4U; ++index)
    {
        p_output[index] = static_cast<uint8_t>((value >> (8U * index)) & 0xFFU);
    }
}

/** Generate the test signal; calloc supplies the delay-flushing tail. */
static void
test_generate_signal(test_context_t *p_test)
{
    const double tau = 6.283185307179586;
    for (uint32_t index = 0U; index < APP_INPUT_SAMPLES; ++index)
    {
        const double time_seconds =
            static_cast<double>(index) / APP_SAMPLE_RATE_HZ;
        double value = 0.0;
        if (time_seconds < 1.0)
        {
            value = (9000.0 * sin(tau * 440.0 * time_seconds)) +
                    (3000.0 * sin(tau * 1000.0 * time_seconds));
        }
        else if (time_seconds < 2.5)
        {
            const double sweep_seconds = time_seconds - 1.0;
            value = 10000.0 * sin(tau * ((300.0 * sweep_seconds) +
                     (600.0 * sweep_seconds * sweep_seconds)));
        }
        else
        {
            /* Final half-second of silence. */
        }
        p_test->p_input[index] = static_cast<int16_t>(value);
    }
}

static bool
test_prepare(test_context_t *p_test)
{
    if (!test_codec_ok(p_test,
        codec_encoder_create(&g_codec_config, &p_test->p_codec),
        "encoder create"))
    {
        return (false);
    }
    if (!test_codec_ok(p_test,
        codec_get_lookahead(p_test->p_codec, &p_test->lookahead), "lookahead"))
    {
        return (false);
    }
    p_test->frames = (APP_INPUT_SAMPLES + p_test->lookahead +
                     APP_FRAME_SAMPLES - 1U) / APP_FRAME_SAMPLES;
    p_test->padded_samples = p_test->frames * APP_FRAME_SAMPLES;
    p_test->p_input.reset(static_cast<int16_t *>(heap_caps_calloc(
        p_test->padded_samples, sizeof(int16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
    p_test->p_output.reset(static_cast<int16_t *>(heap_caps_malloc(
        p_test->padded_samples * sizeof(int16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
    p_test->p_expected.reset(static_cast<uint8_t *>(heap_caps_malloc(
        p_test->frames * (APP_MAX_PACKET_BYTES + 2U),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
    if ((nullptr == p_test->p_input) || (nullptr == p_test->p_output) ||
        (nullptr == p_test->p_expected))
    {
        test_log(p_test, "FAIL: PSRAM test-buffer allocation\n");
        return (false);
    }
    test_generate_signal(p_test);
    (void)memcpy(p_test->header, "OSD1", 4U);
    test_put_u32(p_test->header + 4U, APP_SAMPLE_RATE_HZ);
    test_put_u32(p_test->header + 8U, APP_CHANNELS);
    test_put_u32(p_test->header + 12U, APP_FRAME_SAMPLES);
    test_put_u32(p_test->header + 16U, APP_INPUT_SAMPLES);
    test_put_u32(p_test->header + 20U, p_test->lookahead);
    test_put_u32(p_test->header + 24U, p_test->frames);
    test_put_u32(p_test->header + 28U, APP_BITRATE_BPS);
    test_log(p_test, "Input: %u samples (%u bytes) | lookahead: %u\n",
        APP_INPUT_SAMPLES, APP_INPUT_SAMPLES * 2U, p_test->lookahead);
    return (true);
}

/** Exclusive creation both preserves old files and avoids check/open races. */
static bool
test_open_new(test_context_t *p_test)
{
    if (!test_sd_ok(p_test, sd_card_mkdir(APP_TEST_DIRECTORY), "mkdir"))
    {
        return (false);
    }
    for (uint32_t index = 0U; index < TEST_MAX_FILES; ++index)
    {
        const int32_t length = snprintf(p_test->path, sizeof(p_test->path),
            "%s/run_%04u.opusbin", APP_TEST_DIRECTORY, index);
        if ((length < 0) ||
            (static_cast<size_t>(length) >= sizeof(p_test->path)))
        {
            test_log(p_test, "FAIL: test path too long\n");
            return (false);
        }
        const sd_card_status_t status = sd_card_open(p_test->path,
            SD_CARD_CREATE_NEW, &p_test->p_file);
        if (SD_CARD_EXISTS != status)
        {
            if (!test_sd_ok(p_test, status, "create test file"))
            {
                return (false);
            }
            test_log(p_test, "SD test file: %s\n", p_test->path);
            return (test_sd_ok(p_test, sd_card_write(p_test->p_file,
                p_test->header, sizeof(p_test->header)), "write header"));
        }
        vTaskDelay(1U);
    }
    test_log(p_test, "FAIL: all test filenames occupied\n");
    return (false);
}

/** Exercise rejected operations without changing codec state or file data. */
static bool
test_api_contracts(test_context_t *p_test)
{
    codec_config_t invalid_config = g_codec_config;
    invalid_config.sample_rate_hz = 12345U;
    codec_t *p_invalid_codec = nullptr;
    const codec_status_t create_status =
        codec_encoder_create(&invalid_config, &p_invalid_codec);
    const bool b_invalid_rejected =
        (CODEC_INVALID_ARGUMENT == create_status) &&
        (nullptr == p_invalid_codec);
    codec_destroy(&p_invalid_codec);

    uint8_t packet[APP_MAX_PACKET_BYTES] = {};
    size_t packet_bytes = 1U;
    const codec_status_t encode_status = codec_encode(p_test->p_codec,
        p_test->p_input.get(), APP_FRAME_SAMPLES - 1U,
        packet, sizeof(packet), &packet_bytes);

    sd_card_file_t *p_duplicate = nullptr;
    const sd_card_status_t duplicate_status = sd_card_open(p_test->path,
        SD_CARD_CREATE_NEW, &p_duplicate);
    const bool b_duplicate_rejected = (SD_CARD_EXISTS == duplicate_status) &&
                                      (nullptr == p_duplicate);
    const sd_card_status_t close_status = sd_card_close(&p_duplicate);
    const bool b_passed = b_invalid_rejected && b_duplicate_rejected &&
        (SD_CARD_OK == close_status) &&
        (CODEC_INVALID_ARGUMENT == encode_status) && (0U == packet_bytes) &&
        (SD_CARD_INVALID_STATE == sd_card_unmount()) &&
        (SD_CARD_INVALID_STATE ==
            sd_card_read(p_test->p_file, packet, 1U)) &&
        (SD_CARD_INVALID_ARGUMENT ==
            sd_card_open("/../escape", SD_CARD_READ, &p_duplicate));
    (void)sd_card_close(&p_duplicate);
    if (!b_passed)
    {
        test_log(p_test, "FAIL: API argument/state/preservation checks\n");
    }
    else
    {
        test_log(p_test, "API argument/state/preservation checks passed\n");
    }
    return (b_passed);
}

static bool
test_encode_frames(test_context_t *p_test)
{
    test_log(p_test, "Encoding and writing packets to SD...\n");
    for (uint32_t frame = 0U; frame < p_test->frames; ++frame)
    {
        const int64_t frame_started = esp_timer_get_time();
        uint8_t *p_record = p_test->p_expected.get() + p_test->stream_bytes;
        size_t packet_bytes = 0U;
        const int64_t started = esp_timer_get_time();
        const codec_status_t status = codec_encode(p_test->p_codec,
            p_test->p_input.get() + (frame * APP_FRAME_SAMPLES),
            APP_FRAME_SAMPLES,
            p_record + 2U, APP_MAX_PACKET_BYTES, &packet_bytes);
        test_timing_add(&p_test->encode, started);
        if (!test_codec_ok(p_test, status, "encode frame"))
        {
            return (false);
        }
        p_record[0] = static_cast<uint8_t>(packet_bytes & 0xFFU);
        p_record[1] = static_cast<uint8_t>((packet_bytes >> 8U) & 0xFFU);
        const int64_t write_started = esp_timer_get_time();
        const sd_card_status_t write_status = sd_card_write(p_test->p_file,
            p_record, packet_bytes + 2U);
        test_timing_add(&p_test->write, write_started);
        test_timing_add(&p_test->encode_write, frame_started);
        if (!test_sd_ok(p_test, write_status, "write packet"))
        {
            return (false);
        }
        p_test->stream_bytes += packet_bytes + 2U;
        p_test->payload_bytes += static_cast<uint32_t>(packet_bytes);
        if (packet_bytes < p_test->smallest_packet)
        {
            p_test->smallest_packet = packet_bytes;
        }
        if (packet_bytes > p_test->largest_packet)
        {
            p_test->largest_packet = packet_bytes;
        }
        vTaskDelay(1U);
    }
    codec_destroy(&p_test->p_codec);
    const int64_t started = esp_timer_get_time();
    const sd_card_status_t status = sd_card_close(&p_test->p_file);
    p_test->flush_usec = static_cast<uint32_t>(esp_timer_get_time() - started);
    return (test_sd_ok(p_test, status, "flush/sync/close"));
}

static bool
test_reopen(test_context_t *p_test)
{
    if (!test_sd_ok(p_test, sd_card_open(p_test->path, SD_CARD_READ,
                                       &p_test->p_file), "reopen file"))
    {
        return (false);
    }
    size_t file_bytes = 0U;
    if (!test_sd_ok(p_test, sd_card_size(p_test->p_file, &file_bytes), "size"))
    {
        return (false);
    }
    if (file_bytes != (sizeof(p_test->header) + p_test->stream_bytes))
    {
        test_log(p_test, "FAIL: reopened SD file size mismatch\n");
        return (false);
    }
    uint8_t header[TEST_HEADER_BYTES];
    if (!test_sd_ok(p_test, sd_card_read(p_test->p_file, header,
                                        sizeof(header)), "read header"))
    {
        return (false);
    }
    if (0 != memcmp(header, p_test->header, sizeof(header)))
    {
        test_log(p_test, "FAIL: SD header readback mismatch\n");
        return (false);
    }
    return (test_codec_ok(p_test, codec_decoder_create(&g_codec_config,
        &p_test->p_codec), "decoder create"));
}

/** Read and verify one complete record before handing it to the codec API. */
static bool
test_read_packet(test_context_t *p_test, size_t cursor,
                 uint8_t *p_packet, size_t *p_bytes)
{
    uint8_t length[2];
    if (!test_sd_ok(p_test, sd_card_read(p_test->p_file, length,
                                       sizeof(length)), "read length"))
    {
        return (false);
    }
    *p_bytes = static_cast<size_t>(length[0]) |
               (static_cast<size_t>(length[1]) << 8U);
    if ((cursor > p_test->stream_bytes) ||
        ((p_test->stream_bytes - cursor) < 2U) || (0U == *p_bytes) ||
        (*p_bytes > APP_MAX_PACKET_BYTES) ||
        (*p_bytes > (p_test->stream_bytes - cursor - 2U)))
    {
        test_log(p_test, "FAIL: invalid stored packet length\n");
        return (false);
    }
    if (!test_sd_ok(p_test, sd_card_read(p_test->p_file, p_packet,
                                        *p_bytes), "read packet"))
    {
        return (false);
    }
    if ((0 != memcmp(length, p_test->p_expected.get() + cursor, 2U)) ||
        (0 != memcmp(p_packet, p_test->p_expected.get() + cursor + 2U,
                     *p_bytes)))
    {
        test_log(p_test, "FAIL: SD packet readback mismatch\n");
        return (false);
    }
    return (true);
}

static bool
test_decode_frames(test_context_t *p_test)
{
    size_t cursor = 0U;
    uint32_t decoded_samples = 0U;
    uint8_t packet[APP_MAX_PACKET_BYTES];
    test_log(p_test, "Reading, verifying, and decoding SD packets...\n");
    for (uint32_t frame = 0U; frame < p_test->frames; ++frame)
    {
        const int64_t frame_started = esp_timer_get_time();
        size_t bytes = 0U;
        if (!test_read_packet(p_test, cursor, packet, &bytes))
        {
            return (false);
        }
        test_timing_add(&p_test->read, frame_started);
        uint32_t samples = 0U;
        const int64_t started = esp_timer_get_time();
        const codec_status_t status = codec_decode(p_test->p_codec,
            packet, bytes, p_test->p_output.get() + decoded_samples,
            APP_FRAME_SAMPLES, &samples);
        test_timing_add(&p_test->decode, started);
        test_timing_add(&p_test->read_decode, frame_started);
        if (!test_codec_ok(p_test, status, "decode frame"))
        {
            return (false);
        }
        decoded_samples += samples;
        cursor += bytes + 2U;
        vTaskDelay(1U);
    }
    if ((cursor != p_test->stream_bytes) ||
        (decoded_samples != p_test->padded_samples))
    {
        test_log(p_test, "FAIL: packet/sample count mismatch\n");
        return (false);
    }
    return (test_sd_ok(p_test, sd_card_close(&p_test->p_file), "read close"));
}

/** Compare delay-aligned audio; thresholds are only a signal smoke test. */
static bool
test_quality(test_context_t *p_test)
{
    double input_energy = 0.0;
    double output_energy = 0.0;
    double error_energy = 0.0;
    double cross = 0.0;
    for (uint32_t index = 0U; index < APP_INPUT_SAMPLES; ++index)
    {
        const double input = p_test->p_input[index];
        const double output = p_test->p_output[index + p_test->lookahead];
        input_energy += input * input;
        output_energy += output * output;
        error_energy += (input - output) * (input - output);
        cross += input * output;
    }
    const double correlation = (output_energy > 0.0) ?
        cross / sqrt(input_energy * output_energy) : 0.0;
    const double snr = 10.0 * log10(input_energy / fmax(error_energy, 1.0));
    test_log(p_test, "Packets: %u | min/max: %u/%u | payload: %u bytes\n",
        p_test->frames, static_cast<unsigned>(p_test->smallest_packet),
        static_cast<unsigned>(p_test->largest_packet), p_test->payload_bytes);
    test_log(p_test, "Aligned audio: correlation=%.4f | SNR=%.2f dB\n",
             correlation, snr);
    test_timing_print(p_test, &p_test->encode, "Encode");
    test_timing_print(p_test, &p_test->decode, "Decode");
    test_timing_print(p_test, &p_test->write, "SD packet write (buffered)");
    test_timing_print(p_test, &p_test->read, "SD read + verify");
    test_timing_print(p_test, &p_test->encode_write, "Encode + SD write");
    test_timing_print(p_test, &p_test->read_decode,
                      "SD read + verify + decode");
    test_log(p_test, "SD flush/sync/close: %.3f ms | verified %u file bytes\n",
        p_test->flush_usec / 1000.0,
        static_cast<unsigned>(sizeof(p_test->header) + p_test->stream_bytes));
    test_log(p_test, "Retained on card: %s (custom Opus container)\n",
             p_test->path);
    test_log(p_test, "Codec task minimum unused stack: %u bytes\n",
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    return (isfinite(correlation) && (correlation > 0.8) && (snr > 6.0));
}

bool
codec_test_run(codec_test_log_t pf_log)
{
    if (nullptr == pf_log)
    {
        return (false);
    }
    test_context_t test{};
    test.pf_log = pf_log;
    test.smallest_packet = APP_MAX_PACKET_BYTES;
    test_log(&test, "\n--- Opus + SD API round-trip | %s ---\n",
             codec_version());
    test_log(&test, "%u Hz, mono PCM16, %u bps CBR, %u ms, complexity %u\n",
        APP_SAMPLE_RATE_HZ, APP_BITRATE_BPS, APP_FRAME_MSEC, APP_COMPLEXITY);
    bool b_passed = test_prepare(&test) && test_open_new(&test) &&
        test_api_contracts(&test) && test_encode_frames(&test) &&
        test_reopen(&test) &&
        test_decode_frames(&test) && test_quality(&test);

    /* One cleanup path covers every failed stage as well as successful runs. */
    if (!test_sd_ok(&test, sd_card_close(&test.p_file), "cleanup close"))
    {
        b_passed = false;
    }
    codec_destroy(&test.p_codec);
    test.p_input.reset();
    test.p_output.reset();
    test.p_expected.reset();
    test_log(&test, b_passed ?
        "PASS: Opus + SD write/read, decode, and audio checks\n" :
        "FAIL: Opus + SD integration test\n");
    return (b_passed);
}
