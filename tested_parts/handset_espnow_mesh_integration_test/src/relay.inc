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
//   Routed relay  A board finds a path first (route discovery: its request
//                 floods once, the target answers back along the path the
//                 request took), then each frame goes hop by hop, unicast,
//                 acknowledged by the radio, along that path only. A broken
//                 path is reported back (route error) and found again; a
//                 path in use is looked for again in the background so a
//                 shorter one replaces it.
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

struct relay_peer
{
    session::link link;
    uint32_t last_hello_ms = 0;
    uint32_t hellos = 0;  // Sent since the handshake started.
    uint32_t last_accept_ms = 0;
    uint32_t last_resync_ms = 0;
    uint32_t last_presence_ms = 0;  // Last presence ping sent.
    bool shown_online = false;      // Last state logged as PEER.
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
    uint32_t asked_ms = 0;
    uint32_t refreshed_ms = 0;  // Last background request.
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
    relay_route routes[max_id + 1];  // Routed relay only.
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

// Route requests seen, by (origin, request ID): each is relayed once.
relay_frame::seen_cache routed_requests;
uint16_t routed_request_id = 0;
uint32_t routed_error_ms[max_id + 1] = {};  // Last route error, by target.
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
        static_cast<int32_t>(now - state.last_accept_ms) <
            static_cast<int32_t>(app_config::presence_timeout_ms);
}

void relay_reject(relay_net &net, const radio::received_frame &frame,
    const char *reason)
{
    ++net.rejected;
    Serial.printf("%s REJECT ", net.tag);
    radio::print_mac(frame.mac);
    Serial.printf(" | %u bytes | %s\n", frame.size, reason);
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
    relay_route &route = routed_net.routes[target];
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
        relay_route &route = routed_net.routes[id];
        if (route.valid && route.next_hop == neighbour)
        {
            route.valid = false;
            Serial.printf("ROUTED route to %u dropped: %s\n", id, why);
        }
    }
}

// Takes `path` (this board first, a listed board at every step) as the
// route to its last board if there is none, it is shorter, the old one is
// due for a refresh, or it answers this board's own request.
void routed_learn(const uint8_t *path, size_t length, uint32_t now,
    bool own_answer)
{
    if (length < 2 || length > relay_frame::max_path || path[0] != own_id ||
        relay_is_blocked(path[1]))
    {
        return;
    }
    for (size_t i = 1; i < length; ++i)
    {
        if (!valid_peer(path[i]))
        {
            return;
        }
    }
    const uint8_t target = path[length - 1];
    relay_route &route = routed_net.routes[target];
    const bool take = own_answer || !route.valid ||
        length < route.path_length ||
        now - route.learned_ms >= app_config::relay_route_refresh_ms;
    if (!take)
    {
        return;
    }
    const bool changed = !route.valid || route.path_length != length ||
        memcmp(route.path, path, length) != 0;
    route.valid = true;
    route.next_hop = path[1];
    route.path_length = static_cast<uint8_t>(length);
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
    relay_route &route = routed_net.routes[target];
    ++route.tries;
    route.asked_ms = now;
    route.refreshed_ms = now;
    relay_frame::route_message request = {};
    request.kind = relay_frame::route_request;
    request.origin = own_id;
    request.target = target;
    request.request_id = ++routed_request_id;
    request.ttl = app_config::relay_max_hops;
    request.path_length = 1;
    request.path[0] = own_id;
    // Its echoes from the neighbours are copies.
    routed_requests.first(own_id, request.request_id, now,
        app_config::relay_seen_window_ms);
    ++routed_net.requests;
    routed_send(request, radio::broadcast_mac);
}

// This board needs a route to `target`: asks now, unless a request is out
// already (service_routed retries it) or nobody answered a moment ago.
void routed_discover(uint8_t target, uint32_t now)
{
    relay_route &route = routed_net.routes[target];
    if (route.searching ||
        (route.tries >= app_config::relay_request_tries &&
            now - route.asked_ms < app_config::relay_request_backoff_ms))
    {
        return;
    }
    route.searching = true;
    route.tries = 0;
    Serial.printf("ROUTED looking for a route to %u (%s)\n", target,
        device_name(target));
    routed_ask(target, now);
}

