// Included inside main.cpp's anonymous namespace, after mesh.inc.
//
// ESP-NOW multi-hop: everything ESP-NOW has (connections, push-to-talk
// voice, RIGHT hello, presence), carried across boards that relay, in two
// ways (relay_frame.h has the frames):
//
//   Flood mesh    Every board re-broadcasts every frame it hasn't seen
//                 before, up to relay_max_hops transmissions. All paths at
//                 once: nothing to set up and nothing to repair, but each
//                 frame is sent once by every board that hears it.
//   All nodes     Discovery floods bounded, distinct candidate paths. A
//                 unicast reply confirms the reverse direction. Data carries
//                 the full path, and an active call uses it in both directions.
//
// Both are true mesh topologies: every board relays, and there is no root
// and no router (ESP-NOW needs neither). Each has its own session key per
// peer, made over itself, its own connections (call.h), presence pings and
// route line, as ESP-MESH does (mesh.inc). Frames are sealed end to end as
// over ESP-NOW; a relay checks the network-key hop tag before passing a
// frame on, and can't open it.
//
// Serial `relay block N` makes this board ignore relayed frames heard
// directly from board N, as if N were out of range, so a middleman can be
// tested on one desk (block each way).

static_assert(radio::min_frame_size <= relay_frame::min_route_size,
    "the receive callback would drop route messages");

// A board over a relay way: the session and presence state ESP-NOW keeps
// (peer_state), and the path its frames take.
struct relay_peer : peer_state
{
    // The path the last frame from it took, origin first (route line).
    uint8_t heard_path[relay_frame::max_path] = {};
    uint8_t heard_length = 0;
    uint32_t heard_ms = 0;
};

// Routed relay: how this board reaches one board, and its discovery.
struct relay_route
{
    bool valid = false;
    uint8_t next_hop = 0;
    uint8_t path_length = 0;
    uint8_t path[relay_frame::max_path] = {};  // This board first.
    uint32_t learned_ms = 0;
    bool searching = false;  // A request is out, retried until answered.
    uint8_t tries = 0;
    uint16_t request_id = 0;
    uint32_t asked_ms = 0;
    uint32_t refreshed_ms = 0;  // Last background request.
};

struct routed_path
{
    uint8_t length = 0;
    uint8_t boards[relay_frame::max_path] = {};

    bool to(uint8_t peer) const
    {
        return length >= 2 && boards[length - 1] == peer;
    }
};

struct relay_net
{
    relay_net(const char *tag_in, via link_in, uint8_t marker_in,
        call::machine &calls_in) :
        tag(tag_in), link(link_in), marker(marker_in), calls(calls_in)
    {
    }
    const char *tag;  // Log prefix: FLOOD, ROUTED.
    via link;
    uint8_t marker;
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
    uint32_t no_route = 0;    // Routed: nowhere to send it.
    uint32_t send_errors = 0;
    uint32_t requests = 0;    // Routed: route requests this board sent.
};

relay_net flood_net("FLOOD", via::flood, relay_frame::marker_flood,
    flood_calls);
relay_net routed_net("ROUTED", via::routed, relay_frame::marker_routed,
    routed_calls);
relay_net *const relay_nets[] = {&flood_net, &routed_net};

// The routed relay's routes, by target, and the route requests it has
// seen by origin, request ID and ordered candidate path.
relay_route routed_routes[max_id + 1];
relay_frame::seen_cache routed_requests;
uint16_t routed_request_id = 0;
routed_path routed_call_path;
// Only set while processing an authenticated incoming message in loop().
const routed_path *routed_reply_path = nullptr;
uint32_t routed_end_count = 0;
uint32_t relay_blocked = 0;  // Bit N: ignore relayed frames from board N.
uint32_t relay_muted_tx = 0;
uint32_t relay_muted_rx = 0;

relay_net &relay_of(via link)
{
    return link == via::routed ? routed_net : flood_net;
}

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
bool relay_peer_online(via link, uint8_t peer, uint32_t now)
{
    const relay_peer &state = relay_of(link).peers[peer];
    return state.link.established() &&
        within_presence(state.last_accept_ms, now);
}

void relay_reject(relay_net &net, const radio::received_frame &frame,
    const char *reason)
{
    log_reject(way(net.link).tag, net.rejected, frame.mac, frame.size,
        reason);
}

// Routed relay: routes

