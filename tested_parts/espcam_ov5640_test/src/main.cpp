#include <Arduino.h>
#include <WiFi.h>
#include <esp_camera.h>
#include <esp_log.h>
#include <esp_rom_crc.h>
#include <esp_http_server.h>
#include <freertos/semphr.h>

#include "app_config.h"

namespace
{
struct frame_size_info
{
    framesize_t size;
    const char *name;
    uint16_t width;
    uint16_t height;
};

// Every size the self-test checks, smallest first. The largest one sets the
// frame buffer size at init, so all of them fit later.
constexpr frame_size_info frame_sizes[] = {
    {FRAMESIZE_QVGA, "QVGA", 320, 240},
    {FRAMESIZE_VGA, "VGA", 640, 480},
    {FRAMESIZE_SVGA, "SVGA", 800, 600},
    {FRAMESIZE_XGA, "XGA", 1024, 768},
    {FRAMESIZE_HD, "HD", 1280, 720},
    {FRAMESIZE_SXGA, "SXGA", 1280, 1024},
    {FRAMESIZE_UXGA, "UXGA", 1600, 1200},
    {FRAMESIZE_FHD, "FHD", 1920, 1080},
    {FRAMESIZE_QXGA, "QXGA", 2048, 1536},
    {FRAMESIZE_QSXGA, "QSXGA", 2560, 1920},
};
constexpr size_t frame_size_count =
    sizeof(frame_sizes) / sizeof(frame_sizes[0]);
constexpr size_t default_size_index = 1;  // VGA for the live view

SemaphoreHandle_t camera_mutex = nullptr;
volatile bool test_running = false;
volatile size_t current_size_index = default_size_index;
volatile uint32_t frames_streamed = 0;
bool serial_live = false;
uint32_t serial_frames_sent = 0;
httpd_handle_t web_server = nullptr;
httpd_handle_t stream_server = nullptr;
bool camera_ready = false;

struct jpeg_check
{
    bool valid;
    uint16_t width;
    uint16_t height;
};

// Checks the SOI and EOI markers and reads the real size from the SOF header
// rather than trusting the driver's width and height fields.
jpeg_check check_jpeg(const uint8_t *buf, size_t len)
{
    jpeg_check result = {false, 0, 0};
    if (buf == nullptr || len < 4 || buf[0] != 0xFF || buf[1] != 0xD8)
    {
        return result;
    }

    bool has_eoi = false;
    const size_t tail_start = len > 16 ? len - 16 : 0;
    for (size_t i = len - 1; i > tail_start; --i)
    {
        if (buf[i - 1] == 0xFF && buf[i] == 0xD9)
        {
            has_eoi = true;
            break;
        }
    }
    if (!has_eoi)
    {
        return result;
    }

    size_t i = 2;
    while (i + 4 <= len)
    {
        if (buf[i] != 0xFF)
        {
            return result;
        }
        const uint8_t marker = buf[i + 1];
        if (marker == 0xFF)
        {
            ++i;
            continue;
        }
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD8))
        {
            i += 2;
            continue;
        }
        if (marker == 0xDA)
        {
            return result;  // scan data before any frame header
        }
        const size_t seg_len = (static_cast<size_t>(buf[i + 2]) << 8) |
                               buf[i + 3];
        const bool is_sof = marker >= 0xC0 && marker <= 0xCF &&
                            marker != 0xC4 && marker != 0xC8 &&
                            marker != 0xCC;
        if (is_sof)
        {
            if (i + 9 > len)
            {
                return result;
            }
            result.height = static_cast<uint16_t>((buf[i + 5] << 8) |
                                                  buf[i + 6]);
            result.width = static_cast<uint16_t>((buf[i + 7] << 8) |
                                                 buf[i + 8]);
            result.valid = true;
            return result;
        }
        i += 2 + seg_len;
    }
    return result;
}