// A frame from `origin` for `target` can't go on from here: tell `origin`
// so it looks for another route.
void routed_report(uint8_t origin, uint8_t target, uint32_t now)
{
    if (origin == own_id)
    {
        routed_discover(target, now);
        return;
    }
    if (now - routed_error_ms[target] < app_config::relay_error_gap_ms)
    {
        return;
    }
    routed_error_ms[target] = now;
    const relay_route *back = routed_route(origin, now);
    const uint8_t *mac = back ? routed_mac(back->next_hop) : nullptr;
    if (!mac)
    {
        return;  // No way to tell it; its own timeouts notice.
    }
    relay_frame::route_message error = {};
    error.kind = relay_frame::route_error;
    error.origin = origin;
    error.target = target;
    error.ttl = 1;
    error.path_length = 1;
    error.path[0] = own_id;
    routed_send(error, mac);
    Serial.printf("ROUTED no route to %u for a frame from %u: route error "
                  "sent\n",
        target, origin);
}

void routed_on_request(relay_frame::route_message request, uint8_t sender,
    uint32_t now)
{
    if (request.origin == own_id || !valid_peer(request.origin) ||
        request.path[0] != request.origin ||
        request.path[request.path_length - 1] != sender)
    {
        return;
    }
    for (size_t i = 0; i < request.path_length; ++i)
    {
        if (request.path[i] == own_id)
        {
            return;  // It looped back.
        }
    }
    if (!routed_requests.first(request.origin, request.request_id, now,
            app_config::relay_seen_window_ms) ||
        request.path_length >= relay_frame::max_path)
    {
        return;
    }
    // The way back to the asker: this board, then the request's path
    // backwards.
    uint8_t back[relay_frame::max_path];
    size_t length = 0;
    back[length++] = own_id;
    for (size_t i = request.path_length; i-- > 0;)
    {
        back[length++] = request.path[i];
    }
    routed_learn(back, length, now, false);
    if (request.target == own_id)
    {
        // Answer back along the request's path, with the whole route.
        relay_frame::route_message reply = request;
        reply.kind = relay_frame::route_reply;
        reply.path[reply.path_length++] = own_id;
        const uint8_t *mac = routed_mac(sender);
        if (mac)
        {
            routed_send(reply, mac);
        }
        char text[40];
        path_text(reply.path, reply.path_length, text, sizeof(text));
        Serial.printf("ROUTED route request from %u answered: %s\n",
            request.origin, text);
        return;
    }
    if (request.ttl <= 1)
    {
        return;
    }
    --request.ttl;
    request.path[request.path_length++] = own_id;
    routed_send(request, radio::broadcast_mac);
}

void routed_on_reply(const relay_frame::route_message &reply, uint8_t sender,
    uint32_t now)
{
    const size_t length = reply.path_length;
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
    // Routes to every board on the path, both ways from this one.
    uint8_t way[relay_frame::max_path];
    for (size_t j = at + 1; j < length; ++j)
    {
        size_t n = 0;
        for (size_t k = at; k <= j; ++k)
        {
            way[n++] = reply.path[k];
        }
        routed_learn(way, n, now, at == 0 && j == length - 1);
    }
    for (size_t j = 0; j < at; ++j)
    {
        size_t n = 0;
        for (size_t k = at + 1; k-- > j;)
        {
            way[n++] = reply.path[k];
        }
        routed_learn(way, n, now, false);
    }
    if (at == 0)
    {
        relay_route &route = routed_net.routes[reply.target];
        route.searching = false;
        route.tries = 0;
        return;
    }
    const uint8_t *mac = routed_mac(reply.path[at - 1]);
    if (mac)
    {
        routed_send(reply, mac);
    }
}

void routed_on_error(const relay_frame::route_message &error, uint8_t sender,
    uint32_t now)
{
    if (!valid_peer(error.target) && error.target != own_id)
    {
        return;
    }
    relay_route &route = routed_net.routes[error.target];
    if (route.valid && route.next_hop == sender)
    {
        route.valid = false;
        Serial.printf("ROUTED route to %u broken beyond Board %u: dropped\n",
            error.target, sender);
    }
    if (error.origin == own_id)
    {
        routed_discover(error.target, now);
        return;
    }
    const relay_route *back = routed_route(error.origin, now);
    const uint8_t *mac = back ? routed_mac(back->next_hop) : nullptr;
    if (mac)
    {
        routed_send(error, mac);
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
    case relay_frame::route_error:
        routed_on_error(message, sender, now);
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
        relay_route &route = routed_net.routes[target];
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
    const uint8_t *inner, size_t inner_size, uint32_t now)
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
        const relay_route *route = routed_route(destination, now);
        mac = route ? routed_mac(route->next_hop) : nullptr;
        if (!mac)
        {
            ++net.no_route;
            routed_discover(destination, now);
            return ESP_ERR_NOT_FOUND;
        }
    }
    return relay_transmit(net, mac, head, inner, inner_size, false);
}