// The route to `target` if there is a usable one. One not refreshed for
// the route lifetime is dropped here.
relay_route *routed_route(uint8_t target, uint32_t now)
{
    if (target < 1 || target > max_id)
    {
        return nullptr;
    }
    relay_route &route = routed_routes[target];
    if (!route.valid)
    {
        return nullptr;
    }
    if (now - route.learned_ms >= app_config::relay_route_lifetime_ms)
    {
        route.valid = false;
        Serial.printf("ROUTED route to %u expired\n", target);
        return nullptr;
    }
    return &route;
}

void routed_drop_via(uint8_t neighbour, const char *why)
{
    for (uint8_t id = 1; id <= max_id; ++id)
    {
        relay_route &route = routed_routes[id];
        if (route.valid && route.next_hop == neighbour)
        {
            route.valid = false;
            Serial.printf("ROUTED route to %u dropped: %s\n", id, why);
        }
    }
}

// Only an answer to our current discovery may replace this cache entry.
// Active calls keep a separate copy even when a new route is found.
void routed_learn(const uint8_t *path, uint8_t length, uint32_t now)
{
    const uint8_t target = path[length - 1];
    relay_route &route = routed_routes[target];
    const bool changed = !route.valid || route.path_length != length ||
        memcmp(route.path, path, length) != 0;
    route.valid = true;
    route.next_hop = path[1];
    route.path_length = length;
    memcpy(route.path, path, length);
    route.learned_ms = now;
    route.refreshed_ms = now;
    if (changed)
    {
        char text[40];
        path_text(path, length, text, sizeof(text));
        Serial.printf("ROUTED route to %u (%s): %s\n", target,
            device_name(target), text);
    }
}

void routed_send(const relay_frame::route_message &message,
    const uint8_t *mac)
{
    if (routed_net.muted)
    {
        ++relay_muted_tx;
        return;
    }
    uint8_t out[relay_frame::route_fixed_size + relay_frame::max_path +
        relay_frame::route_tag_size];
    size_t size = 0;
    if (!relay_frame::build_route(e2e_keys::network, message, out,
            sizeof(out), &size) ||
        radio::send(mac, out, size) != ESP_OK)
    {
        ++routed_net.send_errors;
    }
}

const uint8_t *routed_mac(uint8_t board)
{
    const network_config::device *entry = radio::find_device(board);
    return entry ? entry->mac : nullptr;
}

// Floods a route request for `target`.
void routed_ask(uint8_t target, uint32_t now)
{
    // Most nearby boards answer without flooding through the whole mesh.
    // Widen the search on each retry, up to the full five-board path.
    constexpr uint8_t probe_hops[] = {1, 2, app_config::relay_max_hops};
    static_assert(sizeof(probe_hops) == app_config::relay_request_tries,
        "each route request attempt needs a hop limit");
    relay_route &route = routed_routes[target];
    const uint8_t hops = probe_hops[route.tries];
    ++route.tries;
    route.searching = true;
    route.asked_ms = now;
    route.refreshed_ms = now;
    relay_frame::route_message request = {};
    request.kind = relay_frame::route_request;
    request.origin = own_id;
    request.target = target;
    route.request_id = ++routed_request_id;
    request.request_id = route.request_id;
    request.ttl = hops;
    request.path_length = 1;
    request.path[0] = own_id;
    ++routed_net.requests;
    routed_send(request, radio::broadcast_mac);
}

// This board needs a route to `target`: asks now, unless a request is out
// already (service_routed retries it) or nobody answered a moment ago.
void routed_discover(uint8_t target, uint32_t now)
{
    relay_route &route = routed_routes[target];
    if (route.searching ||
        (route.tries >= app_config::relay_request_tries &&
            now - route.asked_ms < app_config::relay_request_backoff_ms))
    {
        return;
    }
    route.tries = 0;
    Serial.printf("ROUTED looking for a route to %u (%s)\n", target,
        device_name(target));
    routed_ask(target, now);
}

bool routed_known_path(const uint8_t *path, size_t length)
{
    if (length > app_config::relay_max_hops + 1u ||
        !relay_frame::simple_path(path, length, max_id))
    {
        return false;
    }
    for (size_t i = 0; i < length; ++i)
    {
        if (path[i] != own_id && !valid_peer(path[i]))
        {
            return false;
        }
    }
    return true;
}

