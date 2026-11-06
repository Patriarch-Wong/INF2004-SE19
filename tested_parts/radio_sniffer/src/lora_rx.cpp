#include "lora_rx.h"

#include <Arduino.h>
#include <RadioLib.h>
#include <SPI.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <atomic>

namespace lora_rx
{
namespace
{
using namespace sniffer_config;

// RadioLib waits for BUSY in loops that call yield(). Arduino's yield()
// lets only equal or higher priority tasks run, which starved IDLE0 and
// tripped the task watchdog in handset_lora_integration_test. Sleeping one
// tick per poll lets it run.
class task_hal : public ArduinoHal
{
public:
    task_hal(SPIClass &spi, SPISettings spi_settings)
        : ArduinoHal(spi, spi_settings)
    {
    }

    void yield() override
    {
        vTaskDelay(1);
    }
};

task_hal hal(SPI, SPISettings(lora_spi_hz, MSBFIRST, SPI_MODE0));

// RadioLib order: HAL, CS, interrupt (DIO9), RESET, BUSY.
LR1121 radio = new Module(&hal, lora_cs_pin, lora_irq_pin, lora_reset_pin,
    lora_busy_pin);

// The LR1121's own RF-switch DIOs on the Core1121, as
// handset_lora_integration_test. RX needs DIO5 high.
const uint32_t rf_switch_pins[] = {
    RADIOLIB_LR11X0_DIO5, RADIOLIB_LR11X0_DIO6, RADIOLIB_NC, RADIOLIB_NC,
    RADIOLIB_NC};
const Module::RfSwitchMode_t rf_switch_table[] = {
    {LR11x0::MODE_STBY, {LOW, LOW}},
    {LR11x0::MODE_RX, {HIGH, LOW}},
    {LR11x0::MODE_TX, {LOW, HIGH}},
    {LR11x0::MODE_TX_HP, {LOW, HIGH}},
    {LR11x0::MODE_TX_HF, {LOW, LOW}},
    {LR11x0::MODE_GNSS, {LOW, LOW}},
    {LR11x0::MODE_WIFI, {LOW, LOW}},
    END_OF_MODE_TABLE,
};

TaskHandle_t task_handle = nullptr;
QueueHandle_t queue = nullptr;
volatile bool packet_flag = false;
volatile int64_t packet_time_us = 0;
bool bus_started = false;

portMUX_TYPE settings_lock = portMUX_INITIALIZER_UNLOCKED;
lora_settings wanted = default_lora;   // Guarded by settings_lock.
std::atomic<bool> restart{false};
lora_settings active = default_lora;   // The task's copy.

std::atomic<bool> wanted_on{true};
std::atomic<uint8_t> radio_state{static_cast<uint8_t>(state::off)};
std::atomic<int16_t> error_code{0};
std::atomic<const char *> error_step{""};
std::atomic<bool> busy_high{false};

std::atomic<uint32_t> n_starts{0};
std::atomic<uint32_t> n_received{0};
std::atomic<uint32_t> n_crc_errors{0};
std::atomic<uint32_t> n_read_errors{0};
std::atomic<uint32_t> n_queue_drops{0};

void IRAM_ATTR on_dio9()
{
    packet_time_us = esp_timer_get_time();
    packet_flag = true;
    BaseType_t woken = pdFALSE;
    if (task_handle)
    {
        vTaskNotifyGiveFromISR(task_handle, &woken);
    }
    portYIELD_FROM_ISR(woken);
}

void set_state(state value)
{
    radio_state.store(static_cast<uint8_t>(value));
}

bool check(int16_t code, const char *step)
{
    if (code == RADIOLIB_ERR_NONE)
    {
        return true;
    }
    error_code.store(code);
    error_step.store(step);
    set_state(state::failed);
    return false;
}

void probe_busy()
{
    pinMode(lora_busy_pin, INPUT);
    pinMode(lora_reset_pin, OUTPUT);
    digitalWrite(lora_reset_pin, LOW);
    delay(2);
    digitalWrite(lora_reset_pin, HIGH);
    const uint32_t start = millis();
    while (digitalRead(lora_busy_pin) == HIGH &&
        millis() - start < lora_busy_wait_ms)
    {
        delay(1);
    }
    busy_high.store(digitalRead(lora_busy_pin) == HIGH);
}

bool listen_again()
{
    if (!check(radio.standby(), "standby before RX") ||
        !check(radio.explicitHeader(), "explicit header"))
    {
        return false;
    }
    packet_flag = false;
    return check(radio.startReceive(), "start receive");
}

void start_radio()
{
    set_state(state::starting);
    n_starts.fetch_add(1);
    error_code.store(0);
    error_step.store("");
    if (!bus_started)
    {
        SPI.begin(lora_sck_pin, lora_miso_pin, lora_mosi_pin, lora_cs_pin);
        bus_started = true;
    }
    probe_busy();
    radio.tcxoVoltage = lora_tcxo_volts;
    ConfigLoRa_t config;
    config.frequency = active.mhz;
    config.bandwidth = active.bandwidth_khz;
    config.spreadingFactor = active.spreading_factor;
    config.codingRate = active.coding_rate;
    config.syncWord = active.sync_word;
    config.preambleLength = active.preamble;
    // begin() sets an output power; the sniffer never transmits.
    config.power = 0;
    if (!check(radio.begin(config), "init"))
    {
        return;
    }
    radio.setRfSwitchTable(rf_switch_pins, rf_switch_table);
    radio.setPacketReceivedAction(on_dio9);
    if (!listen_again())
    {
        return;
    }
    set_state(state::listening);
}

void read_packet()
{
    packet_flag = false;
    frame_decode::lora_capture item;
    item.time_us = packet_time_us;
    item.mhz = active.mhz;
    item.spreading_factor = active.spreading_factor;
    // Standby freezes the buffer while it is copied.
    if (!check(radio.standby(), "standby"))
    {
        return;
    }
    const size_t size = radio.getPacketLength();
    if (size == 0 || size > lora_max_packet)
    {
        n_read_errors.fetch_add(1);
        listen_again();
        return;
    }
    // readData still copies the bytes when the CRC fails, then says so.
    const int16_t code = radio.readData(item.payload, size);
    item.rssi = radio.getRSSI();
    item.snr = radio.getSNR();
    const bool listening = listen_again();
    if (code != RADIOLIB_ERR_NONE && code != RADIOLIB_ERR_CRC_MISMATCH)
    {
        n_read_errors.fetch_add(1);
        return;
    }
    item.size = size;
    item.crc_ok = code == RADIOLIB_ERR_NONE;
    if (!item.crc_ok)
    {
        n_crc_errors.fetch_add(1);
    }
    n_received.fetch_add(1);
    if (xQueueSend(queue, &item, 0) != pdTRUE)
    {
        n_queue_drops.fetch_add(1);
    }
    (void)listening;  // A failed restart is retried by the task loop.
}

void task(void *)
{
    for (;;)
    {
        if (restart.exchange(false))
        {
            portENTER_CRITICAL(&settings_lock);
            active = wanted;
            portEXIT_CRITICAL(&settings_lock);
            set_state(state::off);
        }
        if (!wanted_on.load())
        {
            if (current() == state::listening)
            {
                radio.standby();
            }
            if (current() != state::failed)
            {
                set_state(state::paused);
            }
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
            continue;
        }
        if (current() != state::listening)
        {
            start_radio();
            if (current() != state::listening)
            {
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(lora_retry_ms));
                continue;
            }
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
        if (packet_flag)
        {
            read_packet();
        }
    }
}
}  // namespace

bool begin(const lora_settings &initial)
{
    if (task_handle)
    {
        return true;
    }
    if (!valid(initial))
    {
        return false;
    }
    wanted = initial;
    active = initial;
    queue = xQueueCreate(lora_queue_depth,
        sizeof(frame_decode::lora_capture));
    if (!queue)
    {
        return false;
    }
    return xTaskCreatePinnedToCore(task, "lora_rx", lora_task_stack, nullptr,
               lora_priority, &task_handle, lora_core) == pdPASS;
}

bool apply(const lora_settings &next)
{
    if (!valid(next))
    {
        return false;
    }
    portENTER_CRITICAL(&settings_lock);
    wanted = next;
    portEXIT_CRITICAL(&settings_lock);
    restart.store(true);
    if (task_handle)
    {
        xTaskNotifyGive(task_handle);
    }
    return true;
}

lora_settings settings()
{
    portENTER_CRITICAL(&settings_lock);
    const lora_settings copy = wanted;
    portEXIT_CRITICAL(&settings_lock);
    return copy;
}

void set_enabled(bool on)
{
    // Turning on again restarts the radio from paused (task loop).
    wanted_on.store(on);
    if (task_handle)
    {
        xTaskNotifyGive(task_handle);
    }
}

bool enabled()
{
    return wanted_on.load();
}

state current()
{
    return static_cast<state>(radio_state.load());
}

const char *state_name(state value)
{
    switch (value)
    {
    case state::off: return "off";
    case state::starting: return "starting";
    case state::listening: return "listening";
    case state::paused: return "paused";
    case state::failed: return "failed";
    }
    return "?";
}

int16_t last_error()
{
    return error_code.load();
}

const char *failed_step()
{
    return error_step.load();
}

bool busy_stuck()
{
    return busy_high.load();
}

bool receive(frame_decode::lora_capture *out)
{
    return out && queue && xQueueReceive(queue, out, 0) == pdTRUE;
}

counters stats()
{
    counters c;
    c.starts = n_starts.load();
    c.received = n_received.load();
    c.crc_errors = n_crc_errors.load();
    c.read_errors = n_read_errors.load();
    c.queue_drops = n_queue_drops.load();
    return c;
}
}
