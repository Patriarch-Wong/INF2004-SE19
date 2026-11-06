// Included inside main.cpp's anonymous namespace, after handset.inc.
//
// ESP-MESH connections: everything ESP-NOW has (handset.inc, and main.cpp's
// sessions), over ESP-WIFI-MESH (mesh_radio.h) instead of directly. One
// board asks, the other accepts or declines; while connected, hold OK to
// talk (Opus voice) and RIGHT says hello; either side disconnects; silence
// drops both. The voice and text code is handset.inc's, told to use the
// mesh (via::mesh).
//
// No router, so the boards decide who is root (root failover, below): the
// preferred root (network_config::mesh_root_id) when it is up, else the
// next board in ID order that is. The others join the root, or each other,
// so two boards out of each other's range can talk through a third. Any
// board can go down: its connections end, the others carry on, and if it
// was the root another takes over. ESP-NOW and LoRa don't depend on the
// mesh.
//
// Everything is sealed end to end as over ESP-NOW (secure_message.h),
// under a mesh session key of its own per peer, made by the same handshake
// (session.h) over the mesh: a frame recorded on one transport does not
// open on another. The mesh soft-AP is WPA2 with a password derived from
// the network key, so only these boards join.
//
// Presence pings as over ESP-NOW. The mesh doesn't tell the sender a frame
// arrived, so a board is offline when its pings stop (presence_timeout_ms)
// or sends to it keep failing (this board left the mesh, or the mesh has
// no route to it).

struct mesh_peer
{
    session::link link;
    uint32_t last_hello_ms = 0;
    uint32_t hellos = 0;  // Sent since the handshake started.
    uint32_t last_accept_ms = 0;
    uint32_t last_resync_ms = 0;
    uint32_t last_presence_ms = 0;  // Last presence ping sent.
    bool shown_online = false;      // Last state logged as MESH PEER.
    uint32_t send_failures = 0;     // In a row; a send that works resets.
    uint32_t last_send_ok_ms = 0;
    // Where the board hangs in the mesh, from its last presence ping: its
    // parent (0 for the root) and layer. For the route on screen.
    bool placed = false;
    uint8_t parent = 0;
    uint8_t layer = 0;
    uint32_t placed_ms = 0;
};

mesh_peer mesh_peers[max_id + 1];  // Indexed by device ID.
uint32_t mesh_control_sequence = 0;
uint32_t mesh_accepted = 0;
uint32_t mesh_rejected = 0;
mesh_radio::state mesh_shown_state = mesh_radio::state::off;

// Serial `mesh mute on|off`: drops every mesh frame sent and received, as
// out of range would (the mesh itself stays up), for link-loss tests.
bool mesh_muted = false;
uint32_t mesh_muted_tx = 0;
uint32_t mesh_muted_rx = 0;

bool mesh_ready()
{
    return mesh_radio::current() == mesh_radio::state::connected;
}

bool mesh_can_send(uint8_t peer)
{
    return mesh_peers[peer].link.established();
}

// Sends to `peer` failed several times in a row, with none working for
// ack_lost_ms: as ESP-NOW's missing acknowledgements (acks_lost). Signed:
// the last good send can be stamped after the loop's `now`.
bool mesh_sends_failing(uint8_t peer, uint32_t now)
{
    const mesh_peer &state = mesh_peers[peer];
    return state.send_failures >= app_config::ack_lost_failures &&
        static_cast<int32_t>(now - state.last_send_ok_ms) >=
            static_cast<int32_t>(app_config::ack_lost_ms);
}

// A mesh session is up, something from the board opened recently, and
// sends to it still work.
bool mesh_peer_online(uint8_t peer, uint32_t now)
{
    return mesh_can_send(peer) &&
        static_cast<int32_t>(now - mesh_peers[peer].last_accept_ms) <
            static_cast<int32_t>(app_config::presence_timeout_ms) &&
        !mesh_sends_failing(peer, now);
}

// Routes
//
// The mesh is a tree, so frames between two boards go up from one to the
// nearest board both hang from, then down to the other. Each board knows
// only its own parent; the others' come in their presence pings (sealed
// like everything else, so a route can't be faked). This works out the
// path from that, for the connected screen and the logs.

// The board `id` hangs from (0 for the root), or false if not known: not
// heard from lately, or its parent isn't a listed board.
bool mesh_parent_of(uint8_t id, uint32_t now, uint8_t *parent)
{
    if (id == own_id)
    {
        if (!mesh_ready())
        {
            return false;
        }
        *parent = mesh_radio::is_root() ? 0 : mesh_radio::parent_board();
        return mesh_radio::is_root() || *parent != 0;
    }
    if (id < 1 || id > max_id)
    {
        return false;
    }
    const mesh_peer &state = mesh_peers[id];
    if (!state.placed ||
        static_cast<int32_t>(now - state.placed_ms) >=
            static_cast<int32_t>(app_config::presence_timeout_ms))
    {
        return false;
    }
    *parent = state.layer == 1 ? 0 : state.parent;
    return state.layer == 1 || *parent != 0;
}

