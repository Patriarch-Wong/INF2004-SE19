// Included inside main.cpp's anonymous namespace, after mesh.inc.
//
// Peer mesh: everything ESP-NOW has (connections, push-to-talk voice,
// RIGHT hello, presence), carried across boards that relay for each other
// over ESP-NOW. Every board is a node and a router: no root, no router,
// nobody in charge (peer_routing.h has the routing, peer_frame.h the
// frames).
//
//   Links     Every board broadcasts a link-state advert every
//             peer_advert_ms: the boards it hears and how well. Adverts
//             are flooded (each board passes each one on once), so every
//             board holds the same map. A link counts only if both ends
//             hear each other.
//   Routes    Worked out from the map on every board, the same both ways:
//             A's route to B is B's route to A turned round, so the other
//             side's voice and every reply retrace the same boards.
//   Frames    The sender puts the whole route in the frame; each board on
//             it hands the frame to the next as acknowledged ESP-NOW
//             unicast. A relay whose next board stopped acknowledging
//             drops it as a neighbour, sends a fresh advert at once, and
//             splices its own route into the frame, so frames already on
//             their way still arrive.
//
// It runs alongside ESP-NOW, ESP-MESH and LoRa, as a fourth way to
// connect, under a session key of its own per peer made by the same
// handshake over the peer mesh. Frames are sealed end to end as over
// ESP-NOW; a relay checks the network-key hop tag before passing a frame
// on, and can't open it. Adverts carry an HMAC under the network key.
//
// Tools `Block Board N` (serial `peer block N`) makes this board ignore
// peer mesh frames heard directly from board N, as if N were out of
// range, so a middleman can be tested on one desk (block each way).

static_assert(radio::min_frame_size <= peer_frame::min_advert_size,
    "the receive callback would drop short adverts");
static_assert(max_id < peer_routing::max_nodes,
    "peer_routing can't hold this many boards");
static_assert(app_config::peer_max_hops <= peer_frame::max_path,
    "a route can't carry that many hops");
static_assert(voice_link::max_voice_size + secure_message::overhead <=
        peer_frame::max_inner_size,
    "a voice frame (two Opus packets) must fit with a full route");

// A board over the peer mesh: the session and presence state ESP-NOW
// keeps (peer_state), and the route its last frame took.
struct peer_mesh_peer : peer_state
{
    uint8_t heard_route[peer_frame::max_path] = {};
    uint8_t heard_length = 0;
    uint32_t heard_ms = 0;
};

struct peer_mesh_counts
{
    uint32_t sent = 0;        // Frames this board started.
    uint32_t relayed = 0;     // Passed on for others.
    uint32_t rerouted = 0;    // Passed on along a spliced route.
    uint32_t delivered = 0;   // Opened here.
    uint32_t copies = 0;      // Duplicates dropped.
    uint32_t rejected = 0;
    uint32_t blocked = 0;     // Heard from a blocked board.
    uint32_t hop_drops = 0;   // Out of hops (TTL).
    uint32_t no_route = 0;    // Nowhere to send it.
    uint32_t send_errors = 0;
    uint32_t adverts_sent = 0;
    uint32_t adverts_relayed = 0;
    uint32_t adverts_taken = 0;
    uint32_t neighbours_lost = 0;
    uint32_t muted_tx = 0;
    uint32_t muted_rx = 0;
};

peer_mesh_peer peer_peers[max_id + 1];  // Indexed by device ID.
peer_routing::table peer_table;
peer_frame::seen_cache peer_seen;
peer_mesh_counts peer_counts;
uint32_t peer_control_sequence = 0;
uint16_t peer_next_id = 0;
uint16_t peer_advert_sequence = 0;
uint32_t peer_next_advert_ms = 0;
uint32_t peer_computed_ms = 0;
bool peer_dirty = true;            // The map changed: work out routes.
bool peer_advert_due = false;      // Send an advert now (a neighbour lost).
uint32_t peer_routes_logged = 0;   // peer_table.version() last logged.
bool peer_muted = false;
uint32_t peer_blocked = 0;  // Bit N: ignore frames heard from board N.
// Each neighbour's ESP-NOW acknowledgement state when it was last dropped
// for lost acks, so it isn't dropped again for the same failures.
uint32_t peer_lost_failures[max_id + 1] = {};
uint32_t peer_lost_ok_ms[max_id + 1] = {};
bool peer_had_route[max_id + 1] = {};  // To see a route appear.