esp_err_t relay_send_payload(via link, uint8_t peer, const uint8_t *payload,
    size_t size, uint8_t type)
{
    relay_net &net = relay_of(link);
    uint8_t inner[relay_frame::max_inner_size] = {};
    size_t inner_size = 0;
    uint32_t counter = 0;
    const status sealed = net.peers[peer].link.seal(e2e_keys::network, type,
        payload, size, inner, sizeof(inner), &inner_size, &counter);
    if (sealed != status::ok)
    {
        Serial.printf("%s SEAL_ERROR %s\n", net.tag,
            secure_message::status_name(sealed));
        return ESP_FAIL;
    }
    return relay_send_inner(net, peer, inner, inner_size, millis());
}

void relay_send_control(relay_net &net, uint8_t destination,
    const control_frame::message &content, uint32_t now)
{
    if (++net.control_sequence == 0)
    {
        net.control_sequence = 1;
    }
    const secure_message::header fields = {secure_message::version,
        control_frame::type_session, own_id, destination,
        net.control_sequence};
    uint8_t inner[relay_frame::max_inner_size];
    size_t size = 0;
    const status built = control_frame::build(e2e_keys::network, fields,
        content, inner, sizeof(inner), &size);
    if (built != status::ok)
    {
        Serial.printf("%s control frame not built: %s\n", net.tag,
            secure_message::status_name(built));
        return;
    }
    relay_send_inner(net, destination, inner, size, now);
}

// Sessions, as ESP-NOW's and the mesh's

void relay_start_session(relay_net &net, uint8_t peer, uint32_t now)
{
    uint8_t nonce[session::nonce_size];
    esp_fill_random(nonce, sizeof(nonce));
    control_frame::message hello = {};
    relay_peer &state = net.peers[peer];
    if (state.link.initiate(nonce, &hello))
    {
        relay_send_control(net, peer, hello, now);
        state.last_hello_ms = now;
        state.hellos = 1;
        Serial.printf("%s SESSION hello -> %u\n", net.tag, peer);
    }
}

void service_relay_sessions(relay_net &net, uint32_t now)
{
    for (uint8_t peer = 1; peer <= max_id; ++peer)
    {
        relay_peer &state = net.peers[peer];
        control_frame::message hello = {};
        const bool slow = state.hellos >= app_config::session_fast_hellos;
        const uint32_t period = slow ? app_config::session_slow_retry_ms :
            app_config::session_retry_ms;
        if (state.link.initiating() && now - state.last_hello_ms >= period &&
            state.link.hello(&hello))
        {
            relay_send_control(net, peer, hello, now);
            state.last_hello_ms = now;
            ++state.hellos;
            if (state.hellos == app_config::session_fast_hellos)
            {
                Serial.printf("%s SESSION %u not answering: retrying every "
                              "%lu s\n",
                    net.tag, peer, static_cast<unsigned long>(
                        app_config::session_slow_retry_ms / 1000));
            }
        }
    }
}

void relay_maybe_resync(relay_net &net, uint8_t peer, uint32_t now)
{
    relay_peer &state = net.peers[peer];
    if (!state.link.initiating() &&
        now - state.last_accept_ms >= app_config::session_resync_ms &&
        now - state.last_resync_ms >= app_config::session_resync_ms)
    {
        state.last_resync_ms = now;
        Serial.printf("%s SESSION resync with %u\n", net.tag, peer);
        relay_start_session(net, peer, now);
    }
}

