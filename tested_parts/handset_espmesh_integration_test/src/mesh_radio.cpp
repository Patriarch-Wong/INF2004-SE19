#include "mesh_radio.h"
#include <Arduino.h>
#include <esp_event.h>
#include <esp_mac.h>
#include <esp_mesh.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <atomic>
#include <string.h>
#include "app_config.h"
#include "radio.h"

namespace mesh_radio
{
namespace
{
constexpr size_t note_queue_depth = 16;

QueueHandle_t receive_queue = nullptr;
QueueHandle_t note_queue = nullptr;
mesh_cfg_t saved_config = {};
bool initialized = false;  // start()'s one-time part worked.
bool placeholder = false;
const char *failed_at = "";
esp_err_t failure = ESP_OK;
uint8_t own_mac[6] = {};
uint8_t own_board = 0;

std::atomic<uint8_t> status{static_cast<uint8_t>(state::off)};
std::atomic<bool> broken{false};  // The last start or restart failed.
std::atomic<uint32_t> status_ms{0};
std::atomic<bool> root_role{false};
std::atomic<bool> rootless_now{false};
std::atomic<uint8_t> root_id{0};
std::atomic<uint8_t> parent_id{0};
std::atomic<bool> router_noted{false};
std::atomic<int> layer_now{0};
// Bit N: board N is joined to this board's soft-AP. A set, so a repeated
// join or leave event can't make the count drift.
std::atomic<uint32_t> children_set{0};
std::atomic<int> nodes_now{0};
// The receive task stays out of esp_mesh_recv() while the loop stops and
// restarts the mesh, whose stop deletes the queues that call waits on.
std::atomic<bool> rx_pause{false};
std::atomic<bool> rx_paused{false};
std::atomic<uint32_t> sent{0};
std::atomic<uint32_t> send_failed{0};
std::atomic<uint32_t> received{0};
std::atomic<uint32_t> rx_drops{0};
std::atomic<uint32_t> unknown_drops{0};
std::atomic<uint32_t> parent_joins{0};
std::atomic<uint32_t> parent_losses{0};
std::atomic<uint32_t> router_misses{0};
std::atomic<uint32_t> restarts{0};
std::atomic<uint32_t> note_drops{0};

void set_state(state value)
{
    status.store(static_cast<uint8_t>(value));
    status_ms.store(millis());
}

void post(note_kind kind, int value = 0, const uint8_t *mac = nullptr)
{
    note item = {};
    item.kind = kind;
    item.value = static_cast<int16_t>(value);
    if (mac)
    {
        memcpy(item.mac, mac, 6);
    }
    if (!note_queue || xQueueSend(note_queue, &item, 0) != pdTRUE)
    {
        note_drops.fetch_add(1);
    }
}

// Records the root of this board's mesh, noting a change.
void set_root(uint8_t id, const uint8_t *mac = nullptr)
{
    if (root_id.exchange(id) != id && id != 0)
    {
        post(note_kind::root_found, id, mac);
    }
}

uint8_t board_id(const uint8_t *mac)
{
    const network_config::device *entry = find_board(mac);
    return entry ? entry->id : 0;
}

void set_child(const uint8_t *mac, bool joined)
{
    const uint8_t id = board_id(mac);
    if (id == 0 || id >= 32)
    {
        return;  // Not a listed board.
    }
    if (joined)
    {
        children_set.fetch_or(1u << id);
    }
    else
    {
        children_set.fetch_and(~(1u << id));
    }
}

// Runs in the default event loop task: record and queue, nothing slow.
// Events reach it in the order they were posted, so a restart's STOPPED
// comes before its STARTED.
void on_event(void *, esp_event_base_t, int32_t id, void *data)
{
    switch (id)
    {
    case MESH_EVENT_STARTED:
        children_set.store(0);
        parent_id.store(0);
        rootless_now.store(false);
        router_noted.store(false);
        // A root is the mesh as soon as it starts: its soft-AP takes
        // children, and it never gets a router to join.
        if (root_role.load())
        {
            layer_now.store(MESH_ROOT_LAYER);
            nodes_now.store(1);
            root_id.store(own_board);
            set_state(state::connected);
        }
        else
        {
            layer_now.store(0);
            nodes_now.store(0);
            root_id.store(0);
            set_state(state::joining);
        }
        post(note_kind::started, root_role.load());
        break;
    case MESH_EVENT_STOPPED:
        layer_now.store(0);
        children_set.store(0);
        parent_id.store(0);
        nodes_now.store(0);
        root_id.store(0);
        set_state(state::off);
        post(note_kind::stopped);
        break;
    case MESH_EVENT_PARENT_CONNECTED:
    {
        const auto *info = static_cast<mesh_event_connected_t *>(data);
        if (root_role.load())
        {
            break;  // Never: there is no router.
        }
        parent_joins.fetch_add(1);
        layer_now.store(info->self_layer);
        rootless_now.store(false);
        parent_id.store(board_id(info->connected.bssid));
        set_state(state::connected);
        post(note_kind::parent_joined, info->self_layer,
            info->connected.bssid);
        if (info->self_layer == MESH_ROOT_LAYER + 1)
        {
            // The parent is the root.
            set_root(parent_id.load(), info->connected.bssid);
        }
        break;
    }
    case MESH_EVENT_PARENT_DISCONNECTED:
    {
        const auto *info = static_cast<mesh_event_disconnected_t *>(data);
        if (root_role.load())
        {
            // A look for the router that isn't there. Expected; noted once.
            router_misses.fetch_add(1);
            if (!router_noted.exchange(true))
            {
                post(note_kind::router_missing, info->reason);
            }
            break;
        }
        // Only a loss if this board was in; a failed join attempt is not.
        if (status.load() == static_cast<uint8_t>(state::connected))
        {
            parent_losses.fetch_add(1);
            post(note_kind::parent_lost, info->reason);
        }
        layer_now.store(0);
        root_id.store(0);
        parent_id.store(0);
        set_state(state::joining);
        break;
    }
    case MESH_EVENT_CHILD_CONNECTED:
    {
        const auto *info = static_cast<mesh_event_child_connected_t *>(data);
        set_child(info->mac, true);
        post(note_kind::child_joined, 0, info->mac);
        break;
    }
    case MESH_EVENT_CHILD_DISCONNECTED:
    {
        const auto *info =
            static_cast<mesh_event_child_disconnected_t *>(data);
        set_child(info->mac, false);
        post(note_kind::child_left, 0, info->mac);
        break;
    }
    case MESH_EVENT_ROUTING_TABLE_ADD:
    case MESH_EVENT_ROUTING_TABLE_REMOVE:
    {
        const auto *info =
            static_cast<mesh_event_routing_table_change_t *>(data);
        nodes_now.store(info->rt_size_new);
        post(note_kind::nodes_changed, info->rt_size_new);
        break;
    }
    case MESH_EVENT_NO_PARENT_FOUND:
    {
        const auto *info = static_cast<mesh_event_no_parent_found_t *>(data);
        post(note_kind::no_parent, info->scan_times);
        break;
    }
    case MESH_EVENT_LAYER_CHANGE:
    {
        const auto *info = static_cast<mesh_event_layer_change_t *>(data);
        layer_now.store(info->new_layer);
        post(note_kind::layer_changed, info->new_layer);
        break;
    }
    case MESH_EVENT_NETWORK_STATE:
    {
        const auto *info = static_cast<mesh_event_network_state_t *>(data);
        rootless_now.store(info->is_rootless);
        post(note_kind::rootless, info->is_rootless);
        break;
    }
    case MESH_EVENT_ROOT_ADDRESS:
    {
        const auto *info = static_cast<mesh_event_root_address_t *>(data);
        const network_config::device *root = find_board(info->addr);
        set_root(root ? root->id : 0, info->addr);
        break;
    }
    case MESH_EVENT_ROOT_ASKED_YIELD:
        if (root_role.load())
        {
            post(note_kind::asked_to_yield);
        }
        break;
    default:
        break;
    }
}

// Waits in esp_mesh_recv() and queues each frame for the loop. Cheap
// filters only, as ESP-NOW's receive callback; the loop authenticates.
// The wait is short so a restart never finds this task inside it.
void receive_task(void *)
{
    static uint8_t buffer[MESH_MPS];
    static received_frame frame;
    for (;;)
    {
        if (rx_pause.load())
        {
            rx_paused.store(true);
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        mesh_addr_t from = {};
        mesh_data_t data = {};
        data.data = buffer;
        data.size = sizeof(buffer);
        int flag = 0;
        const esp_err_t result = esp_mesh_recv(&from, &data,
            app_config::mesh_receive_wait_ms, &flag, nullptr, 0);
        if (result != ESP_OK)
        {
            if (result != ESP_ERR_MESH_TIMEOUT)
            {
                vTaskDelay(pdMS_TO_TICKS(10));  // Stopped, or not started.
            }
            continue;
        }
        if (data.size < secure_message::overhead ||
            data.size > secure_message::max_frame_size ||
            memcmp(from.addr, own_mac, 6) == 0)
        {
            continue;
        }
        if (!radio::find_device(from.addr))
        {
            unknown_drops.fetch_add(1);
            continue;
        }
        received.fetch_add(1);
        memcpy(frame.mac, from.addr, 6);
        frame.size = static_cast<uint8_t>(data.size);
        memcpy(frame.bytes, buffer, data.size);
        if (xQueueSend(receive_queue, &frame, 0) != pdTRUE)
        {
            rx_drops.fetch_add(1);
        }
    }
}

// Returns once the receive task is out of esp_mesh_recv() and will stay
// out: it sets rx_paused only after seeing rx_pause, at the top of its
// loop.
void pause_receiver()
{
    rx_paused.store(false);
    rx_pause.store(true);
    const uint32_t limit_ms = 5 * app_config::mesh_receive_wait_ms;
    for (uint32_t waited = 0; !rx_paused.load() && waited < limit_ms;
         waited += 5)
    {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void resume_receiver()
{
    rx_pause.store(false);
}

const char *fail(const char *step, esp_err_t result)
{
    failed_at = step;
    failure = result;
    broken.store(true);
    set_state(state::failed);
    Serial.printf("MESH %s failed: %s\n", step, esp_err_to_name(result));
    return step;
}

// The settings esp_mesh_init() resets, applied after every init. Returns
// the step that failed (with *result), or null.
const char *apply_settings(esp_err_t *result)
{
    if ((*result = esp_mesh_set_topology(MESH_TOPO_TREE)) != ESP_OK ||
        (*result = esp_mesh_set_max_layer(app_config::mesh_max_layer)) !=
            ESP_OK ||
        (*result = esp_mesh_set_xon_qsize(app_config::mesh_xon_qsize)) !=
            ESP_OK ||
        (*result = esp_mesh_set_ap_assoc_expire(
             app_config::mesh_assoc_expire_s)) != ESP_OK)
    {
        return "settings";
    }
    // Mesh power save holds frames for sleeping boards; voice can't wait.
    const esp_err_t power_save = esp_mesh_disable_ps();
    if (power_save != ESP_OK)
    {
        Serial.printf("MESH power save not disabled: %s\n",
            esp_err_to_name(power_save));
    }
    // No router: nobody votes for a root; restart(true) makes one.
    // Every board fixes the root (esp_mesh.h).
    if ((*result = esp_mesh_fix_root(true)) != ESP_OK)
    {
        return "fix root";
    }
    if ((*result = esp_mesh_set_ap_authmode(WIFI_AUTH_WPA2_PSK)) != ESP_OK)
    {
        return "soft-AP security";
    }
    return nullptr;
}
}

const char *start(const char *password)
{
    esp_wifi_get_mac(WIFI_IF_STA, own_mac);
    const network_config::device *self = find_board(own_mac);
    own_board = self ? self->id : 0;
    receive_queue = xQueueCreate(app_config::mesh_rx_queue_depth,
        sizeof(received_frame));
    note_queue = xQueueCreate(note_queue_depth, sizeof(note));
    if (!receive_queue || !note_queue)
    {
        return fail("queues", ESP_ERR_NO_MEM);
    }
    esp_err_t result = esp_mesh_init();
    if (result != ESP_OK)
    {
        return fail("init", result);
    }
    result = esp_event_handler_register(MESH_EVENT, ESP_EVENT_ANY_ID,
        on_event, nullptr);
    if (result != ESP_OK)
    {
        return fail("events", result);
    }
    const char *step = apply_settings(&result);
    if (step)
    {
        return fail(step, result);
    }
    mesh_cfg_t &config = saved_config;
    config = {};
    config.crypto_funcs = &g_wifi_default_mesh_crypto_funcs;
    config.channel = network_config::channel;
    config.allow_channel_switch = false;  // ESP-NOW is on this channel too.
    memcpy(config.mesh_id.addr, network_config::mesh_id, 6);
    config.router.ssid_len = 0;  // No router.
    config.router.allow_router_switch = false;
    config.mesh_ap.max_connection = app_config::mesh_ap_connections;
    config.mesh_ap.nonmesh_max_connection = 0;  // Only mesh boards join.
    const size_t password_size = strlen(password);
    if (password_size < 8 || password_size >= sizeof(config.mesh_ap.password))
    {
        return fail("soft-AP password", ESP_ERR_INVALID_ARG);
    }
    memcpy(config.mesh_ap.password, password, password_size);
    result = esp_mesh_set_config(&config);
    if (result == ESP_ERR_MESH_ARGUMENT)
    {
        // This IDF wants a router SSID: name one that exists nowhere.
        const size_t size = strlen(app_config::mesh_placeholder_router);
        config.router.ssid_len = static_cast<uint8_t>(size);
        memcpy(config.router.ssid, app_config::mesh_placeholder_router, size);
        result = esp_mesh_set_config(&config);
        placeholder = result == ESP_OK;
    }
    if (result != ESP_OK)
    {
        return fail("configure", result);
    }
    if (xTaskCreatePinnedToCore(receive_task, "mesh_rx",
            app_config::mesh_task_stack, nullptr, app_config::mesh_priority,
            nullptr, app_config::mesh_core) != pdPASS)
    {
        return fail("receive task", ESP_ERR_NO_MEM);
    }
    initialized = true;
    // Always a node first: a board that finds a mesh joins it rather than
    // starting a second one (mesh.inc makes it the root if none is found).
    root_role.store(false);
    set_state(state::joining);
    if ((result = esp_mesh_start()) != ESP_OK)
    {
        return fail("start", result);
    }
    broken.store(false);
    return nullptr;
}

const char *restart(bool root)
{
    if (!initialized)
    {
        return failed_at;
    }
    restarts.fetch_add(1);
    pause_receiver();
    esp_mesh_stop();  // Fine if it wasn't running.
    root_role.store(root);
    const mesh_type_t type = root ? MESH_ROOT : MESH_IDLE;
    esp_err_t result = esp_mesh_set_type(type);
    if (result == ESP_ERR_MESH_NOT_INIT)
    {
        // Here the stop also undid esp_mesh_init() (seen on boards 1-3,
        // 2026-11-05: becoming root failed with ESP_ERR_MESH_NOT_INIT).
        // Set the mesh up again, settings and all, as start() did.
        if ((result = esp_mesh_init()) != ESP_OK)
        {
            resume_receiver();
            return fail("init again", result);
        }
        const char *step = apply_settings(&result);
        if (step)
        {
            resume_receiver();
            return fail(step, result);
        }
        result = esp_mesh_set_type(type);
    }
    if (result != ESP_OK)
    {
        resume_receiver();
        return fail(root ? "become root" : "become a node", result);
    }
    result = esp_mesh_set_config(&saved_config);
    if (result != ESP_OK)
    {
        resume_receiver();
        return fail("configure", result);
    }
    set_state(state::joining);
    result = esp_mesh_start();
    resume_receiver();
    if (result != ESP_OK)
    {
        return fail("restart", result);
    }
    broken.store(false);
    return nullptr;
}

void stop()
{
    if (!initialized)
    {
        return;
    }
    restarts.fetch_add(1);
    pause_receiver();
    esp_mesh_stop();
    // The stop leaves the soft-AP up with default (open) settings; drop it.
    // ESP-NOW is on the station and stays. esp_mesh_start() brings the
    // soft-AP back.
    esp_wifi_set_mode(WIFI_MODE_STA);
    root_role.store(false);
    broken.store(false);
    set_state(state::off);
    resume_receiver();  // Its esp_mesh_recv() fails until a restart.
}

bool can_restart()
{
    return initialized;
}

state current()
{
    return broken.load() ? state::failed :
        static_cast<state>(status.load());
}

uint32_t state_since_ms()
{
    return status_ms.load();
}

const char *state_name(state value)
{
    switch (value)
    {
    case state::off: return "off";
    case state::joining: return "joining";
    case state::connected: return "connected";
    case state::failed: return "failed";
    }
    return "?";
}

bool is_root()
{
    return root_role.load();
}

bool rootless()
{
    return rootless_now.load();
}

uint8_t root_board()
{
    return root_id.load();
}

int layer()
{
    return layer_now.load();
}

int children()
{
    return __builtin_popcount(children_set.load());
}

uint32_t children_boards()
{
    return children_set.load();
}

uint8_t parent_board()
{
    return parent_id.load();
}

int nodes()
{
    return nodes_now.load();
}

bool placeholder_router()
{
    return placeholder;
}

const char *failed_step()
{
    return failed_at;
}

esp_err_t last_error()
{
    return failure;
}

const network_config::device *find_board(const uint8_t *mac)
{
    const network_config::device *entry = radio::find_device(mac);
    if (entry)
    {
        return entry;
    }
    // A soft-AP MAC (a parent's BSSID, the root's address). This sdkconfig
    // has two universal MACs (station, Bluetooth), so esp_read_mac() gives
    // the soft-AP a locally administered one derived from the station MAC
    // (E0:... becomes E2:... on these boards); derive each board's the same
    // way. Seen on hardware 2026-11-05: the last-byte + 1 rule of four-MAC
    // chips named no parent (board 2's parent, board 1, was
    // E2:72:A1:D8:E2:10).
    for (const network_config::device &board : network_config::devices)
    {
        // radio's entry, not this one: the constexpr table has a copy in
        // every file that includes it, so compare by ID, never by address.
        const network_config::device *listed = radio::find_device(board.id);
        uint8_t soft_ap[6];
        if (listed && esp_derive_local_mac(soft_ap, listed->mac) == ESP_OK &&
            memcmp(soft_ap, mac, 6) == 0)
        {
            return listed;
        }
    }
    return nullptr;
}

esp_err_t send(const uint8_t *mac, const uint8_t *frame, size_t size)
{
    if (current() != state::connected)
    {
        send_failed.fetch_add(1);
        return ESP_ERR_MESH_DISCONNECTED;
    }
    mesh_addr_t to = {};
    memcpy(to.addr, mac, 6);
    mesh_data_t data = {};
    data.data = const_cast<uint8_t *>(frame);
    data.size = static_cast<uint16_t>(size);
    data.proto = MESH_PROTO_BIN;
    data.tos = MESH_TOS_P2P;  // Each hop retries, as ESP-NOW's MAC does.
    // esp_mesh.h: to the root, no address and no flag; to any other board,
    // its address and P2P (the root forwards it down if need be). The root
    // is whichever board is root now, not the preferred one.
    const uint8_t root = root_id.load();
    const network_config::device *root_entry =
        root ? radio::find_device(root) : nullptr;
    const bool to_root = !root_role.load() && root_entry &&
        memcmp(mac, root_entry->mac, 6) == 0;
    const esp_err_t result = esp_mesh_send(to_root ? nullptr : &to, &data,
        (to_root ? 0 : MESH_DATA_P2P) | MESH_DATA_NONBLOCK, nullptr, 0);
    (result == ESP_OK ? sent : send_failed).fetch_add(1);
    return result;
}

bool busy(esp_err_t result)
{
    return result == ESP_ERR_MESH_QUEUE_FULL ||
        result == ESP_ERR_MESH_NO_MEMORY ||
        result == ESP_ERR_MESH_XON_NO_WINDOW;
}

bool receive(received_frame *frame)
{
    return receive_queue && xQueueReceive(receive_queue, frame, 0) == pdTRUE;
}

bool next_note(note *item)
{
    return note_queue && xQueueReceive(note_queue, item, 0) == pdTRUE;
}

counters stats()
{
    return {sent.load(), send_failed.load(), received.load(), rx_drops.load(),
        unknown_drops.load(), parent_joins.load(), parent_losses.load(),
        router_misses.load(), restarts.load(), note_drops.load()};
}
}