// The boards from `id` up to the root, `id` first; 0 if a step is unknown.
size_t mesh_chain(uint8_t id, uint32_t now, uint8_t *chain, size_t capacity)
{
    size_t length = 0;
    while (length < capacity)
    {
        chain[length++] = id;
        uint8_t parent = 0;
        if (!mesh_parent_of(id, now, &parent))
        {
            return 0;
        }
        if (parent == 0)
        {
            return length;  // Reached the root.
        }
        id = parent;
    }
    return 0;  // Deeper than the mesh allows: stale pings disagree.
}

// The boards frames pass between this board and `peer`, this board first
// and the peer last; 0 if not known yet.
size_t mesh_route(uint8_t peer, uint32_t now, uint8_t *path, size_t capacity)
{
    constexpr size_t deepest = app_config::mesh_max_layer;
    uint8_t mine[deepest];
    uint8_t theirs[deepest];
    const size_t up = mesh_chain(own_id, now, mine, deepest);
    const size_t down = mesh_chain(peer, now, theirs, deepest);
    if (up == 0 || down == 0)
    {
        return 0;
    }
    // The first board on this board's way up that is on the peer's too.
    for (size_t i = 0; i < up; ++i)
    {
        for (size_t j = 0; j < down; ++j)
        {
            if (mine[i] != theirs[j])
            {
                continue;
            }
            if (i + j + 1 > capacity)
            {
                return 0;
            }
            size_t length = 0;
            for (size_t k = 0; k <= i; ++k)
            {
                path[length++] = mine[k];
            }
            for (size_t k = j; k-- > 0;)
            {
                path[length++] = theirs[k];
            }
            return length;
        }
    }
    return 0;  // Different roots: not one mesh (yet).
}

// A path of board IDs, this board first: "1 > 3 > 2, via Board 3",
// "1 > 2, direct", or "finding..." while not known (shorter than 2). Also
// the ESP-NOW multi-hop route lines (relay.inc).
void path_text(const uint8_t *path, size_t length, char *text,
    size_t capacity)
{
    if (length < 2)
    {
        snprintf(text, capacity, "finding...");
        return;
    }
    size_t used = 0;
    for (size_t i = 0; i < length && used < capacity; ++i)
    {
        used += snprintf(text + used, capacity - used, "%s%u",
            i == 0 ? "" : " > ", path[i]);
    }
    if (used >= capacity)
    {
        return;
    }
    if (length == 2)
    {
        snprintf(text + used, capacity - used, ", direct");
    }
    else if (length == 3)
    {
        snprintf(text + used, capacity - used, ", via Board %u", path[1]);
    }
    else
    {
        snprintf(text + used, capacity - used, ", via %u boards",
            static_cast<unsigned>(length - 2));
    }
}

void mesh_route_text(uint8_t peer, uint32_t now, char *text, size_t capacity)
{
    uint8_t path[2 * app_config::mesh_max_layer];
    const size_t length = mesh_route(peer, now, path, sizeof(path));
    path_text(path, length, text, capacity);
}

// A board's ID and name from its station or soft-AP MAC, else the MAC.
void mesh_print_board(const uint8_t *mac)
{
    const network_config::device *entry = mesh_radio::find_board(mac);
    if (entry)
    {
        Serial.printf("%u (%s)", entry->id, entry->name);
    }
    else
    {
        radio::print_mac(mac);
    }
}

void mesh_reject(const mesh_radio::received_frame &frame, const char *reason)
{
    ++mesh_rejected;
    Serial.print("MESH REJECT ");
    radio::print_mac(frame.mac);
    Serial.printf(" | %u bytes | %s\n", frame.size, reason);
}

// Every mesh frame goes out here: mute, and the run of failures per peer.
esp_err_t mesh_transmit(uint8_t peer, const uint8_t *frame, size_t size)
{
    if (mesh_muted)
    {
        ++mesh_muted_tx;
        return ESP_OK;  // Gone, as out of range: presence and timeouts notice.
    }
    const esp_err_t result = mesh_radio::send(radio::find_device(peer)->mac,
        frame, size);
    mesh_peer &state = mesh_peers[peer];
    if (result == ESP_OK)
    {
        state.send_failures = 0;
        state.last_send_ok_ms = millis();
    }
    else if (!mesh_radio::busy(result))
    {
        ++state.send_failures;
    }
    return result;
}

void mesh_send_control(uint8_t destination,
    const control_frame::message &content)
{
    if (++mesh_control_sequence == 0)
    {
        mesh_control_sequence = 1;
    }
    const secure_message::header fields = {secure_message::version,
        control_frame::type_session, own_id, destination,
        mesh_control_sequence};
    uint8_t frame[secure_message::max_frame_size];
    size_t size = 0;
    const status built = control_frame::build(e2e_keys::network, fields,
        content, frame, sizeof(frame), &size);
    if (built != status::ok)
    {
        Serial.printf("MESH control frame not built: %s\n",
            secure_message::status_name(built));
        return;
    }
    mesh_transmit(destination, frame, size);
}