void routed_on_request(relay_frame::route_message request, uint8_t sender,
    uint32_t now)
{
    if ((request.target != own_id && !valid_peer(request.target)) ||
        request.origin == request.target || request.ttl == 0 ||
        request.path_length + request.ttl >
            app_config::relay_max_hops + 1 ||
        !routed_known_path(request.path, request.path_length) ||
        request.path[0] != request.origin ||
        request.path[request.path_length - 1] != sender ||
        memchr(request.path, own_id, request.path_length) ||
        memchr(request.path, request.target, request.path_length))
    {
        return;
    }
    // Keep alternative paths even if an earlier request got here through
    // a one-way radio link. Each prefix uses at most four board IDs.
    uint32_t candidate = 0;
    for (size_t i = 0; i < request.path_length; ++i)
    {
        candidate = (candidate << 8) | request.path[i];
    }
    if (!routed_requests.first(request.origin, request.request_id, now,
            app_config::relay_seen_window_ms, candidate))
    {
        return;
    }
    request.path[request.path_length++] = own_id;
    if (request.target == own_id)
    {
        // Answer back along the request's path, with the whole route.
        relay_frame::route_message reply = request;
        reply.kind = relay_frame::route_reply;
        const uint8_t *mac = routed_mac(sender);
        if (mac)
        {
            routed_send(reply, mac);
        }
        return;
    }
    if (request.ttl <= 1)
    {
        return;
    }
    --request.ttl;
    routed_send(request, radio::broadcast_mac);
}

void routed_on_reply(const relay_frame::route_message &reply, uint8_t sender,
    uint32_t now)
{
    const uint8_t length = reply.path_length;
    if (!routed_known_path(reply.path, length))
    {
        return;
    }
    size_t at = length;
    for (size_t i = 0; i < length; ++i)
    {
        if (reply.path[i] == own_id)
        {
            at = i;
            break;
        }
    }
    if (at + 1 >= length || reply.path[at + 1] != sender ||
        reply.path[0] != reply.origin ||
        reply.path[length - 1] != reply.target)
    {
        return;
    }
    if (at == 0)
    {
        relay_route &route = routed_routes[reply.target];
        if (route.searching && route.request_id == reply.request_id)
        {
            routed_learn(reply.path, length, now);
            route.searching = false;
            route.tries = 0;
        }
        return;
    }
    const uint8_t *mac = routed_mac(reply.path[at - 1]);
    if (mac && !relay_is_blocked(reply.path[at - 1]))
    {
        routed_send(reply, mac);
    }
}

void routed_control(const radio::received_frame &frame, uint32_t now)
{
    const uint8_t sender = relay_board_of(frame.mac);
    if (relay_is_blocked(sender))
    {
        ++routed_net.blocked;
        return;
    }
    if (routed_net.muted)
    {
        ++relay_muted_rx;
        return;
    }
    relay_frame::route_message message = {};
    if (!relay_frame::parse_route(e2e_keys::network, frame.bytes, frame.size,
            &message))
    {
        relay_reject(routed_net, frame, "bad route message");
        return;
    }
    switch (message.kind)
    {
    case relay_frame::route_request:
        routed_on_request(message, sender, now);
        break;
    case relay_frame::route_reply:
        routed_on_reply(message, sender, now);
        break;
    }
}

// Retries requests, refreshes routes in use, and drops routes whose first
// hop stopped acknowledging (the radio's ESP-NOW acknowledgements, as for
// direct ESP-NOW links).
void service_routed(uint32_t now)
{
    for (uint8_t target = 1; target <= max_id; ++target)
    {
        if (!valid_peer(target))
        {
            continue;
        }
        relay_route &route = routed_routes[target];
        if (route.valid && acks_lost(route.next_hop, now))
        {
            routed_drop_via(route.next_hop, "first hop not acknowledging");
        }
        if (route.searching &&
            now - route.asked_ms >= app_config::relay_request_retry_ms)
        {
            if (route.tries < app_config::relay_request_tries)
            {
                routed_ask(target, now);
            }
            else
            {
                route.searching = false;
                Serial.printf("ROUTED no answer from %u (%s) after %u "
                              "requests; asking again in %lu s if needed\n",
                    target, device_name(target), route.tries,
                    static_cast<unsigned long>(
                        app_config::relay_request_backoff_ms / 1000));
            }
        }
        // In use and due: look again, keeping this route meanwhile.
        if (route.valid && !route.searching &&
            routed_net.peers[target].link.established() &&
            now - route.refreshed_ms >= app_config::relay_route_refresh_ms)
        {
            route.tries = 0;
            routed_ask(target, now);
        }
    }
}

