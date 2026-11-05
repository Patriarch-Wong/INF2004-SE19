#pragma once

#include <esp_err.h>
#include <stddef.h>
#include <stdint.h>
#include "network_config.h"
#include "secure_message.h"

// ESP-MESH (ESP-WIFI-MESH, esp_mesh.h) on the Wi-Fi that radio::start()
// brought up, alongside ESP-NOW on the same channel. Frames are whole
// secure_message frames, as over ESP-NOW; the mesh carries them hop by hop
// to the board with the destination's station MAC.
//
// No router. ESP-WIFI-MESH elects as root the board nearest the router;
// with none there is nothing to vote on, so every board fixes the root
// (esp_mesh_fix_root(true)), the router SSID is left empty (as ESP-MDF's
// router-less mode does), and the application decides who is root:
// restart(true) makes this board the root (esp_mesh_set_type(MESH_ROOT)),
// restart(false) a node that joins whatever mesh it finds. mesh.inc picks
// (root failover). The root reports each failed look for the absent
// router as a parent disconnect; that is counted (router_misses) and
// otherwise ignored.
//
// A task waits in esp_mesh_recv() and queues each frame from an
// allowlisted MAC for the loop; the mesh's events (joined, lost the parent,
// a child came or went) are queued as notes. Neither prints: the loop does.
// send(), restart() and stop() are for the loop only (esp_mesh_send() is
// not reentrant, and a restart pauses the receive task first).
namespace mesh_radio
{
enum class state : uint8_t
{
    off,        // Not started, or stopped (`mesh off`).
    joining,    // Looking for a mesh to join.
    connected,  // In the mesh: the root, or joined to a parent.
    failed,     // A start or restart failed; see failed_step().
};

struct received_frame
{
    uint8_t mac[6];  // Station MAC of the board that sent it.
    uint8_t size;
    uint8_t bytes[secure_message::max_frame_size];
};

enum class note_kind : uint8_t
{
    started,          // value: 1 if this board is the root.
    parent_joined,    // value: layer; mac: the parent's soft-AP (BSSID).
    parent_lost,      // value: Wi-Fi disconnect reason.
    child_joined,     // mac: the child's station MAC.
    child_left,       // mac: the child's station MAC.
    nodes_changed,    // value: boards in this board's part of the mesh.
    no_parent,        // value: scans made without finding a parent.
    layer_changed,    // value: new layer (1 is the root).
    rootless,         // value: 1 the mesh has no root now, 0 it has one.
    root_found,       // value: the root's board ID (0 if not listed).
    router_missing,   // Root only, once per start: no router, as intended.
    asked_to_yield,   // Root only: another root wants this one to give way.
    stopped,
};

struct note
{
    note_kind kind;
    int16_t value;
    uint8_t mac[6];
};

struct counters
{
    uint32_t sent;
    uint32_t send_failed;
    uint32_t received;
    uint32_t rx_drops;       // Receive queue full.
    uint32_t unknown_drops;  // From a MAC not in network_config.h.
    uint32_t parent_joins;
    uint32_t parent_losses;
    uint32_t router_misses;  // Root: looks for the absent router.
    uint32_t restarts;       // restart() and stop() calls.
    uint32_t note_drops;
};

// Sets the mesh up and starts it as a node, looking for a mesh to join.
// `password` is the mesh soft-AP's WPA2 password, 8..63 characters, the
// same on every board. Returns null on success, or what failed (also kept
// for failed_step()).
const char *start(const char *password);
// Stops the mesh and starts it again as the root or as a node. Children
// and the parent are dropped; they find their way back. Returns null on
// success, or what failed.
const char *restart(bool root);
// Leaves the mesh, as if this board were off; restart() comes back.
void stop();
// start() got far enough for restart() to work.
bool can_restart();

state current();
// millis() when the current state began.
uint32_t state_since_ms();
const char *state_name(state value);
// This board is the root now.
bool is_root();
// The mesh this board is in says it has no root (its root went away).
bool rootless();
// Board ID of the root of this board's mesh; 0 if not known.
uint8_t root_board();
// Hops from the root (1 is the root); 0 until joined.
int layer();
// Boards joined to this board's soft-AP: how many, and which (bit N is
// board N).
int children();
uint32_t children_boards();
// Board ID of this board's parent; 0 for the root, or if not known.
uint8_t parent_board();
// Boards in this board's part of the mesh, itself included: the whole mesh
// on the root.
int nodes();
// The router SSID was refused empty, so a name that exists nowhere is set.
bool placeholder_router();
const char *failed_step();
esp_err_t last_error();

// The board with this station MAC, or with this soft-AP MAC (a parent's
// BSSID, the root's address; derived from the station MAC as
// esp_read_mac() does). Null if not listed.
const network_config::device *find_board(const uint8_t *mac);

// Sends to the board with this station MAC, without blocking. ESP_OK only
// says the mesh queued it.
esp_err_t send(const uint8_t *mac, const uint8_t *frame, size_t size);
// The mesh's queues are full: try again later rather than counting a loss.
bool busy(esp_err_t result);

// Next received frame, or event note, without waiting.
bool receive(received_frame *frame);
bool next_note(note *item);

counters stats();
}