esp_err_t mesh_send_payload(uint8_t peer, const uint8_t *payload,
    size_t size, uint8_t type)
{
    uint8_t frame[secure_message::max_frame_size] = {};
    size_t frame_size = 0;
    uint32_t counter = 0;
    const status sealed = mesh_peers[peer].link.seal(e2e_keys::network, type,
        payload, size, frame, sizeof(frame), &frame_size, &counter);
    if (sealed != status::ok)
    {
        Serial.printf("MESH SEAL_ERROR %s\n",
            secure_message::status_name(sealed));
        return ESP_FAIL;
    }
    return mesh_transmit(peer, frame, frame_size);
}

// Sessions, as main.cpp's ESP-NOW ones

void mesh_start_session(uint8_t peer, uint32_t now)
{
    uint8_t nonce[session::nonce_size];
    esp_fill_random(nonce, sizeof(nonce));
    control_frame::message hello = {};
    if (mesh_peers[peer].link.initiate(nonce, &hello))
    {
        mesh_send_control(peer, hello);
        mesh_peers[peer].last_hello_ms = now;
        mesh_peers[peer].hellos = 1;
        Serial.printf("MESH SESSION hello -> %u\n", peer);
    }
}

// This board is in the mesh (joined, or formed it as root): make a session
// with every paired board that has none, and hurry a handshake already
// waiting, whose hellos went nowhere while this board was out.
void mesh_start_sessions(uint32_t now)
{
    for (uint8_t peer = 1; peer <= max_id; ++peer)
    {
        mesh_peer &state = mesh_peers[peer];
        if (!state.link.paired() || state.link.established())
        {
            continue;
        }
        if (!state.link.initiating())
        {
            mesh_start_session(peer, now);
        }
        else
        {
            state.hellos = 0;
            state.last_hello_ms = now - app_config::session_retry_ms;
        }
    }
}

void service_mesh_sessions(uint32_t now)
{
    for (uint8_t peer = 1; peer <= max_id; ++peer)
    {
        mesh_peer &state = mesh_peers[peer];
        control_frame::message hello = {};
        // Fast at first, then slowly, as over ESP-NOW (ISSUES.md B1).
        const bool slow = state.hellos >= app_config::session_fast_hellos;
        const uint32_t period = slow ? app_config::session_slow_retry_ms :
            app_config::session_retry_ms;
        if (state.link.initiating() && now - state.last_hello_ms >= period &&
            state.link.hello(&hello))
        {
            mesh_send_control(peer, hello);
            state.last_hello_ms = now;
            ++state.hellos;
            if (state.hellos == app_config::session_fast_hellos)
            {
                Serial.printf("MESH SESSION %u not answering: retrying "
                              "every %lu s\n",
                    peer, static_cast<unsigned long>(
                        app_config::session_slow_retry_ms / 1000));
            }
            else if (!slow)
            {
                Serial.printf("MESH SESSION hello -> %u (retry)\n", peer);
            }
        }
    }
}

// A paired peer whose frames keep failing to open, with nothing accepted
// from it for a while, gets a new handshake in case the two lost step.
void mesh_maybe_resync(uint8_t peer, uint32_t now)
{
    mesh_peer &state = mesh_peers[peer];
    if (!state.link.initiating() &&
        now - state.last_accept_ms >= app_config::session_resync_ms &&
        now - state.last_resync_ms >= app_config::session_resync_ms)
    {
        state.last_resync_ms = now;
        Serial.printf("MESH SESSION resync with %u\n", peer);
        mesh_start_session(peer, now);
    }
}

void mesh_print_session(const char *how, uint8_t peer)
{
    Serial.printf("MESH SESSION established with %u | session %08lX | %s\n",
        peer, static_cast<unsigned long>(mesh_peers[peer].link.session_id()),
        how);
}

void handle_mesh_session(const mesh_radio::received_frame &frame,
    const secure_message::header &fields,
    const control_frame::message &content, uint32_t now)
{
    mesh_peer &state = mesh_peers[fields.source];
    if (!state.link.paired())
    {
        mesh_reject(frame, "no pair key for source");
        return;
    }
    if (content.step == static_cast<uint8_t>(session::step::hello) &&
        state.link.initiating())
    {
        // The peer is back. If this board has the lower ID it ignores the
        // hello and its own must win, so send it now.
        state.hellos = 0;
        state.last_hello_ms = now - app_config::session_retry_ms;
    }
    uint8_t nonce[session::nonce_size];
    esp_fill_random(nonce, sizeof(nonce));
    control_frame::message response = {};
    switch (state.link.receive(content, nonce, &response))
    {
    case session::event::replied:
        mesh_send_control(fields.source, response);
        Serial.printf("MESH SESSION reply -> %u\n", fields.source);
        break;
    case session::event::established:
        mesh_send_control(fields.source, response);
        mesh_print_session("initiator", fields.source);
        state.last_accept_ms = now;  // Online now.
        break;
    case session::event::confirmed:
        mesh_print_session("responder", fields.source);
        state.last_accept_ms = now;
        break;
    case session::event::bad_tag:
        mesh_reject(frame, "bad handshake tag");
        break;
    case session::event::ignored:
        break;
    }
}