uint8_t peer_board_of(const uint8_t *mac)
{
    const network_config::device *entry = radio::find_device(mac);
    return entry ? entry->id : 0;
}

const uint8_t *peer_mac(uint8_t board)
{
    const network_config::device *entry = radio::find_device(board);
    return entry ? entry->mac : nullptr;
}

bool peer_is_blocked(uint8_t board)
{
    return board > 0 && board < 32 && (peer_blocked & (1u << board)) != 0;
}

// A session is up, something from the board opened recently, and there is
// a route to it. Relayed frames get no end-to-end acknowledgement, so
// presence pings are the sign it is reachable.
bool peer_mesh_online(uint8_t peer, uint32_t now)
{
    const peer_mesh_peer &state = peer_peers[peer];
    return state.link.established() &&
        within_presence(state.last_accept_ms, now) &&
        peer_table.next_hop(peer) != 0;
}

void peer_reject(const radio::received_frame &frame, const char *reason)
{
    log_reject("PEER ", peer_counts.rejected, frame.mac, frame.size, reason);
}

// Every peer mesh frame goes out here: mute.
esp_err_t peer_transmit(const uint8_t *mac, const uint8_t *frame,
    size_t size)
{
    if (peer_muted)
    {
        ++peer_counts.muted_tx;
        return ESP_OK;  // Gone, as out of range: timeouts notice.
    }
    const esp_err_t result = radio::send(mac, frame, size);
    if (result != ESP_OK)
    {
        ++peer_counts.send_errors;
    }
    return result;
}

// Adverts

void peer_send_advert(uint32_t now)
{
    peer_table.tick(now);
    peer_frame::advert message = {};
    message.ttl = app_config::peer_max_hops;
    message.origin = own_id;
    message.sequence = ++peer_advert_sequence;
    message.count = peer_table.publish(now, message.links,
        peer_frame::max_links);
    uint8_t out[peer_frame::max_advert_size];
    size_t size = 0;
    if (!peer_frame::build_advert(e2e_keys::network, message, out,
            sizeof(out), &size))
    {
        ++peer_counts.send_errors;
        return;
    }
    if (peer_transmit(radio::broadcast_mac, out, size) == ESP_OK)
    {
        ++peer_counts.adverts_sent;
    }
    peer_dirty = true;  // This board's own links are part of the map.
    // A little jitter, so boards switched on together don't keep sending
    // their adverts at the same moment.
    peer_next_advert_ms = now + app_config::peer_advert_ms -
        app_config::peer_advert_jitter_ms / 2 +
        esp_random() % (app_config::peer_advert_jitter_ms + 1);
}

void peer_take_advert(const radio::received_frame &frame, uint8_t sender,
    uint32_t now)
{
    peer_frame::advert message = {};
    if (!peer_frame::parse_advert(e2e_keys::network, frame.bytes, frame.size,
            &message))
    {
        peer_reject(frame, "bad advert");
        return;
    }
    // Any copy proves the board that sent it is in range.
    peer_table.heard(sender, now);
    if (!peer_table.take(message, now))
    {
        return;  // A copy, an old one, or this board's own.
    }
    ++peer_counts.adverts_taken;
    peer_dirty = true;
    if (message.ttl <= 1)
    {
        return;
    }
    // Pass it on once with one hop fewer; the tag doesn't cover the TTL.
    uint8_t out[peer_frame::max_advert_size];
    memcpy(out, frame.bytes, frame.size);
    out[1] = static_cast<uint8_t>(message.ttl - 1);
    if (peer_transmit(radio::broadcast_mac, out, frame.size) == ESP_OK)
    {
        ++peer_counts.adverts_relayed;
    }
}

// Routes