// Sending

void routed_trace(const char *event, const relay_frame::envelope &head)
{
    char text[40];
    path_text(head.path, head.path_length, text, sizeof(text));
    Serial.printf("ROUTED TEXT path %s id=%u at=%u %s\n", event, head.id,
        own_id, text);
}

void routed_pin(const uint8_t *path, uint8_t length)
{
    routed_call_path.length = length;
    memcpy(routed_call_path.boards, path, length);
    char text[40];
    path_text(path, length, text, sizeof(text));
    Serial.printf("ROUTED call path %08lX: %s\n",
        static_cast<unsigned long>(routed_calls.call_id()), text);
}

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

// Sends a secure_message frame from this board toward `destination`: the
// flood mesh broadcasts it; the routed relay hands it to the route's first
// hop, or looks for a route (the frame is dropped; resends and the next
// frames go once it is found).
esp_err_t relay_send_inner(relay_net &net, uint8_t destination,
    const uint8_t *inner, size_t inner_size, uint32_t now,
    bool call_traffic = false, bool trace_text = false)
{
    if (net.muted)
    {
        ++relay_muted_tx;
        return ESP_OK;  // Gone, as out of range: timeouts notice.
    }
    relay_frame::envelope head = {};
    head.marker = net.marker;
    head.ttl = app_config::relay_max_hops;
    head.id = ++net.next_id;
    head.path_length = 1;
    head.path[0] = own_id;
    net.seen.first(own_id, head.id, now, app_config::relay_seen_window_ms);
    const uint8_t *mac = radio::broadcast_mac;
    if (net.link == via::routed)
    {
        if (routed_reply_path && routed_reply_path->to(destination))
        {
            head.path_length = routed_reply_path->length;
            memcpy(head.path, routed_reply_path->boards, head.path_length);
        }
        else if (call_traffic && routed_call_path.to(destination))
        {
            head.path_length = routed_call_path.length;
            memcpy(head.path, routed_call_path.boards, head.path_length);
        }
        else if (const relay_route *route = routed_route(destination, now))
        {
            head.path_length = route->path_length;
            memcpy(head.path, route->path, head.path_length);
        }
        else
        {
            ++net.no_route;
            routed_discover(destination, now);
            return ESP_ERR_NOT_FOUND;
        }
        head.ttl = head.path_length - 1;
        mac = routed_mac(head.path[1]);
        if (!mac || relay_is_blocked(head.path[1]))
        {
            ++net.no_route;
            return ESP_ERR_NOT_FOUND;
        }
    }
    const esp_err_t result =
        relay_transmit(net, mac, head, inner, inner_size, false);
    if (result == ESP_OK && net.link == via::routed && trace_text)
    {
        routed_trace("tx", head);
    }
    return result;
}

esp_err_t relay_send_payload(via link, uint8_t peer, const uint8_t *payload,
    size_t size, uint8_t type)
{
    relay_net &net = relay_of(link);
    uint8_t inner[relay_frame::max_inner_size] = {};
    size_t inner_size = 0;
    if (!seal_payload(net.peers[peer].link, way(link).tag, type, payload,
            size, inner, sizeof(inner), &inner_size))
    {
        return ESP_FAIL;
    }
    bool call_traffic = net.calls.current() != call::state::idle &&
        net.calls.peer() == peer;
    if (link == via::routed && type == secure_message::type_voice)
    {
        call::message message = {};
        if (call::parse(payload, size, &message))
        {
            // Final messages may still be queued after the call ends.
            call_traffic = peer == net.calls.peer() &&
                message.call_id == net.calls.call_id();
        }
    }
    return relay_send_inner(net, peer, inner, inner_size, millis(),
        call_traffic, type == secure_message::type_text);
}

void relay_send_control(relay_net &net, uint8_t destination,
    const control_frame::message &content, uint32_t now)
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
    relay_send_inner(net, destination, inner, size, now);
}

// Receive

routed_path routed_reverse(const relay_frame::envelope &head)
{
    routed_path path;
    path.length = head.path_length;
    for (size_t i = 0; i < path.length; ++i)
    {
        path.boards[i] = head.path[path.length - 1 - i];
    }
    return path;
}

