// Included inside main.cpp's anonymous namespace, after handset.inc.
//
// ESP-NOW flood mesh: everything ESP-NOW has (connections, push-to-talk
// voice, RIGHT hello, presence), carried across boards that relay
// (relay_frame.h has the frames). Every board re-broadcasts every frame it
// hasn't seen before, up to relay_max_hops transmissions. All paths at
// once: nothing to set up and nothing to repair, but each frame is sent
// once by every board that hears it.
//
// A true mesh topology: every board relays, and there is no root and no
// router (ESP-NOW needs neither). It has its own session key per peer, made
// over itself, its own connections (call.h), presence pings and route
// line. Frames are sealed end to end as over ESP-NOW; a relay checks the
// network-key hop tag before passing a frame on, and can't open it.
//
// Serial `relay block N` makes this board ignore relayed frames heard
// directly from board N, as if N were out of range, so a middleman can be
// tested on one desk (block each way).

// A board over the flood mesh: the session and presence state ESP-NOW keeps
// (peer_state), and the path its frames take.
struct relay_peer : peer_state
{
    // The path the last frame from it took, origin first (route line).
    uint8_t heard_path[relay_frame::max_path] = {};
    uint8_t heard_length = 0;
    uint32_t heard_ms = 0;
};

struct relay_net
{
    relay_net(const char *tag_in, via link_in, call::machine &calls_in) :
        tag(tag_in), link(link_in), calls(calls_in)
    {
    }
    const char *tag;  // Log prefix: FLOOD.
    via link;
    call::machine &calls;
    relay_peer peers[max_id + 1];  // Indexed by device ID.
    relay_frame::seen_cache seen;
    uint32_t control_sequence = 0;
    uint16_t next_id = 0;
    bool muted = false;
    // Frames this board started, relayed, dropped as copies, took in.
    uint32_t sent = 0;
    uint32_t relayed = 0;
    uint32_t copies = 0;
    uint32_t delivered = 0;
    uint32_t rejected = 0;
    uint32_t blocked = 0;
    uint32_t hop_drops = 0;   // Out of hops (TTL).
    uint32_t busy_drops = 0;  // Not passed on: this board was connected.
    uint32_t send_errors = 0;
};

relay_net flood_net("FLOOD", via::flood, flood_calls);
uint32_t relay_blocked = 0;  // Bit N: ignore relayed frames from board N.
uint32_t relay_muted_tx = 0;
uint32_t relay_muted_rx = 0;

uint8_t relay_board_of(const uint8_t *mac)
{
    const network_config::device *entry = radio::find_device(mac);
    return entry ? entry->id : 0;
}

bool relay_is_blocked(uint8_t board)
{
    return board > 0 && board < 32 && (relay_blocked & (1u << board)) != 0;
}

// A session is up and something from the board opened recently. Broadcast
// and relayed frames get no end-to-end acknowledgement, so presence pings
// are the only sign the board is reachable.
bool relay_peer_online(uint8_t peer, uint32_t now)
{
    const relay_peer &state = flood_net.peers[peer];
    return state.link.established() &&
        within_presence(state.last_accept_ms, now);
}

void relay_reject(relay_net &net, const radio::received_frame &frame,
    const char *reason)
{
    log_reject(way(net.link).tag, net.rejected, frame.mac, frame.size,
        reason);
}

// Sending

esp_err_t relay_transmit(relay_net &net, const uint8_t *mac,
    const relay_frame::envelope &head, const uint8_t *inner,
    size_t inner_size, bool relaying)
{
    uint8_t out[secure_message::max_frame_size];
    size_t size = 0;
    if (!relay_frame::build(head, inner, inner_size, out, sizeof(out), &size))
    {
        ++net.send_errors;
        return ESP_ERR_INVALID_SIZE;
    }
    const esp_err_t result = radio::send(mac, out, size);
    if (result != ESP_OK)
    {
        ++net.send_errors;
    }
    else
    {
        ++(relaying ? net.relayed : net.sent);
    }
    return result;
}