void peer_log_routes(uint32_t now)
{
    Serial.print("PEER routes |");
    for (uint8_t target = 1; target <= max_id; ++target)
    {
        if (!valid_peer(target))
        {
            continue;
        }
        uint8_t path[peer_frame::max_path];
        const uint8_t length = peer_table.path(target, path, sizeof(path));
        char text[40];
        path_text(path, length, text, sizeof(text));
        Serial.printf(" %u: %s |", target, length ? text : "no route");
    }
    Serial.printf(" neighbours:");
    bool any = false;
    for (uint8_t id = 1; id <= max_id; ++id)
    {
        if (valid_peer(id) && peer_table.neighbour(id, now))
        {
            Serial.printf(" %u (%u%%)", id, peer_table.quality(id));
            any = true;
        }
    }
    Serial.println(any ? "" : " none");
}

// A route to `peer` has just appeared (it came into range, or this board
// just started): make a session if there is none, or hurry a handshake
// whose hellos went nowhere while there was no route, as ESP-MESH does on
// joining (mesh_start_sessions()).
void peer_route_appeared(uint8_t peer, uint32_t now)
{
    peer_mesh_peer &state = peer_peers[peer];
    if (!state.link.paired() || state.link.established())
    {
        return;
    }
    if (!state.link.initiating())
    {
        start_session(via::peer, peer, now);
    }
    else
    {
        state.hellos = 0;
        state.last_hello_ms = now - app_config::session_retry_ms;
    }
}

// Drops neighbours whose ESP-NOW unicasts stopped being acknowledged (the
// radio's acknowledgements, as for direct ESP-NOW links), at most once
// per run of failures.
void peer_check_acks(uint32_t now)
{
    for (uint8_t id = 1; id <= max_id; ++id)
    {
        if (!valid_peer(id) || !peer_table.neighbour(id, now))
        {
            continue;
        }
        const radio::ack_state acks = radio::acks(id);
        if (acks.last_ok_ms != peer_lost_ok_ms[id])
        {
            peer_lost_failures[id] = 0;  // Something got through since.
        }
        if (!acks_lost(id, now) || acks.failures <= peer_lost_failures[id])
        {
            continue;
        }
        peer_lost_failures[id] = acks.failures;
        peer_lost_ok_ms[id] = acks.last_ok_ms;
        peer_table.lost(id, now);
        ++peer_counts.neighbours_lost;
        peer_dirty = true;
        peer_advert_due = true;
        Serial.printf("PEER neighbour %u (%s) stopped acknowledging: routed "
                      "around\n",
            id, device_name(id));
    }
}

void service_peer_routes(uint32_t now)
{
    peer_check_acks(now);
    if (peer_advert_due ||
        static_cast<int32_t>(now - peer_next_advert_ms) >= 0)
    {
        peer_advert_due = false;
        peer_send_advert(now);
    }
    // Again when the map changed, and every so often as adverts age out.
    if (peer_dirty || now - peer_computed_ms >= app_config::peer_compute_ms)
    {
        peer_dirty = false;
        peer_computed_ms = now;
        peer_table.compute(now);
        if (peer_table.version() != peer_routes_logged)
        {
            peer_routes_logged = peer_table.version();
            peer_log_routes(now);
        }
        for (uint8_t peer = 1; peer <= max_id; ++peer)
        {
            const bool route = valid_peer(peer) &&
                peer_table.next_hop(peer) != 0;
            if (route && !peer_had_route[peer])
            {
                peer_route_appeared(peer, now);
            }
            peer_had_route[peer] = route;
        }
    }
}

// Sending

esp_err_t peer_forward_to(const peer_frame::envelope &head,
    const uint8_t *inner, size_t inner_size)
{
    uint8_t out[secure_message::max_frame_size];
    size_t size = 0;
    const uint8_t *mac = peer_mac(head.route[head.at]);
    if (!mac || !peer_frame::build(head, inner, inner_size, out, sizeof(out),
                    &size))
    {
        ++peer_counts.send_errors;
        return ESP_ERR_INVALID_SIZE;
    }
    return peer_transmit(mac, out, size);
}