bool routed_matches_call(const routed_path &path)
{
    return path.length == routed_call_path.length &&
        memcmp(path.boards, routed_call_path.boards, path.length) == 0;
}

// Call replies are queued by the existing state machine. Drain earlier
// work first, then send this frame's response using its exact reverse path.
const char *routed_take_payload(uint8_t source, uint8_t type,
    const uint8_t *payload, size_t size, int64_t arrival, uint32_t now,
    const routed_path &back)
{
    call::message message = {};
    const bool call_message = type == secure_message::type_voice &&
        call::parse(payload, size, &message);
    const bool active_peer = routed_calls.current() != call::state::idle &&
        routed_calls.peer() == source;
    const uint32_t prior_id = routed_calls.call_id();
    if (active_peer && (!call_message || message.call_id == prior_id) &&
        !routed_matches_call(back))
    {
        return "frame outside the call path";
    }

    if (call_message)
    {
        send_call_messages(via::routed);
    }
    routed_reply_path = &back;
    const char *problem =
        take_payload(via::routed, source, type, payload, size, arrival, now);
    if (call_message)
    {
        if (routed_calls.current() == call::state::ringing &&
            routed_calls.peer() == source &&
            routed_calls.call_id() == message.call_id &&
            (!active_peer || prior_id != message.call_id))
        {
            routed_pin(back.boards, back.length);
        }
        send_call_messages(via::routed);
    }
    routed_reply_path = nullptr;
    return problem;
}

// A relayed frame addressed to this board, as handle_mesh() takes the
// mesh's.
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
            const routed_path back = routed_reverse(head);
            if (net.link == via::routed)
            {
                routed_reply_path = &back;
            }
            const char *problem =
                take_session(net.link, fields.source, content, now);
            routed_reply_path = nullptr;
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
    if (net.link == via::routed)
    {
        const char *problem = routed_take_payload(fields.source, opened.type,
            payload, size, arrival, now, routed_reverse(head));
        if (problem)
        {
            relay_reject(net, frame, problem);
            return;
        }
    }
    ++net.delivered;
    state.last_accept_ms = now;
    memcpy(state.heard_path, head.path, head.path_length);
    state.heard_length = head.path_length;
    state.heard_ms = now;
    if (net.link == via::flood)
    {
        const char *problem = take_payload(net.link, fields.source,
            opened.type, payload, size, arrival, now);
        if (problem)
        {
            relay_reject(net, frame, problem);
        }
    }
    else if (opened.type == secure_message::type_text)
    {
        routed_trace("rx", head);
    }
}

// Passes a frame for another board on: the flood mesh broadcasts it again,
// the routed relay hands it to the next board in the fixed source path.
void relay_forward(relay_net &net, relay_frame::envelope head,
    const secure_message::header &fields, uint8_t next_hop)
{
    if (head.ttl <= 1)
    {
        ++net.hop_drops;
        return;
    }
    --head.ttl;
    const uint8_t *mac = radio::broadcast_mac;
    if (net.link == via::routed)
    {
        mac = routed_mac(next_hop);
        if (!mac || relay_is_blocked(next_hop))
        {
            ++net.no_route;
            return;
        }
    }
    else
    {
        if (head.path_length >= relay_frame::max_path)
        {
            ++net.hop_drops;
            return;
        }
        head.path[head.path_length++] = own_id;
    }
    const esp_err_t result =
        relay_transmit(net, mac, head, head.inner, head.inner_size, true);
    if (result == ESP_OK && net.link == via::routed &&
        fields.type == secure_message::type_text)
    {
        routed_trace("hop", head);
    }
}