bool set_frame_size(size_t index)
{
    if (index >= frame_size_count)
    {
        return false;
    }
    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor == nullptr)
    {
        return false;
    }
    xSemaphoreTake(camera_mutex, portMAX_DELAY);
    const int status = sensor->set_framesize(sensor, frame_sizes[index].size);
    xSemaphoreGive(camera_mutex);
    if (status != 0)
    {
        return false;
    }
    current_size_index = index;
    return true;
}

camera_fb_t *grab_frame()
{
    xSemaphoreTake(camera_mutex, portMAX_DELAY);
    camera_fb_t *fb = esp_camera_fb_get();
    xSemaphoreGive(camera_mutex);
    return fb;
}

bool init_camera()
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
    config.jpeg_quality = app_config::jpeg_quality;
    config.fb_count = app_config::frame_buffer_count;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.grab_mode = CAMERA_GRAB_LATEST;

    const uint32_t started_ms = millis();
    const esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK)
    {
        Serial.printf("FAIL camera init: 0x%x (%s)\n",
                      static_cast<unsigned>(err), esp_err_to_name(err));
        Serial.println("  Check the ribbon cable, the board env and PSRAM.");
        return false;
    }

    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor == nullptr)
    {
        Serial.println("FAIL camera init: no sensor handle");
        return false;
    }
    const camera_sensor_info_t *info =
        esp_camera_sensor_get_info(&sensor->id);
    Serial.printf("Camera init %lu ms | sensor PID 0x%04x (%s) "
                  "SCCB 0x%02x\n",
                  static_cast<unsigned long>(millis() - started_ms),
                  static_cast<unsigned>(sensor->id.PID),
                  info != nullptr ? info->name : "unknown",
                  static_cast<unsigned>(sensor->slv_addr));
    if (sensor->id.PID != OV5640_PID)
    {
        Serial.println("FAIL sensor is not an OV5640");
        return false;
    }
    Serial.println("PASS sensor is OV5640");
    return set_frame_size(default_size_index);
}

struct size_result
{
    bool pass;
    int settle_frames;
    int frames_ok;
    uint32_t min_bytes;
    uint32_t max_bytes;
    uint32_t total_bytes;
    uint32_t elapsed_ms;
};

size_result test_one_size(size_t index)
{
    const frame_size_info &want = frame_sizes[index];
    size_result r = {false, 0, 0, UINT32_MAX, 0, 0, 0};
    if (!set_frame_size(index))
    {
        Serial.printf("  %-5s set_framesize failed\n", want.name);
        return r;
    }

    // The sensor keeps sending the old size for a few frames.
    bool settled = false;
    while (r.settle_frames < app_config::settle_frames_max && !settled)
    {
        camera_fb_t *fb = grab_frame();
        ++r.settle_frames;
        if (fb == nullptr)
        {
            continue;
        }
        const jpeg_check jc = check_jpeg(fb->buf, fb->len);
        settled = jc.valid && jc.width == want.width &&
                  jc.height == want.height;
        esp_camera_fb_return(fb);
    }

    const uint32_t started_ms = millis();
    for (int n = 0; n < app_config::timed_frames; ++n)
    {
        camera_fb_t *fb = grab_frame();
        if (fb == nullptr)
        {
            Serial.printf("  %-5s frame %d: no frame (timeout)\n", want.name,
                          n);
            continue;
        }
        const jpeg_check jc = check_jpeg(fb->buf, fb->len);
        const uint32_t len = static_cast<uint32_t>(fb->len);
        if (fb->format == PIXFORMAT_JPEG && jc.valid &&
            jc.width == want.width && jc.height == want.height)
        {
            ++r.frames_ok;
            r.total_bytes += len;
            r.min_bytes = len < r.min_bytes ? len : r.min_bytes;
            r.max_bytes = len > r.max_bytes ? len : r.max_bytes;
        }
        else
        {
            Serial.printf("  %-5s frame %d bad: %lu B, jpeg %s, %ux%u\n",
                          want.name, n, static_cast<unsigned long>(len),
                          jc.valid ? "ok" : "corrupt",
                          static_cast<unsigned>(jc.width),
                          static_cast<unsigned>(jc.height));
        }
        esp_camera_fb_return(fb);
    }
    r.elapsed_ms = millis() - started_ms;
    r.pass = r.frames_ok == app_config::timed_frames;
    return r;
}