// Sends a secure_message frame from this board along its route to
// `destination`; with no route it is dropped (resends and the next frames
// go once there is one).
esp_err_t peer_send_inner(uint8_t destination, const uint8_t *inner,
    size_t inner_size, uint32_t now)
{
    peer_frame::envelope head = {};
    head.route_length = peer_table.path(destination, head.route,
        sizeof(head.route));
    if (head.route_length < 2)
    {
        ++peer_counts.no_route;
        return ESP_ERR_NOT_FOUND;
    }
    head.ttl = app_config::peer_max_hops;
    head.id = ++peer_next_id;
    head.at = 1;
    // Its echo from a confused relay is a copy.
    peer_seen.first(own_id, head.id, now, app_config::peer_seen_window_ms);
    const esp_err_t result = peer_forward_to(head, inner, inner_size);
    if (result == ESP_OK)
    {
        ++peer_counts.sent;
    }
    return result;
}

esp_err_t peer_mesh_send_payload(uint8_t peer, const uint8_t *payload,
    size_t size, uint8_t type)
{
    uint8_t inner[peer_frame::max_inner_size] = {};
    size_t inner_size = 0;
    if (!seal_payload(peer_peers[peer].link, "PEER ", type, payload, size,
            inner, sizeof(inner), &inner_size))
    {
        return ESP_FAIL;
    }
    return peer_send_inner(peer, inner, inner_size, millis());
}

void peer_send_control(uint8_t destination,
    const control_frame::message &content, uint32_t now)
{
    uint8_t inner[peer_frame::max_inner_size];
    size_t size = 0;
    const status built = build_control(peer_control_sequence,
        control_frame::type_session, destination, content, inner,
        sizeof(inner), &size);
    if (built != status::ok)
    {
        Serial.printf("PEER control frame not built: %s\n",
            secure_message::status_name(built));
        return;
    }
    peer_send_inner(destination, inner, size, now);
}

// Receive

// A frame addressed to this board, as handle_mesh() takes the mesh's.
void peer_deliver(const radio::received_frame &frame,
    const peer_frame::envelope &head, secure_message::header fields,
    uint32_t now)
{
    peer_mesh_peer &state = peer_peers[fields.source];
    if (control_frame::is_control(fields.type))
    {
        control_frame::message content = {};
        const status parsed = control_frame::parse(e2e_keys::network,
            head.inner, head.inner_size, &fields, &content);
        if (parsed != status::ok)
        {
            peer_reject(frame, secure_message::status_name(parsed));
        }
        else if (fields.type != control_frame::type_session)
        {
            peer_reject(frame, "pairing not used: keys are compiled in");
        }
        else
        {
            const char *problem = take_session(via::peer, fields.source,
                content, now);
            if (problem)
            {
                peer_reject(frame, problem);
            }
        }
        return;
    }
    const int64_t arrival = esp_timer_get_time();
    uint8_t payload[secure_message::max_payload_size];
    size_t size = 0;
    secure_message::header opened = fields;
    const auto result = state.link.open(e2e_keys::network, head.inner,
        head.inner_size, &opened, payload, sizeof(payload), &size);
    if (result != session::open_result::accepted &&
        result != session::open_result::accepted_new_session)
    {
        peer_reject(frame, session::open_result_name(result));
        maybe_resync(via::peer, fields.source, now);
        return;
    }
    ++peer_counts.delivered;
    state.last_accept_ms = now;
    memcpy(state.heard_route, head.route, head.route_length);
    state.heard_length = head.route_length;
    state.heard_ms = now;
    const char *problem = take_payload(via::peer, fields.source, opened.type,
        payload, size, arrival, now);
    if (problem)
    {
        peer_reject(frame, problem);
    }
}

// Passes a frame for another board on, to the next board on its route; if
// that board is no longer a neighbour, along this board's own route
// instead (spliced into the frame).
void peer_forward(peer_frame::envelope head, uint32_t now)
{
    if (head.ttl <= 1)
    {
        ++peer_counts.hop_drops;
        return;
    }
    --head.ttl;
    bool spliced = false;
    if (!peer_table.neighbour(head.route[head.at + 1], now))
    {
        uint8_t own[peer_frame::max_path];
        const uint8_t length = peer_table.path(head.destination(), own,
            sizeof(own));
        if (length < 2 || !peer_frame::splice(&head, own, length))
        {
            ++peer_counts.no_route;
            return;
        }
        spliced = true;
    }
    ++head.at;
    if (peer_forward_to(head, head.inner, head.inner_size) == ESP_OK)
    {
        ++(spliced ? peer_counts.rerouted : peer_counts.relayed);
    }
}