void relay_on_session(relay_net &net, const radio::received_frame &frame,
    const secure_message::header &fields,
    const control_frame::message &content, uint32_t now)
{
    relay_peer &state = net.peers[fields.source];
    if (!state.link.paired())
    {
        relay_reject(net, frame, "no pair key for source");
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
        relay_send_control(net, fields.source, response, now);
        Serial.printf("%s SESSION reply -> %u\n", net.tag, fields.source);
        break;
    case session::event::established:
        relay_send_control(net, fields.source, response, now);
        Serial.printf("%s SESSION established with %u | session %08lX | "
                      "initiator\n",
            net.tag, fields.source,
            static_cast<unsigned long>(state.link.session_id()));
        state.last_accept_ms = now;
        break;
    case session::event::confirmed:
        Serial.printf("%s SESSION established with %u | session %08lX | "
                      "responder\n",
            net.tag, fields.source,
            static_cast<unsigned long>(state.link.session_id()));
        state.last_accept_ms = now;
        break;
    case session::event::bad_tag:
        relay_reject(net, frame, "bad handshake tag");
        break;
    case session::event::ignored:
        break;
    }
}

// Receive

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
            relay_on_session(net, frame, fields, content, now);
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
        relay_maybe_resync(net, fields.source, now);
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

// Passes a frame for another board on: the flood mesh broadcasts it again,
// the routed relay hands it to the next hop toward its destination.
void relay_forward(relay_net &net, relay_frame::envelope head,
    const secure_message::header &fields, uint32_t now)
{
    if (head.ttl <= 1 || head.path_length >= relay_frame::max_path)
    {
        ++net.hop_drops;
        return;
    }
    --head.ttl;
    head.path[head.path_length++] = own_id;
    const uint8_t *mac = radio::broadcast_mac;
    if (net.link == via::routed)
    {
        const relay_route *route = routed_route(fields.destination, now);
        mac = route ? routed_mac(route->next_hop) : nullptr;
        if (!mac)
        {
            ++net.no_route;
            routed_report(fields.source, fields.destination, now);
            return;
        }
    }
    relay_transmit(net, mac, head, head.inner, head.inner_size, true);
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
    // The path must start at the sender of the frame inside and end at the
    // board this copy came from; this board on it means a loop.
    if (head.path[0] != fields.source ||
        head.path[head.path_length - 1] != sender)
    {
        relay_reject(net, frame, "path doesn't match the frame");
        return true;
    }
    for (size_t i = 0; i < head.path_length; ++i)
    {
        if (head.path[i] == own_id)
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
        relay_forward(net, head, fields, now);
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
        const relay_route *route = routed_route(peer, now);
        if (route)
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

const session::link &session_via(via link, uint8_t peer)
{
    switch (link)
    {
    case via::mesh: return mesh_peers[peer].link;
    case via::flood:
    case via::routed: return relay_of(link).peers[peer].link;
    case via::espnow: break;
    }
    return peers[peer].link;
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
    if (!net.calls.dial(peer, id, now))
    {
        Serial.printf("%s LINK refused: already %s\n", net.tag,
            call::state_name(net.calls.current()));
        return false;
    }
    return true;
}

// Pings every board with a session over this transport and logs when one
// comes or goes.
void service_relay_presence(relay_net &net, uint32_t now)
{
    for (uint8_t peer = 1; peer <= max_id; ++peer)
    {
        if (!valid_peer(peer))
        {
            continue;
        }
        relay_peer &state = net.peers[peer];
        const uint32_t period = state.shown_online ?
            app_config::presence_ping_ms : app_config::presence_offline_ping_ms;
        if (state.link.established() &&
            now - state.last_presence_ms >= period)
        {
            state.last_presence_ms = now;
            const uint8_t payload[] = {kind_presence};
            relay_send_payload(net.link, peer, payload, sizeof(payload),
                secure_message::type_voice);  // Best effort.
        }
        const bool online = relay_peer_online(net.link, peer, now);
        if (online != state.shown_online)
        {
            state.shown_online = online;
            Serial.printf("%s PEER %u (%s) %s\n", net.tag, peer,
                device_name(peer), online ? "online" : "offline");
        }
    }
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
        const relay_route &route = routed_net.routes[target];
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
    Serial.printf("RELAY ESP-NOW multi-hop: flood mesh and routed relay | "
                  "up to %u hops | no router, no root\n",
        app_config::relay_max_hops);
    const uint32_t now = millis();
    for (uint8_t peer = 1; peer <= max_id; ++peer)
    {
        for (relay_net *net : relay_nets)
        {
            if (net->peers[peer].link.paired())
            {
                relay_start_session(*net, peer, now);
            }
        }
    }
}

void service_relay(uint32_t now)
{
    service_routed(now);
    for (relay_net *net : relay_nets)
    {
        service_relay_sessions(*net, now);
        service_relay_presence(*net, now);
        service_calls(net->link, now);
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