void run_self_test()
{
    if (!camera_ready)
    {
        Serial.println("Self-test skipped: camera not ready");
        return;
    }
    test_running = true;
    delay(200);  // let a running stream drop its frame

    Serial.printf("\nSelf-test: %d frames per size, JPEG quality %d\n",
                  app_config::timed_frames, app_config::jpeg_quality);
    Serial.println("  size  WxH        settle ok   fps    avg KB  "
                   "min-max KB  result");
    int passed = 0;
    for (size_t i = 0; i < frame_size_count; ++i)
    {
        const size_result r = test_one_size(i);
        const float fps = r.elapsed_ms > 0
                              ? 1000.0f * r.frames_ok / r.elapsed_ms
                              : 0.0f;
        const float avg_kb = r.frames_ok > 0
                                 ? r.total_bytes / 1024.0f / r.frames_ok
                                 : 0.0f;
        Serial.printf("  %-5s %4ux%-4u   %d      %d/%d  %5.1f  %7.1f  "
                      "%5.1f-%-5.1f  %s\n",
                      frame_sizes[i].name,
                      static_cast<unsigned>(frame_sizes[i].width),
                      static_cast<unsigned>(frame_sizes[i].height),
                      r.settle_frames, r.frames_ok, app_config::timed_frames,
                      fps, avg_kb,
                      r.frames_ok > 0 ? r.min_bytes / 1024.0f : 0.0f,
                      r.max_bytes / 1024.0f, r.pass ? "PASS" : "FAIL");
        passed += r.pass ? 1 : 0;
    }
    Serial.printf("Self-test %s: %d/%u sizes passed | PSRAM free %lu KB\n",
                  passed == static_cast<int>(frame_size_count) ? "PASS"
                                                               : "FAIL",
                  passed, static_cast<unsigned>(frame_size_count),
                  static_cast<unsigned long>(ESP.getFreePsram() / 1024));

    set_frame_size(default_size_index);
    test_running = false;
}

// ---- Serial viewer -------------------------------------------------------

// One frame on the serial port, read by tools/serial_view.py:
//   "OVJ1" | length u32 | width u16 | height u16 | crc32 u32 | JPEG bytes
// All little-endian; crc32 is the zlib CRC-32 of the JPEG bytes. Text lines
// can appear between frames, and the viewer prints them.
constexpr uint8_t frame_magic[4] = {'O', 'V', 'J', '1'};

void put_u16(uint8_t *out, uint16_t value)
{
    out[0] = static_cast<uint8_t>(value);
    out[1] = static_cast<uint8_t>(value >> 8);
}

void put_u32(uint8_t *out, uint32_t value)
{
    put_u16(out, static_cast<uint16_t>(value));
    put_u16(out + 2, static_cast<uint16_t>(value >> 16));
}

bool send_frame_serial()
{
    camera_fb_t *fb = grab_frame();
    if (fb == nullptr)
    {
        return false;
    }
    const jpeg_check jc = check_jpeg(fb->buf, fb->len);
    const uint32_t len = static_cast<uint32_t>(fb->len);
    uint8_t header[16];
    memcpy(header, frame_magic, sizeof(frame_magic));
    put_u32(header + 4, len);
    put_u16(header + 8, jc.width);
    put_u16(header + 10, jc.height);
    put_u32(header + 12, esp_rom_crc32_le(0, fb->buf, len));
    Serial.write(header, sizeof(header));
    const size_t sent = Serial.write(fb->buf, fb->len);
    esp_camera_fb_return(fb);
    if (sent != len)
    {
        return false;
    }
    ++serial_frames_sent;
    return true;
}