void peer_take_data(const radio::received_frame &frame, uint8_t sender,
    uint32_t now)
{
    peer_frame::envelope head = {};
    if (!peer_frame::parse(frame.bytes, frame.size, &head))
    {
        peer_reject(frame, "bad envelope");
        return;
    }
    // This copy must be meant for this board, from the board before it.
    if (head.route[head.at] != own_id || head.route[head.at - 1] != sender)
    {
        peer_reject(frame, "not this board's hop");
        return;
    }
    peer_table.heard(sender, now);
    secure_message::header fields = {};
    const status verified = secure_message::verify_hop(e2e_keys::network,
        head.inner, head.inner_size, &fields);
    if (verified != status::ok)
    {
        peer_reject(frame, secure_message::status_name(verified));
        return;
    }
    if (fields.source != head.origin() ||
        fields.destination != head.destination())
    {
        peer_reject(frame, "route doesn't match the frame");
        return;
    }
    if (fields.source == own_id)
    {
        ++peer_counts.copies;  // This board's own frame, back again.
        return;
    }
    if (!valid_peer(fields.source) ||
        (fields.destination != own_id && !valid_peer(fields.destination)))
    {
        peer_reject(frame, "unknown source or destination ID");
        return;
    }
    if (!peer_seen.first(fields.source, head.id, now,
            app_config::peer_seen_window_ms))
    {
        ++peer_counts.copies;
        return;
    }
    if (fields.destination == own_id)
    {
        peer_deliver(frame, head, fields, now);
    }
    else
    {
        peer_forward(head, now);
    }
}

// A peer mesh frame (true; taken here), else false: handle() takes it as
// a direct ESP-NOW frame.
bool peer_handle(const radio::received_frame &frame, uint32_t now)
{
    const uint8_t marker = frame.size > 0 ? frame.bytes[0] : 0;
    if (marker != peer_frame::marker_data &&
        marker != peer_frame::marker_advert)
    {
        return false;
    }
    const uint8_t sender = peer_board_of(frame.mac);
    if (sender == 0)
    {
        return true;  // The radio allowlist keeps these out already.
    }
    if (peer_is_blocked(sender))
    {
        ++peer_counts.blocked;
        return true;
    }
    if (peer_muted)
    {
        ++peer_counts.muted_rx;
        return true;
    }
    if (marker == peer_frame::marker_advert)
    {
        peer_take_advert(frame, sender, now);
    }
    else
    {
        peer_take_data(frame, sender, now);
    }
    return true;
}

// Routes on screen

void peer_route_text(uint8_t peer, char *text, size_t capacity)
{
    uint8_t path[peer_frame::max_path];
    const uint8_t length = peer_table.path(peer, path, sizeof(path));
    path_text(path, length, text, capacity);
}

// Every Wi-Fi way (declared in handset.inc; here, after the last way)

peer_state &peer_via(via link, uint8_t peer)
{
    switch (link)
    {
    case via::mesh: return mesh_peers[peer];
    case via::peer: return peer_peers[peer];
    case via::espnow:
    case via::broadcast: break;
    }
    return peers[peer];
}

void send_control_via(via link, uint8_t destination,
    const control_frame::message &content, uint32_t now)
{
    switch (link)
    {
    case via::mesh:
        mesh_send_control(destination, content);
        return;
    case via::peer:
        peer_send_control(destination, content, now);
        return;
    case via::espnow:
    case via::broadcast: break;
    }
    send_control(control_frame::type_session, destination,
        radio::find_device(destination)->mac, content);
}