// Receive

void handle_mesh(const mesh_radio::received_frame &frame, uint32_t now)
{
    secure_message::header fields = {};
    const status verified = secure_message::verify_hop(e2e_keys::network,
        frame.bytes, frame.size, &fields);
    if (verified != status::ok)
    {
        mesh_reject(frame, secure_message::status_name(verified));
        return;
    }
    if (fields.source == own_id)
    {
        ++own_id_claims;
        return;
    }
    if (!valid_peer(fields.source))
    {
        mesh_reject(frame, "unknown source ID");
        return;
    }
    if (fields.destination != own_id)
    {
        mesh_reject(frame, "not addressed to this device");
        return;
    }
    mesh_peer &state = mesh_peers[fields.source];
    if (control_frame::is_control(fields.type))
    {
        control_frame::message content = {};
        const status parsed = control_frame::parse(e2e_keys::network,
            frame.bytes, frame.size, &fields, &content);
        if (parsed != status::ok)
        {
            mesh_reject(frame, secure_message::status_name(parsed));
        }
        else if (fields.type != control_frame::type_session)
        {
            mesh_reject(frame, "pairing not used: keys are compiled in");
        }
        else
        {
            handle_mesh_session(frame, fields, content, now);
        }
        return;
    }
    const int64_t arrival = esp_timer_get_time();
    uint8_t payload[secure_message::max_payload_size];
    size_t size = 0;
    secure_message::header opened = fields;
    const auto result = state.link.open(e2e_keys::network, frame.bytes,
        frame.size, &opened, payload, sizeof(payload), &size);
    if (result != session::open_result::accepted &&
        result != session::open_result::accepted_new_session)
    {
        mesh_reject(frame, session::open_result_name(result));
        mesh_maybe_resync(fields.source, now);
        return;
    }
    ++mesh_accepted;
    state.last_accept_ms = now;
    // Over the mesh a presence ping also says where the board hangs:
    // parent (0 for the root), then layer.
    if (opened.type == secure_message::type_voice && size >= 3 &&
        voice_link::kind(payload, size) == kind_presence)
    {
        state.placed = true;
        state.parent = payload[1];
        state.layer = payload[2];
        state.placed_ms = now;
    }
    const char *problem = take_payload(via::mesh, fields.source, opened.type,
        payload, size, arrival, now);
    if (problem)
    {
        mesh_reject(frame, problem);
    }
}

// Connections

// Asks `peer` to connect over the mesh; false (with the reason logged) if
// it cannot.
bool place_mesh_call(uint8_t peer, uint32_t now)
{
    if (!valid_peer(peer))
    {
        Serial.printf("MESH LINK refused: no handset %u\n", peer);
        return false;
    }
    if (!mesh_ready())
    {
        Serial.printf("MESH LINK refused: not in the mesh (%s)\n",
            mesh_radio::state_name(mesh_radio::current()));
        return false;
    }
    if (calls.current() != call::state::idle)
    {
        Serial.println("MESH LINK refused: an ESP-NOW connection is in "
                       "progress");
        return false;
    }
    if (lora_in_use())
    {
        Serial.println("MESH LINK refused: a LoRa connection is in progress");
        return false;
    }
    if (busy_elsewhere(mesh_calls))
    {
        Serial.println("MESH LINK refused: a flood mesh or routed relay "
                       "connection is in progress");
        return false;
    }
    if (!mesh_peer_online(peer, now))
    {
        Serial.printf("MESH LINK refused: %u is offline (%s)\n", peer,
            mesh_can_send(peer) ? "no presence ping for a while" :
            "no mesh session");
        return false;
    }
    const uint32_t id = random_id();
    mesh_calls.set_unavailable(false);
    if (!mesh_calls.dial(peer, id, now))
    {
        Serial.printf("MESH LINK refused: already %s\n",
            call::state_name(mesh_calls.current()));
        return false;
    }
    return true;
}

// Where this board hung when it last pinged, so a move pings at once.
uint8_t mesh_pinged_parent = 0;
uint8_t mesh_pinged_layer = 0;