// Broadcasts a secure_message frame from this board: every board that
// hears it passes it on, and the one it is addressed to takes it.
esp_err_t relay_send_inner(relay_net &net, const uint8_t *inner,
    size_t inner_size)
{
    if (net.muted)
    {
        ++relay_muted_tx;
        return ESP_OK;  // Gone, as out of range: timeouts notice.
    }
    relay_frame::envelope head = {};
    head.marker = relay_frame::marker_flood;
    head.ttl = app_config::relay_max_hops;
    head.id = ++net.next_id;
    head.path_length = 1;
    head.path[0] = own_id;
    return relay_transmit(net, radio::broadcast_mac, head, inner, inner_size,
        false);
}

esp_err_t relay_send_payload(uint8_t peer, const uint8_t *payload,
    size_t size, uint8_t type)
{
    relay_net &net = flood_net;
    uint8_t inner[relay_frame::max_inner_size] = {};
    size_t inner_size = 0;
    if (!seal_payload(net.peers[peer].link, way(net.link).tag, type, payload,
            size, inner, sizeof(inner), &inner_size))
    {
        return ESP_FAIL;
    }
    return relay_send_inner(net, inner, inner_size);
}

void relay_send_control(relay_net &net, uint8_t destination,
    const control_frame::message &content)
{
    uint8_t inner[relay_frame::max_inner_size];
    size_t size = 0;
    const status built = build_control(net.control_sequence,
        control_frame::type_session, destination, content, inner,
        sizeof(inner), &size);
    if (built != status::ok)
    {
        Serial.printf("%s control frame not built: %s\n", net.tag,
            secure_message::status_name(built));
        return;
    }
    relay_send_inner(net, inner, size);
}

// Receive