void set_serial_live(bool on)
{
    serial_live = on;
    // Driver warnings printed mid-frame would corrupt it; the viewer counts
    // CRC failures instead.
    esp_log_level_set("*", on ? ESP_LOG_NONE : ESP_LOG_WARN);
    if (!on)
    {
        Serial.printf("Serial live view off (%lu frames sent)\n",
                      static_cast<unsigned long>(serial_frames_sent));
    }
}

// ---- Web viewer ----------------------------------------------------------

constexpr const char *stream_boundary = "ov5640frame";

const char index_html[] = R"HTML(<!doctype html>
<html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>OV5640 test</title>
<style>
body{font-family:sans-serif;margin:12px;background:#111;color:#eee}
img{max-width:100%;border:1px solid #444}
select,button,a{font-size:16px;margin:4px 4px 8px 0}
a{color:#8cf}
</style></head><body>
<h3>OV5640 live view</h3>
<div>
<select id="size"></select>
<button onclick="flip('vflip')">V-flip</button>
<button onclick="flip('hmirror')">Mirror</button>
<a href="/capture" target="_blank">Snapshot</a>
</div>
<div id="status">loading...</div>
<img id="view" alt="stream">
<script>
const sizes=["QVGA","VGA","SVGA","XGA","HD","SXGA","UXGA","FHD","QXGA",
"QSXGA"];
const sel=document.getElementById('size');
sizes.forEach((s,i)=>{const o=document.createElement('option');
o.value=i;o.textContent=s;sel.appendChild(o);});
sel.onchange=()=>fetch('/control?size='+sel.value).then(status);
function flip(k){fetch('/control?'+k+'=toggle').then(status);}
function status(){fetch('/status').then(r=>r.json()).then(j=>{
sel.value=j.size_index;
document.getElementById('status').textContent=
j.size+' | streamed '+j.frames_streamed+' | PSRAM free '+j.psram_free_kb+
' KB'+(j.test_running?' | SELF-TEST RUNNING':'');});}
document.getElementById('view').src=
location.protocol+'//'+location.hostname+':81/stream';
status();setInterval(status,2000);
</script></body></html>)HTML";

esp_err_t handle_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, index_html, HTTPD_RESP_USE_STRLEN);
}

esp_err_t handle_capture(httpd_req_t *req)
{
    if (test_running)
    {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_send(req, "self-test running", HTTPD_RESP_USE_STRLEN);
    }
    // With GRAB_LATEST the first buffer can be older than the last
    // size change, so drop one frame first.
    camera_fb_t *fb = grab_frame();
    if (fb != nullptr)
    {
        esp_camera_fb_return(fb);
    }
    fb = grab_frame();
    if (fb == nullptr)
    {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    const jpeg_check jc = check_jpeg(fb->buf, fb->len);
    // The header values must stay valid until the response is sent.
    char width_text[8];
    char height_text[8];
    snprintf(width_text, sizeof(width_text), "%u",
             static_cast<unsigned>(jc.width));
    snprintf(height_text, sizeof(height_text), "%u",
             static_cast<unsigned>(jc.height));
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition",
                       "inline; filename=ov5640.jpg");
    httpd_resp_set_hdr(req, "X-Width", width_text);
    httpd_resp_set_hdr(req, "X-Height", height_text);
    const esp_err_t err = httpd_resp_send(
        req, reinterpret_cast<const char *>(fb->buf), fb->len);
    esp_camera_fb_return(fb);
    return err;
}