// Pings every board with a mesh session and logs when one comes or goes.
// Each ping carries this board's parent and layer (the peers' routes).
void service_mesh_presence(uint32_t now)
{
    const uint8_t parent = mesh_radio::is_root() ? 0 :
        mesh_radio::parent_board();
    const uint8_t layer = static_cast<uint8_t>(mesh_radio::layer());
    const bool moved = parent != mesh_pinged_parent ||
        layer != mesh_pinged_layer;
    mesh_pinged_parent = parent;
    mesh_pinged_layer = layer;
    for (uint8_t peer = 1; peer <= max_id; ++peer)
    {
        if (!valid_peer(peer))
        {
            continue;
        }
        mesh_peer &state = mesh_peers[peer];
        const uint32_t period = state.shown_online ?
            app_config::presence_ping_ms : app_config::presence_offline_ping_ms;
        if (mesh_ready() && mesh_can_send(peer) &&
            (moved || now - state.last_presence_ms >= period))
        {
            state.last_presence_ms = now;
            const uint8_t payload[] = {kind_presence, parent, layer};
            mesh_send_payload(peer, payload, sizeof(payload),
                secure_message::type_voice);  // Best effort.
        }
        const bool online = mesh_peer_online(peer, now);
        if (online != state.shown_online)
        {
            state.shown_online = online;
            Serial.printf("MESH PEER %u (%s) %s\n", peer, device_name(peer),
                online ? "online" : "offline");
        }
    }
}

// Root failover
//
// The mesh needs exactly one root, and with no router nobody can vote for
// one, so the boards take turns in a fixed order: the preferred root
// (network_config::mesh_root_id), then the other listed boards by ID. A
// board always starts as a node and looks for a mesh to join. If it finds
// none, or its mesh lost its root, for its turn's wait (app_config
// mesh_root_wait_ms plus mesh_root_wait_step_ms per place), it restarts as
// the root and the others join it. So:
//   - all boards on together: the preferred root forms the mesh first;
//   - the root goes down: the first board in the order still up takes over
//     and the rest join it; connections over the mesh end ("Connection
//     lost") and can be made again once it has;
//   - a board comes back: it finds the mesh and joins, never a second root;
//   - two roots anyway (one became root out of range of the others): the
//     one nobody joins steps down after mesh_lonely_root_ms and joins the
//     other mesh.
// A restart stops the mesh for a moment, so none starts while voice plays
// or is sent; the lonely-root one also waits for every connection to end.

bool mesh_switched_off = false;  // `mesh off`: out, as if the board were down.
bool mesh_looking = false;       // No mesh with a root since mesh_looking_since.
uint32_t mesh_looking_since = 0;
bool mesh_alone = false;         // Root, nobody joined since mesh_alone_since.
uint32_t mesh_alone_since = 0;
uint32_t mesh_alone_limit = 0;
bool mesh_yield_asked = false;   // Another root asked this one to give way.
uint32_t mesh_last_restart_ms = 0;
uint32_t mesh_no_parent_logged_ms = 0;
bool mesh_no_parent_logged = false;

// Place in the takeover order: 0 for the preferred root, then the other
// listed boards by ID.
uint32_t mesh_rank(uint8_t id)
{
    if (id == network_config::mesh_root_id)
    {
        return 0;
    }
    uint32_t rank = 1;
    for (uint8_t other = 1; other < id; ++other)
    {
        if (other != network_config::mesh_root_id && radio::find_device(other))
        {
            ++rank;
        }
    }
    return rank;
}

uint32_t mesh_takeover_wait_ms()
{
    return app_config::mesh_root_wait_ms +
        mesh_rank(own_id) * app_config::mesh_root_wait_step_ms;
}

// Not in a mesh, or in one whose root went away.
bool mesh_without_root()
{
    const mesh_radio::state state = mesh_radio::current();
    return state == mesh_radio::state::joining ||
        (state == mesh_radio::state::connected && !mesh_radio::is_root() &&
            mesh_radio::rootless());
}

// How long until this board becomes the root if it still finds no mesh;
// 0 when it isn't looking.
uint32_t mesh_takeover_in_ms(uint32_t now)
{
    if (!mesh_looking)
    {
        return 0;
    }
    const uint32_t waited = now - mesh_looking_since;
    const uint32_t wait = mesh_takeover_wait_ms();
    return waited < wait ? wait - waited : 0;
}

// A mesh restart stops the Wi-Fi mesh for a moment: not under live voice.
bool mesh_voice_running()
{
    return tx != tx_phase::idle || rx_play_at_us.load() != 0;
}

void mesh_restart(bool root, uint32_t now)
{
    mesh_last_restart_ms = now;
    mesh_looking = false;
    mesh_alone = false;
    mesh_yield_asked = false;
    const uint32_t began = millis();
    const char *error = mesh_radio::restart(root);
    Serial.printf("MESH restarted as %s in %lu ms%s%s\n", root ? "root" : "node",
        static_cast<unsigned long>(millis() - began),
        error ? " | failed at " : "", error ? error : "");
}