// A flood or routed frame (true; taken here), else false: handle() takes
// it as a direct ESP-NOW frame.
bool relay_handle(const radio::received_frame &frame, uint32_t now)
{
    const uint8_t marker = frame.size > 0 ? frame.bytes[0] : 0;
    if (marker == relay_frame::marker_route)
    {
        routed_control(frame, now);
        return true;
    }
    relay_net *found = marker == relay_frame::marker_flood ? &flood_net :
        marker == relay_frame::marker_routed ? &routed_net : nullptr;
    if (!found)
    {
        return false;
    }
    relay_net &net = *found;
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
    secure_message::header fields = {};
    const status verified = secure_message::verify_hop(e2e_keys::network,
        head.inner, head.inner_size, &fields);
    if (verified != status::ok)
    {
        relay_reject(net, frame, secure_message::status_name(verified));
        return true;
    }
    if (fields.source == own_id)
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
    uint8_t next_hop = 0;
    if (net.link == via::routed)
    {
        if (!relay_frame::routed_hop(head, fields.source, fields.destination,
                sender, own_id, max_id, &next_hop))
        {
            relay_reject(net, frame, "not on the source path");
            return true;
        }
    }
    else
    {
        if (head.path[0] != fields.source ||
            head.path[head.path_length - 1] != sender)
        {
            relay_reject(net, frame, "path doesn't match the frame");
            return true;
        }
        if (memchr(head.path, own_id, head.path_length))
        {
            ++net.copies;
            return true;
        }
    }
    if (!net.seen.first(fields.source, head.id, now,
            app_config::relay_seen_window_ms))
    {
        ++net.copies;
        return true;
    }
    if (fields.destination == own_id)
    {
        relay_deliver(net, frame, head, fields, now);
    }
    else
    {
        relay_forward(net, head, fields, next_hop);
    }
    return true;
}

// Routes on screen

// The path a connection to `peer` takes, this board first: the route this
// board sends along (routed relay), or the path the last frame from the
// peer took, turned round (flood mesh: every path is used; this is the one
// whose copy arrived first).
void relay_route_text(via link, uint8_t peer, uint32_t now, char *text,
    size_t capacity)
{
    uint8_t path[relay_frame::max_path + 1];
    size_t length = 0;
    if (link == via::routed)
    {
        if (routed_calls.current() != call::state::idle &&
            routed_call_path.to(peer))
        {
            length = routed_call_path.length;
            memcpy(path, routed_call_path.boards, length);
        }
        else if (const relay_route *route = routed_route(peer, now))
        {
            length = route->path_length;
            memcpy(path, route->path, length);
        }
    }
    else
    {
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
    }
    path_text(path, length, text, capacity);
}

// Every Wi-Fi way (declared in handset.inc; here, after the last way)

peer_state &peer_via(via link, uint8_t peer)
{
    switch (link)
    {
    case via::mesh: return mesh_peers[peer];
    case via::flood:
    case via::routed: return relay_of(link).peers[peer];
    case via::espnow: break;
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
    case via::flood:
    case via::routed:
        relay_send_control(relay_of(link), destination, content, now);
        return;
    case via::espnow: break;
    }
    send_control(control_frame::type_session, destination,
        radio::find_device(destination)->mac, content);
}

void route_text_via(via link, uint8_t peer, uint32_t now, char *text,
    size_t capacity)
{
    if (link == via::mesh)
    {
        mesh_route_text(peer, now, text, capacity);
    }
    else
    {
        relay_route_text(link, peer, now, text, capacity);
    }
}

// Why `peer` looks gone over `link` (it didn't reboot): for the log line
// that ends the connection.
const char *gone_reason(via link, uint8_t peer, uint32_t now)
{
    switch (link)
    {
    case via::espnow:
        return acks_lost(peer, now) ? "stopped acknowledging (radio)" :
            "went offline";
    case via::mesh:
        return !mesh_ready() ? "can't be reached: this board left the mesh" :
            mesh_sends_failing(peer, now) ? "can't be reached (sends failing)" :
            "went offline";
    case via::flood:
    case via::routed: break;
    }
    return "went offline";
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
    if (!relay_peer_online(net.link, peer, now))
    {
        Serial.printf("%s LINK refused: %u is offline (%s)\n", net.tag, peer,
            net.peers[peer].link.established() ?
                "no presence ping for a while" : "no session");
        return false;
    }
    const uint32_t id = random_id();
    net.calls.set_unavailable(false);
    if (net.link == via::routed)
    {
        const relay_route *route = routed_route(peer, now);
        if (!route)
        {
            routed_discover(peer, now);
            Serial.printf("ROUTED LINK waiting for a path to %u\n", peer);
            return false;
        }
        send_call_messages(net.link);
        if (!dial(net.calls, way(net.link).tag, peer, id, now))
        {
            return false;
        }
        routed_pin(route->path, route->path_length);
        return true;
    }
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
                  "rejected %lu blocked %lu out of hops %lu send errors %lu",
        static_cast<unsigned long>(net.sent),
        static_cast<unsigned long>(net.relayed),
        static_cast<unsigned long>(net.copies),
        static_cast<unsigned long>(net.delivered),
        static_cast<unsigned long>(net.rejected),
        static_cast<unsigned long>(net.blocked),
        static_cast<unsigned long>(net.hop_drops),
        static_cast<unsigned long>(net.send_errors));
    if (net.link == via::routed)
    {
        Serial.printf(" no route %lu requests %lu",
            static_cast<unsigned long>(net.no_route),
            static_cast<unsigned long>(net.requests));
    }
    Serial.printf("%s | sessions", net.muted ? " | MUTED" : "");
    print_sessions(net.link, now);
    Serial.println();
}