// A relayed frame addressed to this board.
void relay_deliver(relay_net &net, const radio::received_frame &frame,
    const relay_frame::envelope &head, secure_message::header fields,
    uint32_t now)
{
    relay_peer &state = net.peers[fields.source];
    if (control_frame::is_control(fields.type))
    {
        control_frame::message content = {};
        const status parsed = control_frame::parse(e2e_keys::network,
            head.inner, head.inner_size, &fields, &content);
        if (parsed != status::ok)
        {
            relay_reject(net, frame, secure_message::status_name(parsed));
        }
        else if (fields.type != control_frame::type_session)
        {
            relay_reject(net, frame, "pairing not used: keys are compiled in");
        }
        else
        {
            const char *problem = take_session(net.link, fields.source,
                content, now);
            if (problem)
            {
                relay_reject(net, frame, problem);
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
        relay_reject(net, frame, session::open_result_name(result));
        maybe_resync(net.link, fields.source, now);
        return;
    }
    ++net.delivered;
    state.last_accept_ms = now;
    memcpy(state.heard_path, head.path, head.path_length);
    state.heard_length = head.path_length;
    state.heard_ms = now;
    const char *problem = take_payload(net.link, fields.source, opened.type,
        payload, size, arrival, now);
    if (problem)
    {
        relay_reject(net, frame, problem);
    }
}

// Flood mesh frames waiting their random time before this board passes
// them on (app_config::relay_jitter_ms).
struct relay_pending
{
    bool used = false;
    uint32_t due_ms = 0;
    size_t size = 0;
    uint8_t bytes[secure_message::max_frame_size];
};
relay_pending relay_queue[app_config::relay_queue_depth];

// Keeps a flood frame to pass on for a random time; false if it must go
// now (no waiting configured, or the queue is full).
bool relay_hold(const relay_frame::envelope &head, uint32_t now)
{
    if (app_config::relay_jitter_ms == 0)
    {
        return false;
    }
    for (relay_pending &slot : relay_queue)
    {
        if (slot.used)
        {
            continue;
        }
        if (!relay_frame::build(head, head.inner, head.inner_size, slot.bytes,
                sizeof(slot.bytes), &slot.size))
        {
            return false;  // relay_transmit() counts the error.
        }
        slot.used = true;
        slot.due_ms = now + esp_random() % (app_config::relay_jitter_ms + 1);
        return true;
    }
    return false;
}

// Passes on the held frames whose time has come.
void service_relay_queue(uint32_t now)
{
    for (relay_pending &slot : relay_queue)
    {
        if (!slot.used || static_cast<int32_t>(now - slot.due_ms) < 0)
        {
            continue;
        }
        slot.used = false;
        if (flood_net.muted)
        {
            ++relay_muted_tx;  // Muted while it waited.
        }
        else if (any_connected())
        {
            ++flood_net.busy_drops;  // This board connected while it waited.
        }
        else if (radio::send(radio::broadcast_mac, slot.bytes, slot.size) ==
            ESP_OK)
        {
            ++flood_net.relayed;
        }
        else
        {
            ++flood_net.send_errors;
        }
    }
}

// Passes a frame for another board on: broadcasts it again after a random
// wait. A connected board (over any radio) passes nothing on: its radio
// time is for that connection. One only asking or asked still relays
// (any_connected()). Frames addressed to it are always taken
// (relay_deliver()).
void relay_forward(relay_net &net, relay_frame::envelope head, uint32_t now)
{
    if (any_connected())
    {
        ++net.busy_drops;
        return;
    }
    if (!relay_frame::add_hop(&head, own_id))
    {
        ++net.hop_drops;
        return;
    }
    if (relay_hold(head, now))
    {
        return;  // service_relay_queue() sends it.
    }
    relay_transmit(net, radio::broadcast_mac, head, head.inner,
        head.inner_size, true);
}

// A flood frame (true; taken here), else false: handle() takes it as a
// direct ESP-NOW frame.
bool relay_handle(const radio::received_frame &frame, uint32_t now)
{
    if (frame.size == 0 || frame.bytes[0] != relay_frame::marker_flood)
    {
        return false;
    }
    relay_net &net = flood_net;
    const uint8_t sender = relay_board_of(frame.mac);
    if (relay_is_blocked(sender))
    {
        ++net.blocked;
        return true;
    }
    if (net.muted)
    {
        ++relay_muted_rx;
        return true;
    }
    relay_frame::envelope head = {};
    if (!relay_frame::parse(frame.bytes, frame.size, &head))
    {
        relay_reject(net, frame, "bad envelope");
        return true;
    }
    if (!relay_frame::hops_ok(head, app_config::relay_max_hops))
    {
        // The TTL isn't signed: someone raised it to be relayed further.
        relay_reject(net, frame, "hop limit raised");
        return true;
    }
    secure_message::header fields = {};
    const status verified = secure_message::verify_hop(e2e_keys::network,
        head.inner, head.inner_size, &fields);
    if (verified != status::ok)
    {
        relay_reject(net, frame, secure_message::status_name(verified));
        return true;
    }
    const relay_frame::arrival what = relay_frame::classify(head,
        fields.source, fields.destination, sender, own_id);
    if (what == relay_frame::arrival::own)
    {
        ++net.copies;  // This board's own frame, back from a relay.
        return true;
    }
    if (!valid_peer(fields.source) ||
        (fields.destination != own_id && !valid_peer(fields.destination)))
    {
        relay_reject(net, frame, "unknown source or destination ID");
        return true;
    }
    // The path must start at the sender of the frame inside and end at the
    // board this copy came from; this board on it means a loop.
    if (what == relay_frame::arrival::bad_path)
    {
        relay_reject(net, frame, "path doesn't match the frame");
        return true;
    }
    if (what == relay_frame::arrival::looped)
    {
        ++net.copies;
        return true;
    }
    // Told apart by the hop tag just checked, not by the envelope's ID,
    // which anyone could set (relay_frame.h).
    if (!net.seen.first(fields.source,
            relay_frame::frame_key(head.inner, head.inner_size), now,
            app_config::relay_seen_window_ms))
    {
        ++net.copies;
        return true;
    }
    if (what == relay_frame::arrival::deliver)
    {
        relay_deliver(net, frame, head, fields, now);
    }
    else
    {
        relay_forward(net, head, now);
    }
    return true;
}

// Routes on screen

// A path of board IDs, this board first: "1 > 3 > 2, via Board 3",
// "1 > 2, direct", or "finding..." while not known (shorter than 2).
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

// The path a connection to `peer` takes, this board first: the path the
// last frame from the peer took, turned round (every path is used; this is
// the one whose copy arrived first).
void relay_route_text(uint8_t peer, uint32_t now, char *text,
    size_t capacity)
{
    uint8_t path[relay_frame::max_path + 1];
    size_t length = 0;
    const relay_peer &state = flood_net.peers[peer];
    if (state.heard_length > 0 &&
        now - state.heard_ms < app_config::presence_timeout_ms)
    {
        path[length++] = own_id;
        for (size_t i = state.heard_length; i-- > 0;)
        {
            path[length++] = state.heard_path[i];
        }
    }
    path_text(path, length, text, capacity);
}

// Every Wi-Fi way (declared in handset.inc; here, after the last way)

peer_state &peer_via(via link, uint8_t peer)
{
    return link == via::flood ? flood_net.peers[peer] : peers[peer];
}

void send_control_via(via link, uint8_t destination,
    const control_frame::message &content)
{
    if (link == via::flood)
    {
        relay_send_control(flood_net, destination, content);
        return;
    }
    send_control(control_frame::type_session, destination,
        radio::find_device(destination)->mac, content);
}

// Why `peer` looks gone over `link` (it didn't reboot): for the log line
// that ends the connection.
const char *gone_reason(via link, uint8_t peer, uint32_t now)
{
    return link == via::espnow && acks_lost(peer, now) ?
        "stopped acknowledging (radio)" : "went offline";
}

// Connections

bool place_relay_call(relay_net &net, uint8_t peer, uint32_t now)
{
    if (!valid_peer(peer))
    {
        Serial.printf("%s LINK refused: no handset %u\n", net.tag, peer);
        return false;
    }
    if (busy_elsewhere(net.calls))
    {
        Serial.printf("%s LINK refused: a connection over another radio is "
                      "in progress\n",
            net.tag);
        return false;
    }
    if (!relay_peer_online(peer, now))
    {
        Serial.printf("%s LINK refused: %u is offline (%s)\n", net.tag, peer,
            net.peers[peer].link.established() ?
                "no presence ping for a while" : "no session");
        return false;
    }
    const uint32_t id = random_id();
    net.calls.set_unavailable(false);
    return dial(net.calls, way(net.link).tag, peer, id, now);
}

// Status and set-up

void print_relay(relay_net &net)
{
    const uint32_t now = millis();
    Serial.printf("%s | link %s", net.tag,
        call::state_name(net.calls.current()));
    if (net.calls.current() != call::state::idle)
    {
        Serial.printf(" with %u", net.calls.peer());
    }
    Serial.printf(" | sent %lu relayed %lu copies %lu delivered %lu "
                  "rejected %lu blocked %lu out of hops %lu busy drops %lu "
                  "send errors %lu",
        static_cast<unsigned long>(net.sent),
        static_cast<unsigned long>(net.relayed),
        static_cast<unsigned long>(net.copies),
        static_cast<unsigned long>(net.delivered),
        static_cast<unsigned long>(net.rejected),
        static_cast<unsigned long>(net.blocked),
        static_cast<unsigned long>(net.hop_drops),
        static_cast<unsigned long>(net.busy_drops),
        static_cast<unsigned long>(net.send_errors));
    Serial.printf("%s | sessions", net.muted ? " | MUTED" : "");
    print_flood_sessions(now);
    Serial.println();
}

// The boards whose relayed frames this board ignores (`relay block`).
void print_blocked()
{
    Serial.print("RELAY blocked:");
    bool any = false;
    for (uint8_t id = 1; id <= max_id; ++id)
    {
        if (relay_is_blocked(id))
        {
            Serial.printf(" %u", id);
            any = true;
        }
    }
    Serial.println(any ? "" : " none");
}

// Called from start(): the same pair keys as the ESP-NOW sessions.
void setup_relay_keys()
{
    for (const e2e_keys::peer_key &entry : e2e_keys::peer_keys)
    {
        if (valid_peer(entry.peer))
        {
            flood_net.peers[entry.peer].link.begin(own_id, entry.peer,
                entry.key, session::transport::flood);
        }
    }
}

void setup_relay()
{
    flood_net.calls.begin(own_id);  // ESP-NOW's timings.
    flood_net.calls.seed(esp_random());
    flood_net.next_id = static_cast<uint16_t>(esp_random());
    Serial.printf("RELAY ESP-NOW multi-hop: flood mesh | up to %u hops | "
                  "relays wait up to %lu ms | voice pairs %s | no router, no "
                  "root\n",
        app_config::relay_max_hops,
        static_cast<unsigned long>(app_config::relay_jitter_ms),
        flood_pair ? "on" : "off");
    const uint32_t now = millis();
    for (uint8_t peer = 1; peer <= max_id; ++peer)
    {
        if (flood_net.peers[peer].link.paired())
        {
            start_session(via::flood, peer, now);
        }
    }
}

void service_relay(uint32_t now)
{
    service_relay_queue(now);
    service_sessions(via::flood, now, false);
    service_presence(via::flood, now);
    service_calls(via::flood, now);
}

// Test settings, from the Tools page (ui.inc) or serial.

// Ignores (or takes again) flood frames heard directly from `board`, as if it were out of range: set on both boards, frames between
// them must go through a third.
void relay_set_block(uint8_t board, bool on)
{
    if (board < 1 || board >= 32 || board == own_id)
    {
        return;
    }
    if (on)
    {
        relay_blocked |= 1u << board;
    }
    else
    {
        relay_blocked &= ~(1u << board);
    }
    Serial.printf("RELAY %s Board %u: flood frames heard directly from it "
                  "are %s\n",
        on ? "blocking" : "unblocking", board,
        on ? "ignored (as if out of range)" : "taken again");
}

// Drops everything sent and received this way, for link-loss tests.
void relay_set_muted(relay_net &net, bool on)
{
    net.muted = on;
    Serial.printf("%s mute %s | dropped so far (both ways): sent %lu "
                  "received %lu\n",
        net.tag, net.muted ? "on (nothing sent or received)" : "off",
        static_cast<unsigned long>(relay_muted_tx),
        static_cast<unsigned long>(relay_muted_rx));
}

// Flood voice in pairs (app_config::flood_pair_default): this board's own
// streams, from the next frame. The receiving board needs no setting.
void flood_set_pair(bool on)
{
    flood_pair = on;
    Serial.printf("FLOOD pair %s%s\n",
        on ? "on (25 packets a second, each frame sent once)" :
             "off (50 packets a second, each with the frame before it)",
        on && !redundancy ? " | no effect while redundancy is off" : "");
}

// Serial commands. accept, decline and disconnect act on the flood
// connection while one is in progress; hello, talk, test and stop follow
// it on their own (voice_via()).
bool run_relay_command(const char *line, uint32_t now)
{
    relay_net &net = flood_net;
    if (strncmp(line, "flood", 5) == 0)
    {
        const char *rest = line + 5;
        if (*rest == '\0')
        {
            print_relay(net);
            return true;
        }
        if (strcmp(rest, " mute on") == 0 || strcmp(rest, " mute off") == 0)
        {
            relay_set_muted(net, strcmp(rest, " mute on") == 0);
            return true;
        }
        if (strcmp(rest, " pair on") == 0 || strcmp(rest, " pair off") == 0)
        {
            flood_set_pair(strcmp(rest, " pair on") == 0);
            return true;
        }
        if (strncmp(rest, " connect", 8) == 0 &&
            (rest[8] == '\0' || isspace(static_cast<unsigned char>(rest[8]))))
        {
            uint8_t peer = 0;
            if (command_parser::board_id(rest + 8, peer))
            {
                place_relay_call(net, peer, now);
            }
            else
            {
                Serial.printf("%s LINK refused: invalid board ID\n", net.tag);
            }
            return true;
        }
    }
    if (strcmp(line, "relay block") == 0)
    {
        print_blocked();
        return true;
    }
    const bool block = strncmp(line, "relay block ", 12) == 0;
    const bool unblock = strncmp(line, "relay unblock ", 14) == 0;
    if (block || unblock)
    {
        uint8_t board = 0;
        if (!command_parser::board_id(line + (block ? 11 : 13), board) ||
            board == own_id)
        {
            Serial.println("RELAY usage: relay block N | relay unblock N "
                           "(another board's ID)");
            return true;
        }
        relay_set_block(board, block);
        return true;
    }
    return net.calls.current() != call::state::idle &&
        run_link_command(net.calls, way(net.link).tag, line, now);
}