void service_mesh_failover(uint32_t now)
{
    if (mesh_switched_off || !mesh_radio::can_restart())
    {
        return;
    }
    const mesh_radio::state state = mesh_radio::current();
    if (state == mesh_radio::state::failed)
    {
        // Try the same role again; ESP-NOW and LoRa work meanwhile.
        if (now - mesh_last_restart_ms >= app_config::mesh_retry_ms &&
            !mesh_voice_running())
        {
            Serial.printf("MESH trying again (failed at %s: %s)\n",
                mesh_radio::failed_step(),
                esp_err_to_name(mesh_radio::last_error()));
            mesh_restart(mesh_radio::is_root(), now);
        }
        return;
    }
    if (state == mesh_radio::state::off)
    {
        return;  // Between a restart's stop and its start.
    }
    // Takeover: no mesh with a root for this board's turn's wait.
    const bool looking = mesh_without_root();
    if (looking && !mesh_looking)
    {
        mesh_looking = true;
        mesh_looking_since = now;
    }
    else if (!looking)
    {
        mesh_looking = false;
    }
    if (mesh_looking && mesh_takeover_in_ms(now) == 0 && !mesh_voice_running())
    {
        Serial.printf("MESH no mesh with a root for %lu s (this board's turn "
                      "comes after %lu s): becoming the root\n",
            static_cast<unsigned long>((now - mesh_looking_since) / 1000),
            static_cast<unsigned long>(mesh_takeover_wait_ms() / 1000));
        mesh_restart(true, now);
        return;
    }
    // A root with nobody joined: maybe there is another mesh to join.
    const bool alone = state == mesh_radio::state::connected &&
        mesh_radio::is_root() && mesh_radio::children() == 0 &&
        mesh_radio::nodes() <= 1;
    if (alone && !mesh_alone)
    {
        mesh_alone = true;
        mesh_alone_since = now;
        mesh_alone_limit = app_config::mesh_lonely_root_ms +
            esp_random() % app_config::mesh_lonely_jitter_ms;
    }
    else if (!alone)
    {
        mesh_alone = false;
    }
    const bool connections_idle = !any_connection();
    if (mesh_alone && now - mesh_alone_since >= mesh_alone_limit &&
        connections_idle && !mesh_voice_running())
    {
        Serial.printf("MESH root with nobody joined for %lu s: looking for "
                      "another mesh to join\n",
            static_cast<unsigned long>((now - mesh_alone_since) / 1000));
        mesh_restart(false, now);
        return;
    }
    if (mesh_yield_asked && mesh_radio::is_root() && !mesh_voice_running())
    {
        Serial.println("MESH giving way to the other root: joining its mesh");
        mesh_restart(false, now);
    }
}

// The mesh itself

void print_mesh_note(const mesh_radio::note &item)
{
    using mesh_radio::note_kind;
    const uint32_t now = millis();
    switch (item.kind)
    {
    case note_kind::started:
        mesh_no_parent_logged = false;
        if (item.value)
        {
            Serial.printf("MESH started as root (no router) | channel %u | "
                          "others join this board\n",
                network_config::channel);
        }
        else
        {
            Serial.printf("MESH started | looking for a mesh to join; this "
                          "board becomes the root after %lu s if none\n",
                static_cast<unsigned long>(mesh_takeover_wait_ms() / 1000));
        }
        break;
    case note_kind::parent_joined:
        Serial.print("MESH joined | parent ");
        mesh_print_board(item.mac);
        Serial.printf(" | layer %d\n", item.value);
        break;
    case note_kind::parent_lost:
        Serial.printf("MESH left the mesh | Wi-Fi reason %d | looking for a "
                      "parent\n",
            item.value);
        break;
    case note_kind::child_joined:
        Serial.print("MESH child joined: ");
        mesh_print_board(item.mac);
        Serial.println();
        break;
    case note_kind::child_left:
        Serial.print("MESH child left: ");
        mesh_print_board(item.mac);
        Serial.println();
        break;
    case note_kind::nodes_changed:
        Serial.printf("MESH routing table: %d board%s, this one included\n",
            item.value, item.value == 1 ? "" : "s");
        break;
    case note_kind::no_parent:
        // Every scan round while looking: log now and then.
        if (!mesh_no_parent_logged ||
            now - mesh_no_parent_logged_ms >= app_config::mesh_no_parent_log_ms)
        {
            mesh_no_parent_logged = true;
            mesh_no_parent_logged_ms = now;
            Serial.printf("MESH no mesh found yet (%d scans) | this board "
                          "becomes the root in %lu s if none\n",
                item.value,
                static_cast<unsigned long>(mesh_takeover_in_ms(now) / 1000));
        }
        break;
    case note_kind::layer_changed:
        Serial.printf("MESH layer %d\n", item.value);
        break;
    case note_kind::rootless:
        if (item.value)
        {
            Serial.println("MESH network has no root (it went down); a board "
                           "takes over if it doesn't come back");
        }
        else
        {
            Serial.println("MESH network has a root again");
        }
        break;
    case note_kind::root_found:
        Serial.printf("MESH root is Board %d\n", item.value);
        break;
    case note_kind::router_missing:
        Serial.printf("MESH root: no router to join (reason %d), as "
                      "intended; counted in `mesh`, not logged again\n",
            item.value);
        break;
    case note_kind::asked_to_yield:
        mesh_yield_asked = true;
        Serial.println("MESH another root asks this one to give way");
        break;
    case note_kind::stopped:
        Serial.println("MESH stopped");
        break;
    }
}

