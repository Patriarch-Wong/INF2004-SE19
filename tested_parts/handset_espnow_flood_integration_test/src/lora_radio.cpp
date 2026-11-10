#include "lora_radio.h"

#include <Arduino.h>
#include <RadioLib.h>
#include <SPI.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <atomic>
#include <string.h>
#include "app_config.h"

namespace lora_radio
{
namespace
{
using namespace app_config;

// The LCD takes whichever bus LoRa does not (ui.inc).
SPIClass lora_hspi(HSPI);
SPIClass &bus()
{
    return lora_on_hspi ? lora_hspi : SPI;
}

// RadioLib waits for BUSY (up to 3 s after a reset, 1 s per command) and
// for TX-done in loops that call yield(). Arduino's yield() only lets
// tasks of the same or higher priority run, so on core 0 IDLE0 starved
// and the task watchdog rebooted the board whenever the radio was slow
// (board 1, 2026-11-04). Sleeping one tick per poll lets IDLE0 run.
class task_hal : public ArduinoHal
{
public:
    task_hal(SPIClass &spi, SPISettings settings) : ArduinoHal(spi, settings)
    {
    }

    void yield() override
    {
        vTaskDelay(1);
    }
};

task_hal hal(bus(), SPISettings(lora_spi_hz, MSBFIRST, SPI_MODE0));

// RadioLib constructor order: HAL, CS, interrupt (DIO9), RESET, BUSY.
LR1121 radio = new Module(&hal, lora_cs_pin, lora_irq_pin, lora_reset_pin,
    lora_busy_pin);

// The LR1121's internal RF-switch DIOs on the Core1121 (not ESP32 GPIOs),
// as in lora_encrypt_test.
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

struct tx_item
{
    uint8_t size;
    uint8_t bytes[secure_message::max_frame_size];
};

TaskHandle_t task_handle = nullptr;
QueueHandle_t tx_queue = nullptr;
QueueHandle_t rx_queue = nullptr;
volatile bool packet_flag = false;
bool bus_started = false;

std::atomic<uint8_t> radio_state{static_cast<uint8_t>(state::off)};
std::atomic<int16_t> error_code{0};
std::atomic<const char *> error_step{""};
std::atomic<bool> busy_high{false};
std::atomic<uint32_t> busy_ms{0};

std::atomic<uint32_t> n_starts{0};
std::atomic<uint32_t> n_sent{0};
std::atomic<uint32_t> n_send_failed{0};
std::atomic<uint32_t> n_received{0};
std::atomic<uint32_t> n_rx_errors{0};
std::atomic<uint32_t> n_tx_drops{0};
std::atomic<uint32_t> n_rx_drops{0};

void IRAM_ATTR on_dio9()
{
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

// Pulses RESET and times BUSY, so a radio that never leaves reset (wiring
// or power) can be told apart from one that does not answer on SPI.
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
    busy_ms.store(millis() - start);
}

bool listen_again()
{
    if (!check(radio.standby(), "standby before RX"))
    {
        return false;
    }
    // RadioLib 7.7.1 keeps the last TX length as the RX maximum.
    if (!check(radio.explicitHeader(), "restore RX length"))
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
        bus().begin(lora_sck_pin, lora_miso_pin, lora_mosi_pin, lora_cs_pin);
        bus_started = true;
    }
    probe_busy();
    radio.tcxoVoltage = lora_tcxo_volts;
    ConfigLoRa_t config;
    config.frequency = lora_mhz;
    config.bandwidth = lora_bandwidth_khz;
    config.spreadingFactor = lora_spreading_factor;
    config.codingRate = lora_coding_rate;
    config.syncWord = lora_sync_word;
    config.power = lora_power_dbm;
    config.preambleLength = lora_preamble;
    if (!check(radio.begin(config), "init"))
    {
        return;
    }
    radio.setRfSwitchTable(rf_switch_pins, rf_switch_table);
    // The Core1121 routes sub-GHz TX through the high-power PA.
    if (!check(radio.setOutputPower(lora_power_dbm, true), "set power") ||
        !check(radio.setCRC(2), "enable CRC"))
    {
        return;
    }
    radio.setPacketReceivedAction(on_dio9);
    if (!listen_again())
    {
        return;
    }
    set_state(state::ok);
}

void read_packet()
{
    packet_flag = false;
    // Freeze the packet buffer while copying it, as lora_encrypt_test.
    if (!check(radio.standby(), "standby"))
    {
        return;
    }
    received_frame frame = {};
    const size_t size = radio.getPacketLength();
    if (size < secure_message::overhead ||
        size > secure_message::max_frame_size)
    {
        n_rx_errors.fetch_add(1);
        listen_again();
        return;
    }
    const int16_t code = radio.readData(frame.bytes, size);
    frame.rssi = radio.getRSSI();
    frame.snr = radio.getSNR();
    if (!listen_again())
    {
        return;
    }
    if (code != RADIOLIB_ERR_NONE)
    {
        n_rx_errors.fetch_add(1);
        return;
    }
    frame.size = static_cast<uint8_t>(size);
    frame.received_ms = millis();
    n_received.fetch_add(1);
    if (xQueueSend(rx_queue, &frame, 0) != pdTRUE)
    {
        n_rx_drops.fetch_add(1);
    }
}

void transmit(const tx_item &item)
{
    if (!check(radio.standby(), "standby before TX"))
    {
        n_send_failed.fetch_add(1);  // The radio restarts; the frame is gone.
        return;
    }
    // The blocking TX-done interrupt must not look like a received packet.
    radio.clearPacketReceivedAction();
    const int16_t code = radio.transmit(item.bytes, item.size);
    radio.setPacketReceivedAction(on_dio9);
    if (code == RADIOLIB_ERR_NONE)
    {
        n_sent.fetch_add(1);
    }
    else
    {
        n_send_failed.fetch_add(1);
    }
    listen_again();
}

void task(void *)
{
    for (;;)
    {
        if (current() != state::ok)
        {
            // Frames queued before a failure are stale by the time the
            // radio is back; the connection logic resends what matters.
            tx_item stale;
            while (xQueueReceive(tx_queue, &stale, 0) == pdTRUE)
            {
                n_tx_drops.fetch_add(1);
            }
            start_radio();
            if (current() != state::ok)
            {
                vTaskDelay(pdMS_TO_TICKS(lora_retry_ms));
                continue;
            }
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
        if (packet_flag)
        {
            read_packet();
        }
        tx_item item;
        while (current() == state::ok &&
            xQueueReceive(tx_queue, &item, 0) == pdTRUE)
        {
            transmit(item);
            if (packet_flag && current() == state::ok)
            {
                read_packet();
            }
        }
    }
}
}  // namespace

bool begin()
{
    if (task_handle)
    {
        return true;
    }
    tx_queue = xQueueCreate(lora_tx_queue_depth, sizeof(tx_item));
    rx_queue = xQueueCreate(lora_rx_queue_depth, sizeof(received_frame));
    if (!tx_queue || !rx_queue)
    {
        return false;
    }
    // Below the codec (core 0, priority 2): RadioLib waits for TX-done in
    // a loop, and that wait must never delay an Opus frame.
    return xTaskCreatePinnedToCore(task, "lora", lora_task_stack, nullptr,
               lora_priority, &task_handle, lora_core) == pdPASS;
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
    case state::ok: return "ok";
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

uint32_t busy_low_ms()
{
    return busy_ms.load();
}

bool send(const uint8_t *frame, size_t size)
{
    if (current() != state::ok || !frame || size == 0 ||
        size > secure_message::max_frame_size)
    {
        n_tx_drops.fetch_add(1);
        return false;
    }
    tx_item item;
    item.size = static_cast<uint8_t>(size);
    memcpy(item.bytes, frame, size);
    if (xQueueSend(tx_queue, &item, 0) != pdTRUE)
    {
        n_tx_drops.fetch_add(1);
        return false;
    }
    xTaskNotifyGive(task_handle);
    return true;
}

bool receive(received_frame *frame)
{
    return frame && rx_queue &&
        xQueueReceive(rx_queue, frame, 0) == pdTRUE;
}

counters stats()
{
    counters c;
    c.starts = n_starts.load();
    c.sent = n_sent.load();
    c.send_failed = n_send_failed.load();
    c.received = n_received.load();
    c.rx_errors = n_rx_errors.load();
    c.tx_drops = n_tx_drops.load();
    c.rx_drops = n_rx_drops.load();
    return c;
}
}