esp_err_t handle_stream(httpd_req_t *req)
{
    char part[96];
    esp_err_t err = httpd_resp_set_type(
        req, "multipart/x-mixed-replace;boundary=ov5640frame");
    while (err == ESP_OK)
    {
        if (test_running)
        {
            delay(50);
            continue;
        }
        camera_fb_t *fb = grab_frame();
        if (fb == nullptr)
        {
            delay(10);
            continue;
        }
        const int part_len = snprintf(
            part, sizeof(part),
            "\r\n--%s\r\nContent-Type: image/jpeg\r\n"
            "Content-Length: %u\r\n\r\n",
            stream_boundary, static_cast<unsigned>(fb->len));
        err = httpd_resp_send_chunk(req, part, part_len);
        if (err == ESP_OK)
        {
            err = httpd_resp_send_chunk(
                req, reinterpret_cast<const char *>(fb->buf), fb->len);
        }
        esp_camera_fb_return(fb);
        if (err == ESP_OK)
        {
            ++frames_streamed;
        }
    }
    return err;
}

esp_err_t handle_status(httpd_req_t *req)
{
    char json[192];
    const size_t index = current_size_index;
    snprintf(json, sizeof(json),
             "{\"size_index\":%u,\"size\":\"%s\",\"frames_streamed\":%lu,"
             "\"psram_free_kb\":%lu,\"test_running\":%s}",
             static_cast<unsigned>(index), frame_sizes[index].name,
             static_cast<unsigned long>(frames_streamed),
             static_cast<unsigned long>(ESP.getFreePsram() / 1024),
             test_running ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

esp_err_t handle_control(httpd_req_t *req)
{
    char query[64];
    char value[16];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing query");
        return ESP_FAIL;
    }
    sensor_t *sensor = esp_camera_sensor_get();
    bool ok = false;
    if (httpd_query_key_value(query, "size", value, sizeof(value)) == ESP_OK)
    {
        ok = !test_running &&
             set_frame_size(static_cast<size_t>(atoi(value)));
    }
    else if (httpd_query_key_value(query, "vflip", value, sizeof(value)) ==
             ESP_OK)
    {
        ok = sensor->set_vflip(sensor, !sensor->status.vflip) == 0;
    }
    else if (httpd_query_key_value(query, "hmirror", value, sizeof(value)) ==
             ESP_OK)
    {
        ok = sensor->set_hmirror(sensor, !sensor->status.hmirror) == 0;
    }
    if (!ok)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "control failed");
        return ESP_FAIL;
    }
    return httpd_resp_send(req, "ok", HTTPD_RESP_USE_STRLEN);
}

bool start_web()
{
    WiFi.mode(WIFI_AP);
    if (!WiFi.softAP(app_config::ap_ssid, app_config::ap_password,
                     app_config::ap_channel))
    {
        Serial.println("FAIL Wi-Fi access point did not start");
        return false;
    }
    WiFi.setSleep(false);

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.ctrl_port = 32768;
    if (httpd_start(&web_server, &config) != ESP_OK)
    {
        Serial.println("FAIL web server did not start");
        return false;
    }
    const httpd_uri_t pages[] = {
        {"/", HTTP_GET, handle_index, nullptr},
        {"/capture", HTTP_GET, handle_capture, nullptr},
        {"/status", HTTP_GET, handle_status, nullptr},
        {"/control", HTTP_GET, handle_control, nullptr},
    };
    for (const httpd_uri_t &page : pages)
    {
        httpd_register_uri_handler(web_server, &page);
    }

    // The stream gets its own server so it never blocks the page or
    // snapshots.
    config.server_port = 81;
    config.ctrl_port = 32769;
    if (httpd_start(&stream_server, &config) != ESP_OK)
    {
        Serial.println("FAIL stream server did not start");
        return false;
    }
    const httpd_uri_t stream = {"/stream", HTTP_GET, handle_stream, nullptr};
    httpd_register_uri_handler(stream_server, &stream);

    Serial.printf("Wi-Fi AP \"%s\" password \"%s\" channel %u\n",
                  app_config::ap_ssid, app_config::ap_password,
                  static_cast<unsigned>(app_config::ap_channel));
    Serial.printf("Live view: http://%s/  snapshot: /capture  "
                  "stream: port 81 /stream\n",
                  WiFi.softAPIP().toString().c_str());
    return true;
}