// ESP-NOW is always direct, so only the two meshes have a route.
void route_text_via(via link, uint8_t peer, uint32_t now, char *text,
    size_t capacity)
{
    if (link == via::peer)
    {
        peer_route_text(peer, text, capacity);
    }
    else
    {
        mesh_route_text(peer, now, text, capacity);
    }
}

// Why `peer` looks gone over `link` (it didn't reboot): for the log line
// that ends the connection.
const char *gone_reason(via link, uint8_t peer, uint32_t now)
{
    switch (link)
    {
    case via::mesh:
        return !mesh_ready() ? "can't be reached: this board left the mesh" :
            mesh_sends_failing(peer, now) ? "can't be reached (sends failing)" :
            "went offline";
    case via::peer:
        return peer_muted ? "can't be reached: peer mesh muted" :
            peer_table.next_hop(peer) == 0 ? "can't be reached (no route)" :
            "went offline";
    case via::espnow:
    case via::broadcast: break;
    }
    return acks_lost(peer, now) ? "stopped acknowledging (radio)" :
        "went offline";
}

// Connections

bool place_peer_call(uint8_t peer, uint32_t now)
{
    if (!valid_peer(peer))
    {
        Serial.printf("PEER LINK refused: no handset %u\n", peer);
        return false;
    }
    if (busy_elsewhere(peer_calls))
    {
        Serial.println("PEER LINK refused: a connection over another radio "
                       "is in progress");
        return false;
    }
    if (!peer_mesh_online(peer, now))
    {
        Serial.printf("PEER LINK refused: %u is offline (%s)\n", peer,
            !peer_peers[peer].link.established() ? "no session" :
            peer_table.next_hop(peer) == 0 ? "no route" :
            "no presence ping for a while");
        return false;
    }
    peer_calls.set_unavailable(false);
    return dial(peer_calls, "PEER ", peer, random_id(), now);
}

// Status and set-up

void print_peer()
{
    const uint32_t now = millis();
    Serial.printf("PEER | link %s", call::state_name(peer_calls.current()));
    if (peer_calls.current() != call::state::idle)
    {
        Serial.printf(" with %u", peer_calls.peer());
    }
    Serial.printf(" | sent %lu relayed %lu rerouted %lu delivered %lu "
                  "copies %lu rejected %lu blocked %lu out of hops %lu "
                  "no route %lu send errors %lu | adverts sent %lu relayed "
                  "%lu taken %lu | neighbours lost %lu",
        static_cast<unsigned long>(peer_counts.sent),
        static_cast<unsigned long>(peer_counts.relayed),
        static_cast<unsigned long>(peer_counts.rerouted),
        static_cast<unsigned long>(peer_counts.delivered),
        static_cast<unsigned long>(peer_counts.copies),
        static_cast<unsigned long>(peer_counts.rejected),
        static_cast<unsigned long>(peer_counts.blocked),
        static_cast<unsigned long>(peer_counts.hop_drops),
        static_cast<unsigned long>(peer_counts.no_route),
        static_cast<unsigned long>(peer_counts.send_errors),
        static_cast<unsigned long>(peer_counts.adverts_sent),
        static_cast<unsigned long>(peer_counts.adverts_relayed),
        static_cast<unsigned long>(peer_counts.adverts_taken),
        static_cast<unsigned long>(peer_counts.neighbours_lost));
    Serial.printf("%s | sessions", peer_muted ? " | MUTED" : "");
    print_sessions(via::peer, now);
    Serial.println();
}

// Called from start(): the same pair keys as the ESP-NOW sessions.
void setup_peer_keys()
{
    for (const e2e_keys::peer_key &entry : e2e_keys::peer_keys)
    {
        if (valid_peer(entry.peer))
        {
            peer_peers[entry.peer].link.begin(own_id, entry.peer, entry.key);
        }
    }
}