void report_mesh_radio(uint32_t now)
{
    mesh_radio::note item = {};
    while (mesh_radio::next_note(&item))
    {
        print_mesh_note(item);
    }
    const mesh_radio::state current = mesh_radio::current();
    if (current == mesh_shown_state)
    {
        return;
    }
    mesh_shown_state = current;
    if (current == mesh_radio::state::connected)
    {
        mesh_start_sessions(now);
    }
}

void print_mesh()
{
    const mesh_radio::counters c = mesh_radio::stats();
    const uint32_t now = millis();
    Serial.printf("MESH | %s", mesh_switched_off ? "off (mesh off)" :
        mesh_radio::state_name(mesh_radio::current()));
    if (mesh_radio::current() == mesh_radio::state::failed)
    {
        Serial.printf(" (%s: %s)", mesh_radio::failed_step(),
            esp_err_to_name(mesh_radio::last_error()));
    }
    Serial.printf(" | %s layer %d | root Board %u",
        mesh_radio::is_root() ? "root" : "node", mesh_radio::layer(),
        mesh_radio::root_board());
    // Who this board is joined through, and who is joined through it: a
    // middleman has a parent and children.
    if (!mesh_radio::is_root())
    {
        Serial.printf(" | parent Board %u", mesh_radio::parent_board());
    }
    Serial.printf(" | children %d", mesh_radio::children());
    const uint32_t children = mesh_radio::children_boards();
    const char *separator = " (";
    for (uint8_t id = 1; id <= max_id; ++id)
    {
        if (children & (1u << id))
        {
            Serial.printf("%sBoard %u", separator, id);
            separator = ", ";
        }
    }
    Serial.printf("%s | boards %d", children ? ")" : "",
        mesh_radio::nodes());
    if (mesh_looking)
    {
        Serial.printf(" | becomes root in %lu s if none found",
            static_cast<unsigned long>(mesh_takeover_in_ms(now) / 1000));
    }
    Serial.printf(" | link %s", call::state_name(mesh_calls.current()));
    if (mesh_calls.current() != call::state::idle)
    {
        Serial.printf(" with %u", mesh_calls.peer());
    }
    Serial.printf(" | sent %lu failed %lu | received %lu dropped %lu unknown "
                  "%lu | accepted %lu rejected %lu | parent joins %lu losses "
                  "%lu | router misses %lu | restarts %lu%s",
        static_cast<unsigned long>(c.sent),
        static_cast<unsigned long>(c.send_failed),
        static_cast<unsigned long>(c.received),
        static_cast<unsigned long>(c.rx_drops),
        static_cast<unsigned long>(c.unknown_drops),
        static_cast<unsigned long>(mesh_accepted),
        static_cast<unsigned long>(mesh_rejected),
        static_cast<unsigned long>(c.parent_joins),
        static_cast<unsigned long>(c.parent_losses),
        static_cast<unsigned long>(c.router_misses),
        static_cast<unsigned long>(c.restarts),
        mesh_muted ? " | MUTED" : "");
    Serial.print(" | sessions");
    print_sessions(via::mesh, now);
    Serial.println();
}

// Called from start(): loads the same pair keys as the ESP-NOW sessions.
void setup_mesh_keys()
{
    for (const e2e_keys::peer_key &entry : e2e_keys::peer_keys)
    {
        if (valid_peer(entry.peer))
        {
            mesh_peers[entry.peer].link.begin(own_id, entry.peer, entry.key);
        }
    }
}