void print_help()
{
    Serial.println("Commands: l live frames on | x live off | c one frame | "
                   "t self-test");
    Serial.println("          0-9 frame size (0 QVGA .. 9 QSXGA) | v v-flip | "
                   "m mirror | s status | h help");
}

void print_status()
{
    Serial.printf("Board %s | size %s | serial frames %lu (live %s) | "
                  "heap %lu KB | PSRAM free %lu KB\n",
                  app_config::board_name,
                  frame_sizes[current_size_index].name,
                  static_cast<unsigned long>(serial_frames_sent),
                  serial_live ? "on" : "off",
                  static_cast<unsigned long>(ESP.getFreeHeap() / 1024),
                  static_cast<unsigned long>(ESP.getFreePsram() / 1024));
    if (app_config::wifi_enabled)
    {
        Serial.printf("Wi-Fi streamed %lu | AP clients %u\n",
                      static_cast<unsigned long>(frames_streamed),
                      static_cast<unsigned>(WiFi.softAPgetStationNum()));
    }
}

void handle_serial()
{
    while (Serial.available() > 0)
    {
        const int c = Serial.read();
        sensor_t *sensor = camera_ready ? esp_camera_sensor_get() : nullptr;
        if (c == 'l' && camera_ready)
        {
            set_serial_live(true);
        }
        else if (c == 'x')
        {
            set_serial_live(false);
        }
        else if (c == 'c' && camera_ready)
        {
            // Drop one buffer that may predate a size change.
            camera_fb_t *fb = grab_frame();
            if (fb != nullptr)
            {
                esp_camera_fb_return(fb);
            }
            send_frame_serial();
        }
        else if (c == 't')
        {
            const bool was_live = serial_live;
            set_serial_live(false);
            run_self_test();
            set_serial_live(was_live);
        }
        else if (c >= '0' && c <= '9' && camera_ready)
        {
            const size_t index = static_cast<size_t>(c - '0');
            Serial.printf("Frame size %s: %s\n", frame_sizes[index].name,
                          set_frame_size(index) ? "ok" : "failed");
        }
        else if (c == 'v' && sensor != nullptr)
        {
            sensor->set_vflip(sensor, !sensor->status.vflip);
            Serial.printf("V-flip %s\n", sensor->status.vflip ? "on" : "off");
        }
        else if (c == 'm' && sensor != nullptr)
        {
            sensor->set_hmirror(sensor, !sensor->status.hmirror);
            Serial.printf("Mirror %s\n",
                          sensor->status.hmirror ? "on" : "off");
        }
        else if (c == 's')
        {
            print_status();
        }
        else if (c == 'h' || c == '?')
        {
            print_help();
        }
    }
}
}

void setup()
{
    Serial.setTxBufferSize(app_config::serial_tx_buffer_bytes);
    Serial.begin(app_config::serial_baud);
    const uint32_t started_ms = millis();
    while (!Serial && millis() - started_ms < app_config::serial_wait_ms)
    {
        delay(10);
    }

    Serial.printf("\nOV5640 camera test | %s\n", app_config::board_name);
    Serial.printf("PSRAM %lu KB (free %lu KB) | flash %lu MB\n",
                  static_cast<unsigned long>(ESP.getPsramSize() / 1024),
                  static_cast<unsigned long>(ESP.getFreePsram() / 1024),
                  static_cast<unsigned long>(ESP.getFlashChipSize() >> 20));
    if (!psramFound())
    {
        Serial.println("FAIL no PSRAM: frame buffers need it");
        return;
    }

    camera_mutex = xSemaphoreCreateMutex();
    camera_ready = camera_mutex != nullptr && init_camera();
    if (!camera_ready)
    {
        return;
    }
    run_self_test();
    if (app_config::wifi_enabled)
    {
        start_web();
    }
    print_help();
}

void loop()
{
    handle_serial();
    if (serial_live && !test_running)
    {
        send_frame_serial();
    }
    else
    {
        delay(10);
    }
}