void print_routes()
{
    const uint32_t now = millis();
    Serial.print("ROUTES |");
    for (uint8_t target = 1; target <= max_id; ++target)
    {
        if (!valid_peer(target))
        {
            continue;
        }
        const relay_route &route = routed_routes[target];
        if (route.valid)
        {
            char text[40];
            path_text(route.path, route.path_length, text, sizeof(text));
            Serial.printf(" to %u: %s, %lu s old |", target, text,
                static_cast<unsigned long>((now - route.learned_ms) / 1000));
        }
        else
        {
            Serial.printf(" to %u: %s |", target,
                route.searching ? "looking" : "none");
        }
    }
    Serial.printf(" blocked:");
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
                entry.key);
            routed_net.peers[entry.peer].link.begin(own_id, entry.peer,
                entry.key);
        }
    }
}

void setup_relay()
{
    for (relay_net *net : relay_nets)
    {
        net->calls.begin(own_id);  // ESP-NOW's timings.
        net->calls.seed(esp_random());
        net->next_id = static_cast<uint16_t>(esp_random());
    }
    routed_request_id = static_cast<uint16_t>(esp_random());
    Serial.printf("RELAY ESP-NOW: Flood mesh and ESP-MESH(ALLNODES) | "
                  "up to %u hops | no router, no root\n",
        app_config::relay_max_hops);
    const uint32_t now = millis();
    for (uint8_t peer = 1; peer <= max_id; ++peer)
    {
        for (relay_net *net : relay_nets)
        {
            if (net->peers[peer].link.paired())
            {
                start_session(net->link, peer, now);
            }
        }
    }
}

void service_relay(uint32_t now)
{
    service_routed(now);
    for (relay_net *net : relay_nets)
    {
        service_sessions(net->link, now, false);
        service_presence(net->link, now, true, false, presence_ping,
            sizeof(presence_ping));
        service_calls(net->link, now);
    }
    if (routed_calls.end_count() != routed_end_count)
    {
        routed_end_count = routed_calls.end_count();
        const auto reason = routed_calls.last_end();
        if (routed_calls.current() == call::state::idle &&
            (reason == call::end_reason::link_lost ||
                reason == call::end_reason::unreachable ||
                reason == call::end_reason::no_answer))
        {
            const uint8_t peer = routed_calls.peer();
            relay_route &route = routed_routes[peer];
            if (route.path_length == routed_call_path.length &&
                memcmp(route.path, routed_call_path.boards,
                    route.path_length) == 0)
            {
                route.valid = false;
            }
            if (!route.valid)
            {
                routed_discover(peer, now);
            }
        }
    }
}

// Test settings, from the Tools page (ui.inc) or serial.

// Ignores (or takes again) flood and routed frames heard directly from
// `board`, as if it were out of range: set on both boards, frames between
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
        routed_drop_via(board, "blocked");
    }
    else
    {
        relay_blocked &= ~(1u << board);
    }
    Serial.printf("RELAY %s Board %u: flood and routed frames heard directly "
                  "from it are %s\n",
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

// Serial commands. accept, decline and disconnect act on the flood or
// routed connection while one is in progress; hello, talk, test and stop
// follow it on their own (voice_via()).
bool run_relay_command(const char *line, uint32_t now)
{
    for (relay_net *each : relay_nets)
    {
        relay_net &net = *each;
        const char *name = way(net.link).name;  // "flood", "routed"
        const size_t name_size = strlen(name);
        if (strncmp(line, name, name_size) != 0)
        {
            continue;
        }
        const char *rest = line + name_size;
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
    if (strcmp(line, "routes") == 0)
    {
        print_routes();
        return true;
    }
    if (strcmp(line, "relay block") == 0)
    {
        print_routes();
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
    for (relay_net *net : relay_nets)
    {
        if (net->calls.current() != call::state::idle &&
            run_link_command(net->calls, way(net->link).tag, line, now))
        {
            return true;
        }
    }
    return false;
}