// Called once the screen is up. A mesh that fails to start is reported
// and left off; ESP-NOW and LoRa carry on.
bool setup_mesh()
{
    mesh_calls.begin(own_id);  // ESP-NOW's timings.
    mesh_calls.seed(esp_random());
    // The soft-AP's WPA2 password: from the network key, so every board
    // has it and nothing else does. 32 hex characters.
    uint8_t secret[16];
    kdf::input label;
    label.add("SE19 mesh soft-AP password");
    char password[2 * sizeof(secret) + 1] = {};
    if (!kdf::mac(e2e_keys::network, sizeof(e2e_keys::network), label,
            secret, sizeof(secret)))
    {
        Serial.println("MESH failed: could not derive the soft-AP password");
        return false;
    }
    for (size_t i = 0; i < sizeof(secret); ++i)
    {
        snprintf(password + 2 * i, 3, "%02x", secret[i]);
    }
    kdf::wipe(secret, sizeof(secret));
    Serial.printf("MESH starting | channel %u | mesh ID %.6s | no router | "
                  "soft-AP WPA2, key from the network key | root order: "
                  "Board %u first, then by ID; this board's turn after "
                  "%lu s\n",
        network_config::channel,
        reinterpret_cast<const char *>(network_config::mesh_id),
        network_config::mesh_root_id,
        static_cast<unsigned long>(mesh_takeover_wait_ms() / 1000));
    const char *error = mesh_radio::start(password);
    kdf::wipe(password, sizeof(password));
    if (error)
    {
        // Retried by service_mesh_failover() if the mesh got set up.
        Serial.printf("MESH unavailable (%s: %s); ESP-NOW and LoRa still "
                      "work%s\n",
            error, esp_err_to_name(mesh_radio::last_error()),
            mesh_radio::can_restart() ? "; retrying" : "");
        return false;
    }
    if (mesh_radio::placeholder_router())
    {
        Serial.printf("MESH router SSID \"%s\" set because this IDF wants "
                      "one; it doesn't exist, so the mesh still uses no "
                      "router\n",
            app_config::mesh_placeholder_router);
    }
    // radio::start() turned modem sleep off for voice; say if the mesh
    // turned it back on (it is the mesh's to set now).
    wifi_ps_type_t sleep = WIFI_PS_NONE;
    if (esp_wifi_get_ps(&sleep) == ESP_OK && sleep != WIFI_PS_NONE)
    {
        Serial.printf("MESH warning: Wi-Fi power save is %d after the mesh "
                      "started; ESP-NOW voice may stutter\n",
            static_cast<int>(sleep));
    }
    return true;
}

void service_mesh(uint32_t now)
{
    // Failover first: it starts the takeover clock the notes report.
    service_mesh_failover(now);
    report_mesh_radio(now);
    mesh_radio::received_frame frame;
    while (mesh_radio::receive(&frame))
    {
        if (mesh_muted)
        {
            ++mesh_muted_rx;
            continue;
        }
        handle_mesh(frame, now);
    }
    if (mesh_ready())
    {
        service_mesh_sessions(now);
    }
    service_mesh_presence(now);
    service_calls(via::mesh, now);
}

// Test settings, from the Tools page (ui.inc) or serial: link loss, and a
// board going down, without unplugging anything.

// Drops every mesh frame sent and received, as out of range would (the
// mesh itself stays up).
void mesh_set_muted(bool on)
{
    mesh_muted = on;
    Serial.printf("MESH mute %s | dropped so far: sent %lu received %lu\n",
        mesh_muted ? "on (nothing sent or received)" : "off",
        static_cast<unsigned long>(mesh_muted_tx),
        static_cast<unsigned long>(mesh_muted_rx));
}

// Leaves the mesh completely, as if this board were down; another takes
// over if it was the root. mesh_rejoin() comes back.
void mesh_leave()
{
    mesh_switched_off = true;
    mesh_looking = mesh_alone = false;
    mesh_radio::stop();
    Serial.println("MESH off: this board left the mesh, as if it were down "
                   "(Tools: Rejoin mesh)");
}

// Looks for the mesh and joins it, as at boot.
void mesh_rejoin(uint32_t now)
{
    mesh_switched_off = false;
    mesh_restart(false, now);
}

// Makes this board a root now: a second root, if another is up, until the
// one nobody joins steps down.
void mesh_force_root(uint32_t now)
{
    mesh_switched_off = false;
    mesh_restart(true, now);
    Serial.println("MESH forced to be a root: if another root is up, the one "
                   "nobody joins steps down within about a minute");
}

// Serial commands. accept, decline and disconnect act on the mesh
// connection while one is in progress; hello, talk, test and stop follow
// it on their own (voice_via()).
bool run_mesh_command(const char *line, uint32_t now)
{
    if (strcmp(line, "mesh") == 0)
    {
        print_mesh();
        return true;
    }
    if (strcmp(line, "mesh mute on") == 0 || strcmp(line, "mesh mute off") == 0)
    {
        mesh_set_muted(strcmp(line + 10, "on") == 0);
        return true;
    }
    if (strcmp(line, "mesh off") == 0)
    {
        mesh_leave();
        return true;
    }
    if (strcmp(line, "mesh on") == 0)
    {
        mesh_rejoin(now);
        return true;
    }
    if (strcmp(line, "mesh root") == 0)
    {
        mesh_force_root(now);
        return true;
    }
    if (strncmp(line, "mesh connect", 12) == 0 &&
        (line[12] == '\0' || isspace(static_cast<unsigned char>(line[12]))))
    {
        uint8_t peer = 0;
        if (command_parser::board_id(line + 12, peer))
        {
            place_mesh_call(peer, now);
        }
        else
        {
            Serial.println("MESH LINK refused: invalid board ID");
        }
        return true;
    }
    return mesh_in_use() && run_link_command(mesh_calls, "MESH ", line, now);
}