void setup_peer()
{
    peer_routing::settings settings;
    settings.neighbour_timeout_ms = app_config::peer_neighbour_timeout_ms;
    settings.advert_lifetime_ms = app_config::peer_advert_lifetime_ms;
    settings.holddown_ms = app_config::peer_holddown_ms;
    settings.min_quality = app_config::peer_min_quality;
    settings.start_quality = app_config::peer_start_quality;
    peer_table.begin(own_id, max_id, settings);
    peer_calls.begin(own_id);  // ESP-NOW's timings.
    peer_calls.seed(esp_random());
    peer_next_id = static_cast<uint16_t>(esp_random());
    peer_advert_sequence = static_cast<uint16_t>(esp_random());
    const uint32_t now = millis();
    peer_next_advert_ms = now;  // The first advert goes at once.
    Serial.printf("PEER mesh: every board a node and router over ESP-NOW | "
                  "link-state adverts every %lu ms | routes the same both "
                  "ways | up to %u hops | no router, no root\n",
        static_cast<unsigned long>(app_config::peer_advert_ms),
        app_config::peer_max_hops - 1);
    for (uint8_t peer = 1; peer <= max_id; ++peer)
    {
        if (peer_peers[peer].link.paired())
        {
            start_session(via::peer, peer, now);
        }
    }
}

const uint8_t peer_presence_ping[] = {kind_presence};

void service_peer(uint32_t now)
{
    service_peer_routes(now);
    service_sessions(via::peer, now);
    service_presence(via::peer, now, true, false, peer_presence_ping,
        sizeof(peer_presence_ping));
    service_calls(via::peer, now);
}

// Test settings, from the Tools page (ui.inc) or serial.

// Ignores (or takes again) peer mesh frames heard directly from `board`,
// as if it were out of range: set on both boards, frames between them must
// go through a third.
void peer_set_block(uint8_t board, bool on, uint32_t now)
{
    if (board < 1 || board >= 32 || board == own_id)
    {
        return;
    }
    if (on)
    {
        peer_blocked |= 1u << board;
        peer_table.lost(board, now);
        peer_dirty = true;
        peer_advert_due = true;
    }
    else
    {
        peer_blocked &= ~(1u << board);
    }
    Serial.printf("PEER %s Board %u: peer mesh frames heard directly from "
                  "it are %s\n",
        on ? "blocking" : "unblocking", board,
        on ? "ignored (as if out of range)" : "taken again");
}

// Drops everything sent and received over the peer mesh, for link-loss
// tests.
void peer_set_muted(bool on)
{
    peer_muted = on;
    Serial.printf("PEER mute %s | dropped so far: sent %lu received %lu\n",
        peer_muted ? "on (nothing sent or received)" : "off",
        static_cast<unsigned long>(peer_counts.muted_tx),
        static_cast<unsigned long>(peer_counts.muted_rx));
}

// Serial commands. accept, decline and disconnect act on the peer mesh
// connection while one is in progress; hello, talk, test and stop follow
// it on their own (voice_via()).
bool run_peer_command(const char *line, uint32_t now)
{
    if (strcmp(line, "peer") == 0)
    {
        print_peer();
        return true;
    }
    if (strcmp(line, "peer routes") == 0)
    {
        peer_log_routes(now);
        return true;
    }
    if (strcmp(line, "peer mute on") == 0 || strcmp(line, "peer mute off") == 0)
    {
        peer_set_muted(strcmp(line, "peer mute on") == 0);
        return true;
    }
    if (strncmp(line, "peer connect", 12) == 0 &&
        (line[12] == '\0' || isspace(static_cast<unsigned char>(line[12]))))
    {
        uint8_t peer = 0;
        if (command_parser::board_id(line + 12, peer))
        {
            place_peer_call(peer, now);
        }
        else
        {
            Serial.println("PEER LINK refused: invalid board ID");
        }
        return true;
    }
    const bool block = strncmp(line, "peer block ", 11) == 0;
    const bool unblock = strncmp(line, "peer unblock ", 13) == 0;
    if (block || unblock)
    {
        uint8_t board = 0;
        if (!command_parser::board_id(line + (block ? 10 : 12), board) ||
            board == own_id)
        {
            Serial.println("PEER usage: peer block N | peer unblock N "
                           "(another board's ID)");
            return true;
        }
        peer_set_block(board, block, now);
        return true;
    }
    if (peer_calls.current() != call::state::idle &&
        run_link_command(peer_calls, "PEER ", line, now))
    {
        return true;
    }
    return false;
}
