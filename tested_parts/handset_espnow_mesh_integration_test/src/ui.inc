// Included inside main.cpp's anonymous namespace, after handset.inc.
//
// D-pad and ST7789 screen, from dpad_menu_test. Screens:
//
//   Menu            ESP-NOW, ESP-MESH (dot: in the mesh), Flood mesh,
//                   Routed relay, LoRa (dot: LoRa radio state), Volume,
//                   Status
//   ESP-NOW         "Connect <name>" per handset (dot: online), CANCEL back
//   ESP-MESH        "Connect <name>" per handset (dot: online over the
//                   mesh), CANCEL back
//   Flood mesh,     "Connect <name>" per handset (dot: online over that
//   Routed relay    way), CANCEL back
//   LoRa            "<name>" per handset (dot: LoRa session made), CANCEL
//                   back
//   Connecting      waiting for the other handset to accept; CANCEL stops
//   Request         who wants to connect, and over which radio; OK
//                   accepts, CANCEL declines
//   Connected       ESP-NOW, ESP-MESH, flood mesh or routed relay: the
//                   route, hold OK (or PTT) to talk, UP/DOWN volume, RIGHT
//                   says hello, CANCEL disconnects (both sides drop)
//   LoRa connected  UP sends "Hello from <this board>", DOWN sends "Bye
//                   from <this board>", CANCEL disconnects
//   Disconnected    why; any button or 3 s returns to the last list
//   Volume          UP/RIGHT louder, DOWN/LEFT quieter, OK mute/unmute,
//                   CANCEL back
//   Status          IDs, sessions, mesh, LoRa radio and counters, CANCEL
//                   back
//   Routes          this board's place in ESP-MESH, and each board's route
//                   over ESP-MESH, the flood mesh and the routed relay
//   Tools           the test settings, OK switches each (red dot: on):
//                   block a board's relayed frames (middleman test), ESP-NOW
//                   rate, leave/rejoin ESP-MESH, become mesh root, mute
//                   each way (link loss)
//
// Nothing needs the serial port: every function and test setting is on
// these screens (the serial commands do the same, for scripted tests).
//
// The connection state picks the screen: a request over any radio
// replaces whatever is shown. Only one connection runs at a time. Screens
// redraw fully only when they change (about 100 ms at 10 MHz); live values
// are redrawn in place. Only the loop calls this code.

enum class button_id : uint8_t
{
    up,
    down,
    left,
    right,
    confirm,
    cancel,
    ptt,
};

struct button
{
    const char *name;
    int pin;
    button_id id;
    bool raw_pressed;
    bool stable_pressed;
    uint32_t raw_changed_ms;
};

enum class screen : uint8_t
{
    none,
    menu,
    espnow_list,
    mesh_list,
    flood_list,
    routed_list,
    lora_list,
    tools,
    volume,
    status,
    routes,
    connecting,
    request,
    connected,
    lora_connected,
    ended,
};

// What the big box on the connected screen shows.
enum class talk_view : uint8_t
{
    idle,       // Hold OK to talk.
    talking,    // This board is sending.
    sending,    // PTT released; the last frames are still going out.
    listening,  // The other handset's voice is playing.
    notice,     // A refused push-to-talk, for ui_notice_ms.
};

constexpr uint16_t title_color = 0x03EF;      // Teal.
constexpr uint16_t highlight_color = 0xFD20;  // Orange.
constexpr uint16_t muted_color = 0x8410;      // Gray hints.
constexpr uint16_t outline_color = 0x4208;    // Dark gray.
constexpr uint16_t talk_color = 0xF800;       // Red.
constexpr uint16_t listen_color = 0x07E0;     // Green.
constexpr uint16_t idle_box_color = 0x2104;   // Very dark gray.
constexpr int16_t title_height = 32;
constexpr int16_t row_top = 44;
constexpr int16_t row_pitch = 40;
constexpr int16_t row_height = 34;
constexpr int16_t row_margin = 8;
constexpr uint8_t visible_rows = 4;
// The longest list: the Tools page, a block row per other handset (up to 4)
// and 7 more; the top menu has 9. Lists scroll.
constexpr uint8_t max_rows = max_id + 7 > 9 ? max_id + 7 : 9;
constexpr int16_t box_top = 84;
constexpr int16_t box_height = 64;
constexpr int16_t volume_top = 158;
constexpr int16_t heard_top = 182;

// The LCD takes whichever SPI bus LoRa does not (lora_radio.cpp): HSPI by
// default, so LoRa keeps the bus it worked on in lora_encrypt_test.
SPIClass lcd_hspi(HSPI);
SPIClass &lcd_spi = app_config::lora_on_hspi ? SPI : lcd_hspi;
Adafruit_ST7789 lcd(&lcd_spi, app_config::lcd_cs_pin,
    app_config::lcd_dc_pin, app_config::lcd_reset_pin);

button buttons[] = {
    {"UP", app_config::up_pin, button_id::up, false, false, 0},
    {"DOWN", app_config::down_pin, button_id::down, false, false, 0},
    {"LEFT", app_config::left_pin, button_id::left, false, false, 0},
    {"RIGHT", app_config::right_pin, button_id::right, false, false, 0},
    {"OK", app_config::confirm_pin, button_id::confirm, false, false, 0},
    {"CANCEL", app_config::cancel_pin, button_id::cancel, false, false, 0},
    {"PTT", app_config::ptt_pin, button_id::ptt, false, false, 0},
};
constexpr size_t button_count = sizeof(buttons) / sizeof(buttons[0]);

// Menu rows. The top menu holds the five ways to connect and four pages;
// each way's list holds the handsets; the Tools page holds the test
// settings, so nothing needs the serial port.
enum class row_kind : uint8_t
{
    espnow_page,
    mesh_page,
    flood_page,
    routed_page,
    lora_page,
    volume,
    status,
    routes_page,
    tools_page,
    // The five connect rows, together and in this order (row_dots()).
    connect,         // ESP-NOW connection to `peer`.
    mesh_connect,    // ESP-MESH connection to `peer`.
    flood_connect,   // Flood mesh connection to `peer`.
    routed_connect,  // Routed relay connection to `peer`.
    lora_connect,    // LoRa connection to `peer`.
    // Tools: OK switches each.
    tool_block,        // Ignore flood/routed frames heard from `peer`.
    tool_rate,         // ESP-NOW rate, 1 or 6 Mbit/s.
    tool_mesh_leave,   // Leave ESP-MESH (as if down), or rejoin.
    tool_mesh_root,    // Become an ESP-MESH root now.
    tool_mute_flood,   // Drop everything over the flood mesh.
    tool_mute_routed,  // ... the routed relay.
    tool_mute_mesh,    // ... ESP-MESH.
    tool_mute_lora,    // ... LoRa.
};

struct menu_row
{
    row_kind kind;
    uint8_t peer;
};

menu_row rows[max_rows];
uint8_t row_count = 0;
uint8_t selected = 0;
uint8_t first_visible = 0;  // Top row of the scrolled menu.
screen rows_for = screen::none;  // Which list `rows` holds.
// Each list's selection, kept while another list shows.
struct list_position
{
    uint8_t selected;
    uint8_t first_visible;
};
// Menu, ESP-NOW, ESP-MESH, flood mesh, routed relay, LoRa, Tools.
list_position positions[7] = {};

screen shown = screen::none;
// What shows when not connected: a list, volume or status.
screen page = screen::menu;
bool ui_ptt_held = false;    // A stream started from a button is running.
uint32_t ui_shown_ends[connection_count] = {};  // By connections[] entry.
uint32_t ui_end_shown_ms = 0;
bool ui_end_pending = false;
uint32_t ui_last_refresh_ms = 0;
char ui_notice_title[16] = {};
char ui_notice_text[20] = {};
uint32_t ui_notice_ms = 0;
bool ui_notice_on = false;
// The connect rows' dots, by way (row kind connect .. lora_connect) and
// board.
bool ui_dots[5][max_id + 1] = {};
lora_radio::state ui_drawn_lora = lora_radio::state::off;
mesh_radio::state ui_drawn_mesh = mesh_radio::state::off;
call::machine *ui_last_ended = &calls;  // The last connection to end.
char ui_lora_hint[40] = {};
char ui_mesh_hint[40] = {};
// Hello banner over the hint lines, on any screen.
char ui_toast_text[app_config::text_max_chars + 1] = {};
uint16_t ui_toast_color = 0;
uint32_t ui_toast_ms = 0;
bool ui_toast_on = false;
bool ui_toast_drawn = false;
uint32_t ui_text_seen = 0;  // text_in_count already shown.
const char *ui_hint_first = nullptr;  // Last hints drawn, to restore.
const char *ui_hint_second = nullptr;

// Last values drawn in place, so unchanged ones are not redrawn.
talk_view ui_drawn_view = talk_view::idle;
int ui_drawn_volume = -1;
bool ui_drawn_muted = false;
uint32_t ui_drawn_heard = UINT32_MAX;
uint32_t ui_drawn_second = UINT32_MAX;
uint32_t ui_drawn_elapsed = UINT32_MAX;
uint32_t ui_drawn_lora_in = UINT32_MAX;
uint32_t ui_drawn_lora_out = UINT32_MAX;
bool ui_drawn_lora_notice = false;
uint32_t ui_shown_delivery = 0;  // lora_delivery_count already shown.

// The connection the shared screens (connecting, request, disconnected)
// are about: the one in progress, else the one that ended last.
call::machine &ui_calls()
{
    if (lora_calls.current() != call::state::idle)
    {
        return lora_calls;
    }
    call::machine *wifi[] = {&mesh_calls, &flood_calls, &routed_calls,
        &calls};
    for (call::machine *machine : wifi)
    {
        if (machine->current() != call::state::idle)
        {
            return *machine;
        }
    }
    return *ui_last_ended;
}

bool ui_is_lora()
{
    return &ui_calls() == &lora_calls;
}

bool ui_is_espnow()
{
    return &ui_calls() == &calls;
}

// The Wi-Fi way the shown connection runs over (not for LoRa).
via ui_via()
{
    const call::machine &machine = ui_calls();
    for (via link : {via::mesh, via::flood, via::routed})
    {
        if (&machine == &way(link).calls)
        {
            return link;
        }
    }
    return via::espnow;
}

const char *ui_radio_label()
{
    return ui_is_lora() ? "LoRa" : way(ui_via()).label;
}

const char *ui_radio_name()
{
    return ui_is_lora() ? "over LoRa" : way(ui_via()).over;
}

bool active_low(const button &b)
{
    return app_config::cancel_active_low && b.id == button_id::cancel;
}

bool read_pressed(const button &b)
{
    return digitalRead(b.pin) == (active_low(b) ? LOW : HIGH);
}

// Built-in font: 6 x 8 pixels per size step, including spacing.
int16_t text_width(const char *text, uint8_t size)
{
    return static_cast<int16_t>(strlen(text)) * 6 * size - size;
}

void draw_text(const char *text, int16_t x, int16_t top, uint8_t size,
    uint16_t color)
{
    lcd.setTextSize(size);
    lcd.setTextColor(color);
    lcd.setCursor(x, top);
    lcd.print(text);
}

void draw_centered(const char *text, int16_t top, uint8_t size,
    uint16_t color)
{
    draw_text(text, (lcd.width() - text_width(text, size)) / 2, top, size,
        color);
}

// Text over a cleared strip, so a value can be rewritten in place.
void draw_field(const char *text, int16_t x, int16_t top, uint8_t size,
    uint16_t color, int16_t width)
{
    lcd.fillRect(x, top, width, 8 * size, ST77XX_BLACK);
    draw_text(text, x, top, size, color);
}

// Title bar with this board's own name on the right, on every screen.
void draw_title(const char *title, uint16_t color)
{
    lcd.fillRect(0, 0, lcd.width(), title_height, color);
    draw_text(title, 8, 9, 2, ST77XX_WHITE);
    const char *own_name = device_name(own_id);
    draw_text(own_name, lcd.width() - 6 - text_width(own_name, 1), 3, 1,
        ST77XX_WHITE);
}

void draw_hints(const char *first, const char *second)
{
    ui_hint_first = first;
    ui_hint_second = second;
    if (first)
    {
        draw_text(first, 8, 212, 1, muted_color);
    }
    if (second)
    {
        draw_text(second, 8, 226, 1, muted_color);
    }
}

void draw_peer_name(int16_t top)
{
    draw_centered(device_name(ui_calls().peer()), top, 3, highlight_color);
}

// "Label mm:ss" since the connection state was entered.
void draw_elapsed(const char *label, int16_t x, int16_t top, uint8_t size,
    uint32_t now)
{
    const uint32_t seconds = (now - ui_calls().since_ms()) / 1000;
    ui_drawn_elapsed = seconds;
    char line[24];
    snprintf(line, sizeof(line), "%s %02lu:%02lu", label,
        static_cast<unsigned long>(seconds / 60),
        static_cast<unsigned long>(seconds % 60));
    draw_field(line, x, top, size, ST77XX_WHITE, lcd.width() - 8 - x);
}

void notice(const char *title, const char *text, uint32_t now)
{
    snprintf(ui_notice_title, sizeof(ui_notice_title), "%s", title);
    snprintf(ui_notice_text, sizeof(ui_notice_text), "%s", text);
    ui_notice_on = true;
    ui_notice_ms = now;
}

bool notice_showing(uint32_t now)
{
    if (ui_notice_on && now - ui_notice_ms >= app_config::ui_notice_ms)
    {
        ui_notice_on = false;
    }
    return ui_notice_on;
}

void refused_beep()
{
    if (app_config::refused_tone_ms > 0 && rx_play_at_us.load() == 0)
    {
        speaker::tone(app_config::refused_tone_hz,
            app_config::refused_tone_ms);
    }
}

// Menu

void row_label(const menu_row &row, char *label, size_t capacity)
{
    switch (row.kind)
    {
    case row_kind::espnow_page:
        snprintf(label, capacity, "%s", way(via::espnow).label);
        break;
    case row_kind::mesh_page:
        snprintf(label, capacity, "%s", way(via::mesh).label);
        break;
    case row_kind::flood_page:
        snprintf(label, capacity, "%s", way(via::flood).label);
        break;
    case row_kind::routed_page:
        snprintf(label, capacity, "%s", way(via::routed).label);
        break;
    case row_kind::lora_page:
        snprintf(label, capacity, "LoRa");
        break;
    case row_kind::connect:
    case row_kind::mesh_connect:
    case row_kind::flood_connect:
    case row_kind::routed_connect:
        snprintf(label, capacity, "Connect %s", device_name(row.peer));
        break;
    case row_kind::lora_connect:
        snprintf(label, capacity, "%s", device_name(row.peer));
        break;
    case row_kind::volume:
        snprintf(label, capacity, "Volume");
        break;
    case row_kind::status:
        snprintf(label, capacity, "Status");
        break;
    case row_kind::routes_page:
        snprintf(label, capacity, "Routes");
        break;
    case row_kind::tools_page:
        snprintf(label, capacity, "Tools");
        break;
    case row_kind::tool_block:
        snprintf(label, capacity, "Block %s", device_name(row.peer));
        break;
    case row_kind::tool_rate:
        snprintf(label, capacity, "Rate: %u Mbps", radio::rate_mbps());
        break;
    case row_kind::tool_mesh_leave:
        snprintf(label, capacity, mesh_switched_off ? "Rejoin mesh" :
            "Leave mesh");
        break;
    case row_kind::tool_mesh_root:
        snprintf(label, capacity, "Be mesh root");
        break;
    case row_kind::tool_mute_flood:
        snprintf(label, capacity, "Mute flood");
        break;
    case row_kind::tool_mute_routed:
        snprintf(label, capacity, "Mute routed");
        break;
    case row_kind::tool_mute_mesh:
        snprintf(label, capacity, "Mute mesh");
        break;
    case row_kind::tool_mute_lora:
        snprintf(label, capacity, "Mute LoRa");
        break;
    }
}

// A Tools row's dot: red while its test setting is on (a block, a mute,
// out of the mesh), green for "Be mesh root" while this board is the
// root. False: no dot (the rate shows in the label).
bool tool_dot(const menu_row &row, uint16_t *color)
{
    bool on = false;
    switch (row.kind)
    {
    case row_kind::tool_block: on = relay_is_blocked(row.peer); break;
    case row_kind::tool_mesh_leave: on = mesh_switched_off; break;
    case row_kind::tool_mute_flood: on = flood_net.muted; break;
    case row_kind::tool_mute_routed: on = routed_net.muted; break;
    case row_kind::tool_mute_mesh: on = mesh_muted; break;
    case row_kind::tool_mute_lora: on = lora_muted; break;
    case row_kind::tool_mesh_root:
        *color = mesh_radio::is_root() ? listen_color : outline_color;
        return true;
    default:
        return false;
    }
    *color = on ? talk_color : outline_color;
    return true;
}

// Everything the Tools rows show, so they are redrawn when any changes
// (also from serial).
uint32_t tools_state()
{
    return relay_blocked ^ radio::rate_mbps() << 16 ^
        static_cast<uint32_t>(flood_net.muted) << 24 ^
        static_cast<uint32_t>(routed_net.muted) << 25 ^
        static_cast<uint32_t>(mesh_muted) << 26 ^
        static_cast<uint32_t>(lora_muted) << 27 ^
        static_cast<uint32_t>(mesh_switched_off) << 28 ^
        static_cast<uint32_t>(mesh_radio::is_root()) << 29;
}
uint32_t ui_drawn_tools = 0;

// A connect row's dots (by board), or null for any other row.
bool *row_dots(row_kind kind)
{
    const int way = static_cast<int>(kind) -
        static_cast<int>(row_kind::connect);
    return way >= 0 && way < 5 ? ui_dots[way] : nullptr;
}

void draw_menu_row(uint8_t index)
{
    if (index < first_visible || index >= first_visible + visible_rows)
    {
        return;  // Scrolled out of view.
    }
    const menu_row &row = rows[index];
    const int16_t y = row_top + (index - first_visible) * row_pitch;
    const int16_t width = lcd.width() - 2 * row_margin - 6;
    const bool highlighted = index == selected;
    lcd.fillRoundRect(row_margin, y, width, row_height, 6,
        highlighted ? highlight_color : ST77XX_BLACK);
    if (!highlighted)
    {
        lcd.drawRoundRect(row_margin, y, width, row_height, 6, outline_color);
    }
    char label[24];
    row_label(row, label, sizeof(label));
    draw_text(label, 20, y + 10, 2,
        highlighted ? ST77XX_BLACK : ST77XX_WHITE);
    uint16_t color = outline_color;
    bool dot = true;
    const bool *dots = row_dots(row.kind);
    if (dots)
    {
        // Green: the board can be asked over this way (row_up()).
        color = dots[row.peer] ? listen_color : outline_color;
    }
    else if (row.kind >= row_kind::tool_block)
    {
        dot = tool_dot(row, &color);
    }
    else if (row.kind == row_kind::mesh_page)
    {
        // This board in the mesh: green in, red failed, grey joining.
        const mesh_radio::state mesh = mesh_radio::current();
        color = mesh == mesh_radio::state::connected ? listen_color :
            mesh == mesh_radio::state::failed ? talk_color : outline_color;
    }
    else if (row.kind == row_kind::lora_page)
    {
        // The LoRa radio: green ok, red failed, grey starting.
        const lora_radio::state radio = lora_radio::current();
        color = radio == lora_radio::state::ok ? listen_color :
            radio == lora_radio::state::failed ? talk_color : outline_color;
    }
    else
    {
        dot = false;
    }
    if (dot)
    {
        lcd.fillCircle(row_margin + width - 16, y + row_height / 2, 6, color);
    }
}

bool lora_row_ready(uint8_t peer)
{
    return lora_ready() && lora_can_send(peer);
}

bool mesh_row_ready(uint8_t peer, uint32_t now)
{
    return mesh_ready() && mesh_peer_online(peer, now);
}

// A connect row's dot is green: ESP-NOW, the mesh and the relay ways when
// the board is online over it (its presence pings arrive), LoRa once a
// LoRa session with it is made.
bool row_up(const menu_row &row, uint32_t now)
{
    switch (row.kind)
    {
    case row_kind::connect: return peer_online(row.peer, now);
    case row_kind::mesh_connect: return mesh_row_ready(row.peer, now);
    case row_kind::flood_connect:
        return relay_peer_online(via::flood, row.peer, now);
    case row_kind::routed_connect:
        return relay_peer_online(via::routed, row.peer, now);
    case row_kind::lora_connect: return lora_row_ready(row.peer);
    default: return false;
    }
}

// While looking for a mesh: "Looking; root in N s" (this board takes over
// then), or just "Looking for the mesh" while it waits for voice to end.
void mesh_looking_text(char *text, size_t capacity, const char *suffix)
{
    const uint32_t left = mesh_takeover_in_ms(millis());
    if (left > 0)
    {
        snprintf(text, capacity, "Looking; root in %lu s%s",
            static_cast<unsigned long>((left + 999) / 1000), suffix);
    }
    else
    {
        snprintf(text, capacity, "Looking for the mesh%s", suffix);
    }
}

// The mesh list's second hint line: where this board is in the mesh, or
// why it isn't.
const char *mesh_hint()
{
    if (mesh_switched_off)
    {
        snprintf(ui_mesh_hint, sizeof(ui_mesh_hint),
            "Mesh off (Tools: Rejoin mesh)");
        return ui_mesh_hint;
    }
    switch (mesh_radio::current())
    {
    case mesh_radio::state::connected:
        if (mesh_radio::is_root())
        {
            snprintf(ui_mesh_hint, sizeof(ui_mesh_hint),
                "Root, %d board%s  CANCEL: back", mesh_radio::nodes(),
                mesh_radio::nodes() == 1 ? "" : "s");
        }
        else if (mesh_looking)
        {
            // Joined, but the root went away.
            mesh_looking_text(ui_mesh_hint, sizeof(ui_mesh_hint),
                "  CANCEL: back");
        }
        else if (mesh_radio::root_board() != 0)
        {
            snprintf(ui_mesh_hint, sizeof(ui_mesh_hint),
                "Layer %d, root Board %u  CANCEL: back", mesh_radio::layer(),
                mesh_radio::root_board());
        }
        else
        {
            snprintf(ui_mesh_hint, sizeof(ui_mesh_hint),
                "In mesh, layer %d  CANCEL: back", mesh_radio::layer());
        }
        break;
    case mesh_radio::state::failed:
        snprintf(ui_mesh_hint, sizeof(ui_mesh_hint), "Mesh ERR at %s%s",
            mesh_radio::failed_step(),
            mesh_radio::can_restart() ? ", retrying" : "");
        break;
    case mesh_radio::state::joining:
    case mesh_radio::state::off:
        mesh_looking_text(ui_mesh_hint, sizeof(ui_mesh_hint),
            "  CANCEL: back");
        break;
    }
    return ui_mesh_hint;
}

// The LoRa list's second hint line: what the dots mean, or why LoRa is
// not available.
const char *lora_hint()
{
    switch (lora_radio::current())
    {
    case lora_radio::state::ok:
        snprintf(ui_lora_hint, sizeof(ui_lora_hint),
            "Green: answered over LoRa");
        break;
    case lora_radio::state::failed:
        snprintf(ui_lora_hint, sizeof(ui_lora_hint), "LoRa ERR %d %s%s",
            lora_radio::last_error(), lora_radio::failed_step(),
            lora_radio::busy_stuck() ? " BUSY" : "");
        break;
    case lora_radio::state::starting:
    case lora_radio::state::off:
        snprintf(ui_lora_hint, sizeof(ui_lora_hint), "LoRa radio starting");
        break;
    }
    return ui_lora_hint;
}

bool toast_showing(uint32_t now)
{
    if (ui_toast_on && static_cast<int32_t>(now - ui_toast_ms) >=
            static_cast<int32_t>(app_config::text_show_ms))
    {
        ui_toast_on = false;
    }
    return ui_toast_on;
}

void toast(const char *text, uint16_t color, uint32_t now)
{
    snprintf(ui_toast_text, sizeof(ui_toast_text), "%s", text);
    ui_toast_color = color;
    ui_toast_ms = now;
    ui_toast_on = true;
    ui_toast_drawn = false;  // Drawn (again) by service_toast().
}

// The menu's first hint line, replaced by a notice ("Board 2 not
// reachable") while one shows. Not drawn under the hello banner.
void draw_menu_notice(uint32_t now)
{
    if (toast_showing(now))
    {
        return;
    }
    if (notice_showing(now))
    {
        char line[40];
        snprintf(line, sizeof(line), "%s %s", ui_notice_title,
            ui_notice_text);
        draw_field(line, 8, 212, 1, highlight_color, 224);
    }
    else
    {
        draw_field(shown == screen::menu ? "UP/DOWN: choose   OK: select" :
                shown == screen::tools ? "UP/DOWN: choose  OK: switch" :
                "UP/DOWN: choose  OK: connect", 8, 212, 1, muted_color, 224);
    }
}

// Scroll bar on the right edge when the menu has more rows than fit.
void draw_scroll_bar()
{
    const int16_t top = row_top;
    const int16_t height = visible_rows * row_pitch - (row_pitch - row_height);
    lcd.fillRect(lcd.width() - 6, top, 3, height, ST77XX_BLACK);
    if (row_count <= visible_rows)
    {
        return;
    }
    lcd.fillRect(lcd.width() - 5, top, 1, height, outline_color);
    const int16_t thumb = height * visible_rows / row_count;
    const int16_t offset = (height - thumb) * first_visible /
        (row_count - visible_rows);
    lcd.fillRect(lcd.width() - 6, top + offset, 3, thumb, highlight_color);
}

void draw_menu_rows()
{
    lcd.fillRect(0, row_top, lcd.width(),
        visible_rows * row_pitch - (row_pitch - row_height), ST77XX_BLACK);
    for (uint8_t i = 0; i < row_count; ++i)
    {
        draw_menu_row(i);
    }
    draw_scroll_bar();
}

int list_index(screen list)
{
    switch (list)
    {
    case screen::espnow_list: return 1;
    case screen::mesh_list: return 2;
    case screen::flood_list: return 3;
    case screen::routed_list: return 4;
    case screen::lora_list: return 5;
    case screen::tools: return 6;
    default: break;
    }
    return 0;
}

bool is_list(screen value)
{
    return value == screen::menu || list_index(value) != 0;
}

// Fills `rows` for a list, keeping each list's own selection.
void load_rows(screen list)
{
    if (rows_for == list)
    {
        return;
    }
    if (rows_for != screen::none)
    {
        positions[list_index(rows_for)] = {selected, first_visible};
    }
    rows_for = list;
    row_count = 0;
    if (list == screen::menu)
    {
        rows[row_count++] = {row_kind::espnow_page, 0};
        rows[row_count++] = {row_kind::mesh_page, 0};
        rows[row_count++] = {row_kind::flood_page, 0};
        rows[row_count++] = {row_kind::routed_page, 0};
        rows[row_count++] = {row_kind::lora_page, 0};
        rows[row_count++] = {row_kind::volume, 0};
        rows[row_count++] = {row_kind::status, 0};
        rows[row_count++] = {row_kind::routes_page, 0};
        rows[row_count++] = {row_kind::tools_page, 0};
    }
    else if (list == screen::tools)
    {
        for (uint8_t id = 1; id <= max_id && row_count < max_rows; ++id)
        {
            if (valid_peer(id))
            {
                rows[row_count++] = {row_kind::tool_block, id};
            }
        }
        const row_kind tools[] = {row_kind::tool_rate,
            row_kind::tool_mesh_leave, row_kind::tool_mesh_root,
            row_kind::tool_mute_flood, row_kind::tool_mute_routed,
            row_kind::tool_mute_mesh, row_kind::tool_mute_lora};
        for (row_kind kind : tools)
        {
            if (row_count < max_rows)
            {
                rows[row_count++] = {kind, 0};
            }
        }
    }
    else
    {
        row_kind kind = row_kind::connect;
        switch (list)
        {
        case screen::mesh_list: kind = row_kind::mesh_connect; break;
        case screen::flood_list: kind = row_kind::flood_connect; break;
        case screen::routed_list: kind = row_kind::routed_connect; break;
        case screen::lora_list: kind = row_kind::lora_connect; break;
        default: break;
        }
        for (uint8_t id = 1; id <= max_id && row_count < max_rows; ++id)
        {
            if (valid_peer(id))
            {
                rows[row_count++] = {kind, id};
            }
        }
    }
    const list_position saved = positions[list_index(list)];
    selected = saved.selected < row_count ? saved.selected : 0;
    first_visible = saved.first_visible <= selected ? saved.first_visible :
        selected;
    if (selected >= first_visible + visible_rows)
    {
        first_visible = static_cast<uint8_t>(selected - visible_rows + 1);
    }
}

const char *list_title(screen list)
{
    switch (list)
    {
    case screen::espnow_list: return way(via::espnow).label;
    case screen::tools: return "Tools";
    case screen::flood_list: return way(via::flood).label;
    case screen::routed_list: return way(via::routed).label;
    case screen::mesh_list: return way(via::mesh).label;
    case screen::lora_list: return "LoRa";
    default: return "Menu";
    }
}

// The list's second hint line.
const char *list_hint(screen list)
{
    switch (list)
    {
    case screen::espnow_list: return "Green: online   CANCEL: back";
    case screen::tools: return "Red: test setting on  CANCEL: back";
    case screen::flood_list: return "Every board relays  CANCEL: back";
    case screen::routed_list: return "Route found first  CANCEL: back";
    case screen::mesh_list: return mesh_hint();
    case screen::lora_list: return lora_hint();
    default: return "Dots: green ok, red failed";
    }
}

void draw_menu(screen list, uint32_t now)
{
    load_rows(list);
    // The dots are only kept current while the list shows; catch up first
    // so the first frame isn't stale (a board that went offline meanwhile
    // would show green until the next refresh).
    for (uint8_t i = 0; i < row_count; ++i)
    {
        bool *dots = row_dots(rows[i].kind);
        if (dots)
        {
            dots[rows[i].peer] = row_up(rows[i], now);
        }
    }
    ui_drawn_lora = lora_radio::current();
    ui_drawn_mesh = mesh_radio::current();
    if (list == screen::tools)
    {
        ui_drawn_tools = tools_state();
    }
    draw_title(list_title(list), title_color);
    draw_menu_rows();
    draw_menu_notice(now);
    draw_hints(nullptr, list_hint(list));
}

// Volume and status pages

void draw_volume_page_value()
{
    ui_drawn_volume = speaker::volume();
    ui_drawn_muted = speaker::muted();
    char value[8];
    snprintf(value, sizeof(value), "%d", ui_drawn_volume);
    lcd.fillRect(0, 70, lcd.width(), 48, ST77XX_BLACK);
    draw_centered(value, 70, 6, highlight_color);
    const int16_t width = lcd.width() - 40;
    const int16_t filled = static_cast<int16_t>(
        width * ui_drawn_volume / audio_dsp::speaker_guard::max_volume);
    lcd.fillRect(20, 140, width, 16, outline_color);
    lcd.fillRect(20, 140, filled, 16, highlight_color);
    draw_field(ui_drawn_muted ? "Muted (OK: unmute)" : "", 20, 170, 1,
        talk_color, 200);
}

void draw_volume_page()
{
    draw_title("Volume", title_color);
    draw_text("1..32", 8, 44, 1, muted_color);
    draw_volume_page_value();
    draw_hints("UP/RIGHT: louder  DOWN/LEFT: quieter",
        "OK: mute/unmute  CANCEL: back");
}

// Adds " N ok" or " N -" for each other board to `line`, whose first
// `used` characters are written; returns how many are now.
int add_board_marks(char *line, size_t capacity, int used,
    bool (*ok)(uint8_t id))
{
    for (uint8_t id = 1; id <= max_id && used > 0 &&
         used < static_cast<int>(capacity); ++id)
    {
        if (valid_peer(id))
        {
            used += snprintf(line + used, capacity - used, " %u %s", id,
                ok(id) ? "ok" : "-");
        }
    }
    return used;
}

// Status and Routes pages: lines that scroll with UP/DOWN when there are
// more than fit (with four other boards there are).
struct page_line
{
    char text[40];
    uint16_t color;
    bool gap;  // A little space above it: a new group.
};
constexpr size_t max_page_lines = 32;
constexpr int16_t page_gap = 6;
page_line page_lines[max_page_lines];
size_t page_line_count = 0;
size_t page_scroll = 0;      // First line shown.
size_t page_scroll_max = 0;  // Scrolled this far, the last line shows.
size_t page_shown = 0;       // Lines on screen now.
uint32_t page_drawn_layout = UINT32_MAX;  // To clear only when lines move.

__attribute__((format(printf, 3, 4)))
void page_add(uint16_t color, bool gap, const char *format, ...)
{
    if (page_line_count == max_page_lines)
    {
        return;
    }
    page_line &line = page_lines[page_line_count++];
    line.color = color;
    line.gap = gap;
    va_list args;
    va_start(args, format);
    vsnprintf(line.text, sizeof(line.text), format, args);
    va_end(args);
}

// "Label 2 ok 3 -": each other board, ok or not.
void page_add_marks(const char *label, bool gap, bool (*ok)(uint8_t id))
{
    char line[40];
    add_board_marks(line, sizeof(line),
        snprintf(line, sizeof(line), "%s", label), ok);
    page_add(ST77XX_WHITE, gap, "%s", line);
}

// How many lines from `first` fit between `top` and `bottom`, `pitch`
// apart, plus their gaps.
size_t page_fits(size_t first, int16_t top, int16_t bottom, int16_t pitch)
{
    int16_t y = top;
    size_t count = 0;
    for (size_t i = first; i < page_line_count; ++i)
    {
        y += i > first && page_lines[i].gap ? page_gap : 0;
        if (y + 8 > bottom)
        {
            break;
        }
        ++count;
        y += pitch;
    }
    return count;
}

// Draws the lines from page_scroll that fit, and a scroll bar on the right
// edge when not all do. The area is cleared only when the lines move, so
// the once-a-second redraw doesn't flicker.
void draw_page_lines(int16_t top, int16_t bottom, int16_t pitch)
{
    page_scroll_max = 0;
    while (page_scroll_max < page_line_count &&
        page_scroll_max + page_fits(page_scroll_max, top, bottom, pitch) <
            page_line_count)
    {
        ++page_scroll_max;
    }
    if (page_scroll > page_scroll_max)
    {
        page_scroll = page_scroll_max;
    }
    page_shown = page_fits(page_scroll, top, bottom, pitch);
    uint32_t layout = page_scroll << 16 ^ page_line_count << 8;
    for (size_t i = 0; i < page_shown; ++i)
    {
        layout ^= static_cast<uint32_t>(page_lines[page_scroll + i].gap) <<
            (i % 8);
    }
    if (layout != page_drawn_layout)
    {
        page_drawn_layout = layout;
        lcd.fillRect(0, top, lcd.width(), bottom - top, ST77XX_BLACK);
    }
    int16_t y = top;
    for (size_t i = page_scroll; i < page_scroll + page_shown; ++i)
    {
        y += i > page_scroll && page_lines[i].gap ? page_gap : 0;
        draw_field(page_lines[i].text, 8, y, 1, page_lines[i].color, 218);
        y += pitch;
    }
    if (page_scroll_max > 0)
    {
        const int16_t height = bottom - top;
        const int16_t thumb = height * page_shown / page_line_count;
        const int16_t offset = (height - thumb) * page_scroll /
            page_scroll_max;
        lcd.fillRect(lcd.width() - 5, top, 1, height, outline_color);
        lcd.fillRect(lcd.width() - 6, top + offset, 3, thumb,
            highlight_color);
    }
}

// UP/DOWN on the Status or Routes page: half a page each press.
void scroll_page(int direction, uint32_t now);

constexpr int16_t status_top = 44;
constexpr int16_t status_bottom = 204;

void draw_status_values()
{
    page_line_count = 0;
    const network_config::device *self = radio::find_device(own_id);
    page_add(ST77XX_WHITE, false, "ID %u  %02X:%02X:%02X:%02X:%02X:%02X",
        own_id, self->mac[0], self->mac[1], self->mac[2], self->mac[3],
        self->mac[4], self->mac[5]);
    page_add(ST77XX_WHITE, false, "Channel %u, %u Mbps, buffer %lu ms",
        network_config::channel, radio::rate_mbps(),
        static_cast<unsigned long>(playout_ms));
    bool first = true;
    for (uint8_t id = 1; id <= max_id; ++id)
    {
        if (!valid_peer(id))
        {
            continue;
        }
        if (peer_online(id, millis()))
        {
            page_add(listen_color, first, "%s: online %08lX",
                device_name(id),
                static_cast<unsigned long>(peers[id].link.session_id()));
        }
        else
        {
            page_add(muted_color, first, "%s: offline", device_name(id));
        }
        first = false;
    }
    // ESP-MESH: where this board is in it, then who is online over it.
    char line[40];
    const mesh_radio::state mesh = mesh_radio::current();
    if (mesh == mesh_radio::state::failed)
    {
        // The error's name is in the serial log (`mesh`); too long here.
        snprintf(line, sizeof(line), "Mesh ERR at %s",
            mesh_radio::failed_step());
    }
    else if (mesh_switched_off)
    {
        snprintf(line, sizeof(line), "Mesh off (Tools: Rejoin mesh)");
    }
    else if (mesh == mesh_radio::state::connected && mesh_radio::is_root())
    {
        snprintf(line, sizeof(line), "Mesh root, %d board%s, no router",
            mesh_radio::nodes(), mesh_radio::nodes() == 1 ? "" : "s");
    }
    else if (mesh == mesh_radio::state::connected && !mesh_looking)
    {
        snprintf(line, sizeof(line), "Mesh layer %d, root Board %u",
            mesh_radio::layer(), mesh_radio::root_board());
    }
    else
    {
        mesh_looking_text(line, sizeof(line), "");
    }
    page_add(mesh == mesh_radio::state::connected ? listen_color :
        mesh == mesh_radio::state::failed ? talk_color : muted_color, true,
        "%s", line);
    page_add_marks("Mesh online:", false,
        [](uint8_t id) { return mesh_row_ready(id, millis()); });
    // LoRa: the radio, each handset's LoRa session, and the counts.
    const lora_radio::state radio = lora_radio::current();
    if (radio == lora_radio::state::failed)
    {
        page_add(talk_color, true, "LoRa ERR %d %s%s",
            lora_radio::last_error(), lora_radio::failed_step(),
            lora_radio::busy_stuck() ? " BUSY high" : "");
    }
    else
    {
        page_add(radio == lora_radio::state::ok ? listen_color : muted_color,
            true, "LoRa %s %.1f MHz SF%u", lora_radio::state_name(radio),
            app_config::lora_mhz, app_config::lora_spreading_factor);
    }
    page_add_marks("LoRa keys:", false, lora_can_send);
    const lora_radio::counters c = lora_radio::stats();
    page_add(ST77XX_WHITE, false, "LoRa sent %lu  heard %lu",
        static_cast<unsigned long>(c.sent),
        static_cast<unsigned long>(c.received));
    // Flood mesh and routed relay: who is online over each.
    page_add_marks("Flood:", true, [](uint8_t id) {
        return relay_peer_online(via::flood, id, millis());
    });
    page_add_marks("Routed:", false, [](uint8_t id) {
        return relay_peer_online(via::routed, id, millis());
    });
    page_add(ST77XX_WHITE, true, "Frames ok %lu  rejected %lu",
        static_cast<unsigned long>(accepted_count),
        static_cast<unsigned long>(rejected_count));
    page_add(ST77XX_WHITE, false, "Sent %lu  played %lu  lost %lu",
        static_cast<unsigned long>(total_tx_frames),
        static_cast<unsigned long>(total_rx_played),
        static_cast<unsigned long>(total_rx_lost));
    page_add(ST77XX_WHITE, false, "Uptime %lu s",
        static_cast<unsigned long>(millis() / 1000));
    draw_page_lines(status_top, status_bottom, 14);
}

void draw_status_page()
{
    draw_title("Status", title_color);
    draw_status_values();
    draw_hints(page_scroll_max ? "UP/DOWN: more lines" : nullptr,
        "CANCEL: back");
}

// Routes page: where this board is in ESP-MESH, then for each other board
// the route over ESP-MESH, the flood mesh and the routed relay (what
// `mesh`, `flood`, `routed` and `routes` print on serial).

// Orange through a middleman, green direct, grey while not known.
uint16_t route_color(const char *route)
{
    return strstr(route, "via") ? highlight_color :
        strstr(route, "direct") ? listen_color : muted_color;
}

constexpr int16_t routes_top = 44;
constexpr int16_t routes_bottom = 196;

void draw_routes_values(uint32_t now)
{
    page_line_count = 0;
    const mesh_radio::state mesh = mesh_radio::current();
    if (mesh_switched_off)
    {
        page_add(talk_color, false, "Mesh: off (Tools: Rejoin mesh)");
    }
    else if (mesh == mesh_radio::state::failed)
    {
        page_add(talk_color, false, "Mesh: ERR at %s",
            mesh_radio::failed_step());
    }
    else if (mesh == mesh_radio::state::connected && mesh_radio::is_root())
    {
        page_add(listen_color, false, "Mesh: root, %d board%s",
            mesh_radio::nodes(), mesh_radio::nodes() == 1 ? "" : "s");
    }
    else if (mesh == mesh_radio::state::connected)
    {
        page_add(listen_color, false, "Mesh: layer %d, parent Board %u",
            mesh_radio::layer(), mesh_radio::parent_board());
    }
    else
    {
        char looking[40];
        mesh_looking_text(looking, sizeof(looking), "");
        page_add(muted_color, false, "Mesh: %s", looking);
    }
    char line[40];
    int used = snprintf(line, sizeof(line), "Mesh children:");
    const uint32_t children = mesh_radio::children_boards();
    for (uint8_t id = 1; id <= max_id && used > 0 &&
         used < static_cast<int>(sizeof(line)); ++id)
    {
        if (children & (1u << id))
        {
            used += snprintf(line + used, sizeof(line) - used, " %u", id);
        }
    }
    page_add(ST77XX_WHITE, false, "%s%s", line, children ? "" : " none");
    // Each other board: a heading, then a route per way.
    for (uint8_t peer = 1; peer <= max_id; ++peer)
    {
        if (!valid_peer(peer))
        {
            continue;
        }
        page_add(highlight_color, true, "%s", device_name(peer));
        static const char *const names[] = {" Mesh   ", " Flood  ",
            " Routed "};
        const via links[] = {via::mesh, via::flood, via::routed};
        for (size_t i = 0; i < 3; ++i)
        {
            const via link = links[i];
            const bool online = link == via::mesh ?
                mesh_row_ready(peer, now) : relay_peer_online(link, peer, now);
            char route[32];
            if (online)
            {
                route_text_via(link, peer, now, route, sizeof(route));
            }
            else
            {
                snprintf(route, sizeof(route), "offline");
            }
            page_add(route_color(route), false, "%s%s", names[i], route);
        }
    }
    draw_page_lines(routes_top, routes_bottom, 12);
    used = snprintf(line, sizeof(line), "Blocked:");
    bool any = false;
    for (uint8_t id = 1; id <= max_id && used > 0 &&
         used < static_cast<int>(sizeof(line)); ++id)
    {
        if (relay_is_blocked(id))
        {
            used += snprintf(line + used, sizeof(line) - used, " %u", id);
            any = true;
        }
    }
    if (used > 0 && used < static_cast<int>(sizeof(line)))
    {
        snprintf(line + used, sizeof(line) - used, "%s  Rate %u Mbps",
            any ? "" : " none", radio::rate_mbps());
    }
    draw_field(line, 8, 198, 1, any ? talk_color : muted_color, 224);
}

void draw_routes_page(uint32_t now)
{
    draw_title("Routes", title_color);
    draw_routes_values(now);
    draw_hints("Orange: via a middleman",
        page_scroll_max ? "UP/DOWN: more   CANCEL: back" : "CANCEL: back");
}

void scroll_page(int direction, uint32_t now)
{
    const size_t step = page_shown > 2 ? page_shown / 2 : 1;
    if (direction > 0)
    {
        page_scroll = page_scroll + step < page_scroll_max ?
            page_scroll + step : page_scroll_max;
    }
    else
    {
        page_scroll = page_scroll > step ? page_scroll - step : 0;
    }
    if (shown == screen::status)
    {
        draw_status_values();
    }
    else
    {
        draw_routes_values(now);
    }
}

// Connecting and request

void draw_connecting(uint32_t now)
{
    draw_title("Connecting", title_color);
    draw_centered("Asking", 50, 2, ST77XX_WHITE);
    draw_peer_name(74);
    draw_centered(ui_radio_name(), 106, 2, listen_color);
    draw_elapsed("Waiting", 8, 140, 2, now);
    draw_hints(nullptr, "CANCEL: stop");
}

void draw_request()
{
    draw_title("Request", highlight_color);
    draw_peer_name(44);
    draw_centered("wants to connect", 78, 2, ST77XX_WHITE);
    draw_centered(ui_radio_name(), 100, 2,
        ui_is_espnow() ? ST77XX_WHITE : highlight_color);
    // The request opened under the session key, which only the other
    // holder of the pre-shared pair key can make.
    draw_centered("verified by pair key", 128, 1, listen_color);
    lcd.fillRoundRect(8, 150, 108, 40, 6, listen_color);
    draw_text("OK", 50, 162, 2, ST77XX_BLACK);
    lcd.fillRoundRect(124, 150, 108, 40, 6, talk_color);
    draw_text("CANCEL", 142, 162, 2, ST77XX_BLACK);
    draw_hints("OK: accept", "CANCEL: decline");
}

// Connected: push-to-talk

talk_view current_view(uint32_t now)
{
    if (tx == tx_phase::talking)
    {
        return talk_view::talking;
    }
    if (tx == tx_phase::stopping && !tx_yielded)
    {
        return talk_view::sending;
    }
    if (rx_play_at_us.load() != 0)
    {
        return talk_view::listening;
    }
    return notice_showing(now) ? talk_view::notice : talk_view::idle;
}

void draw_box_lines(uint16_t fill, const char *first, uint8_t first_size,
    const char *second, uint16_t text_color)
{
    lcd.fillRoundRect(8, box_top, lcd.width() - 16, box_height, 6, fill);
    if (!second)
    {
        draw_centered(first, box_top + (box_height - 8 * first_size) / 2,
            first_size, text_color);
        return;
    }
    draw_centered(first, box_top + 8, first_size, text_color);
    draw_centered(second, box_top + box_height - 24, 2, text_color);
}

void draw_talk_box(uint32_t now)
{
    const talk_view view = current_view(now);
    ui_drawn_view = view;
    switch (view)
    {
    case talk_view::idle:
        draw_box_lines(idle_box_color, "Hold OK to talk", 2, nullptr,
            ST77XX_WHITE);
        break;
    case talk_view::talking:
        draw_box_lines(talk_color, "TALKING", 3, nullptr, ST77XX_BLACK);
        break;
    case talk_view::sending:
        draw_box_lines(talk_color, "Sending...", 3, nullptr, ST77XX_BLACK);
        break;
    case talk_view::listening:
        draw_box_lines(listen_color, device_name(ui_calls().peer()), 3,
            "is talking", ST77XX_BLACK);
        break;
    case talk_view::notice:
        draw_box_lines(highlight_color, ui_notice_title, 2, ui_notice_text,
            ST77XX_BLACK);
        break;
    }
}

void draw_call_volume()
{
    ui_drawn_volume = speaker::volume();
    ui_drawn_muted = speaker::muted();
    char line[12];
    snprintf(line, sizeof(line), "Vol %2d", ui_drawn_volume);
    draw_field(line, 8, volume_top, 2, ST77XX_WHITE, 80);
    const int16_t left = 92;
    const int16_t width = lcd.width() - 8 - left;
    const int16_t filled = static_cast<int16_t>(
        width * ui_drawn_volume / audio_dsp::speaker_guard::max_volume);
    lcd.fillRect(left, volume_top + 2, width, 12, outline_color);
    lcd.fillRect(left, volume_top + 2, filled, 12,
        ui_drawn_muted ? muted_color : highlight_color);
}

void draw_last_heard(uint32_t now)
{
    ui_drawn_heard = rx_last_end_ms;
    ui_drawn_second = now / 1000;
    if (last_expected == 0 || rx_last_end_ms < ui_calls().since_ms())
    {
        draw_field("Nothing heard yet", 8, heard_top, 1, muted_color, 224);
        draw_field("", 8, heard_top + 12, 1, muted_color, 224);
        return;
    }
    char line[40];
    snprintf(line, sizeof(line), "Last heard %lu s ago",
        static_cast<unsigned long>((now - rx_last_end_ms) / 1000));
    draw_field(line, 8, heard_top, 1, ST77XX_WHITE, 224);
    snprintf(line, sizeof(line), "%lu/%lu frames %s",
        static_cast<unsigned long>(last_played),
        static_cast<unsigned long>(last_expected),
        last_intact ? "ok" : "MISSING");
    draw_field(line, 8, heard_top + 12, 1,
        last_intact ? muted_color : talk_color, 224);
}

// The route line under the board's name: over the mesh, the boards the
// frames pass (green direct, orange through a middleman, grey while the
// peers' pings haven't said yet); ESP-NOW is always direct.
constexpr int16_t route_top = 69;
char ui_drawn_route[40] = {};

uint16_t route_line(uint32_t now, char *text, size_t capacity)
{
    const via link = ui_via();
    if (link == via::espnow)
    {
        snprintf(text, capacity, "Route: %u > %u, direct (ESP-NOW)", own_id,
            calls.peer());
        return listen_color;
    }
    char route[32];
    route_text_via(link, way(link).calls.peer(), now, route, sizeof(route));
    snprintf(text, capacity, "Route: %s", route);
    return route_color(route);
}

// Draws a route_line() result and keeps it, to redraw only on a change.
void draw_route(const char *text, uint16_t color)
{
    snprintf(ui_drawn_route, sizeof(ui_drawn_route), "%s", text);
    draw_field(text, 8, route_top, 1, color, 224);
}

// Push-to-talk over any Wi-Fi way: the same screen, titled by the way.
void draw_connected(uint32_t now)
{
    draw_title(way(ui_via()).title, listen_color);
    char line[24];
    snprintf(line, sizeof(line), "%s", device_name(ui_calls().peer()));
    draw_text(line, 8, 44, 2, highlight_color);
    draw_elapsed("", 160, 48, 1, now);
    lcd.drawFastHLine(8, 64, lcd.width() - 16, outline_color);
    char route[sizeof(ui_drawn_route)];
    const uint16_t color = route_line(now, route, sizeof(route));
    draw_route(route, color);
    draw_talk_box(now);
    draw_call_volume();
    draw_last_heard(now);
    draw_hints("Hold OK: talk   UP/DOWN: volume",
        "RIGHT: say hello   CANCEL: disconnect");
}

// Connected over LoRa: pre-built texts only

bool lora_notice_showing(uint32_t now)
{
    return notice_showing(now) && ui_is_lora();
}

// The box: what UP and DOWN send, or a short notice after a press.
void draw_lora_box(uint32_t now)
{
    ui_drawn_lora_notice = lora_notice_showing(now);
    if (ui_drawn_lora_notice)
    {
        // Sending: orange; Delivered: green; Not sent / Not delivered: red.
        draw_box_lines(ui_notice_title[0] == 'D' ? listen_color :
                ui_notice_title[0] == 'N' ? talk_color : highlight_color,
            ui_notice_title, 2, ui_notice_text, ST77XX_BLACK);
        return;
    }
    lcd.fillRoundRect(8, box_top, lcd.width() - 16, box_height, 6,
        idle_box_color);
    draw_text("UP", 20, box_top + 12, 2, highlight_color);
    draw_text("Hello", 80, box_top + 12, 2, ST77XX_WHITE);
    draw_text("DOWN", 20, box_top + 38, 2, highlight_color);
    draw_text("Bye", 80, box_top + 38, 2, ST77XX_WHITE);
}

void draw_lora_last(uint32_t now)
{
    ui_drawn_lora_in = lora_text_in_count;
    ui_drawn_lora_out = lora_text_out_count;
    ui_drawn_second = now / 1000;
    if (lora_text_in[0] == '\0')
    {
        draw_field("Nothing received yet", 8, volume_top, 1, muted_color,
            224);
        draw_field("", 8, volume_top + 12, 2, ST77XX_WHITE, 224);
    }
    else
    {
        char line[40];
        snprintf(line, sizeof(line), "Received %lu s ago:",
            static_cast<unsigned long>((now - lora_text_in_ms) / 1000));
        draw_field(line, 8, volume_top, 1, muted_color, 224);
        draw_field(lora_text_in, 8, volume_top + 12, 2, listen_color, 224);
    }
    const lora_peer &peer = lora_peers[lora_calls.peer()];
    char line[40];
    if (peer.heard)
    {
        snprintf(line, sizeof(line), "RSSI %.0f dBm  SNR %.1f dB", peer.rssi,
            peer.snr);
    }
    else
    {
        snprintf(line, sizeof(line), "No signal reading yet");
    }
    draw_field(line, 8, volume_top + 34, 1, muted_color, 224);
}

void draw_lora_connected(uint32_t now)
{
    draw_title("LoRa", listen_color);
    char line[24];
    snprintf(line, sizeof(line), "%s", device_name(lora_calls.peer()));
    draw_text(line, 8, 44, 2, highlight_color);
    draw_elapsed("", 160, 48, 1, now);
    lcd.drawFastHLine(8, 70, lcd.width() - 16, outline_color);
    ui_shown_delivery = lora_delivery_count;
    draw_lora_box(now);
    draw_lora_last(now);
    draw_hints("UP: send Hello   DOWN: send Bye", "CANCEL: disconnect");
}

// A receipt came back, or every try went unanswered.
void show_lora_delivery(uint32_t now)
{
    ui_shown_delivery = lora_delivery_count;
    const char *word = lora_text_word == lora_word::hello ? "Hello" : "Bye";
    if (lora_delivery_state == lora_delivery::delivered)
    {
        notice("Delivered", word, now);
    }
    else if (lora_delivery_state == lora_delivery::failed)
    {
        notice("Not delivered", word, now);
        refused_beep();
    }
    draw_lora_box(now);
}

void say_lora(lora_word word, uint32_t now)
{
    switch (send_lora_text(word, now))
    {
    case text_result::sent:
        // Then Delivered or Not delivered (show_lora_delivery).
        notice("Sending", word == lora_word::hello ? "Hello" : "Bye", now);
        break;
    case text_result::offline:
        notice("Not sent", "LoRa radio", now);
        refused_beep();
        break;
    case text_result::too_soon:
        break;  // One text per second: the first is on its way.
    case text_result::not_connected:
        break;  // Only offered on the LoRa connected screen.
    }
    draw_lora_box(now);
}

// Disconnected

const char *end_text(call::end_reason reason)
{
    switch (reason)
    {
    case call::end_reason::hung_up_here: return "You disconnected";
    case call::end_reason::hung_up_there: return "They disconnected";
    case call::end_reason::cancelled_here: return "Stopped asking";
    case call::end_reason::cancelled_there: return "Missed request";
    case call::end_reason::declined_here: return "You declined";
    case call::end_reason::declined_there: return "Declined";
    case call::end_reason::no_answer: return "No answer";
    case call::end_reason::busy: return "Busy";
    case call::end_reason::link_lost: return "Connection lost";
    case call::end_reason::unreachable: return "Not reachable";
    case call::end_reason::none: break;
    }
    return "Disconnected";
}

void draw_ended()
{
    draw_title("Disconnected", talk_color);
    draw_peer_name(64);
    draw_centered(end_text(ui_calls().last_end()), 116, 2, ST77XX_WHITE);
    draw_centered(ui_radio_label(), 146, 1, muted_color);
    draw_hints(nullptr, "Any button: back");
}

const char *screen_name(screen value)
{
    switch (value)
    {
    case screen::menu: return "menu";
    case screen::espnow_list: return "espnow";
    case screen::mesh_list: return "mesh";
    case screen::flood_list: return "flood";
    case screen::routed_list: return "routed";
    case screen::lora_list: return "lora";
    case screen::tools: return "tools";
    case screen::lora_connected: return "lora_connected";
    case screen::volume: return "volume";
    case screen::status: return "status";
    case screen::routes: return "routes";
    case screen::connecting: return "connecting";
    case screen::request: return "request";
    case screen::connected: return "connected";
    case screen::ended: return "disconnected";
    case screen::none: break;
    }
    return "none";
}

uint32_t ui_screen_since_ms = 0;  // When the current screen finished drawing.

void show(screen next, uint32_t now)
{
    const uint32_t started = millis();
    shown = next;
    page_scroll = 0;  // The Status and Routes pages open at the top.
    page_drawn_layout = UINT32_MAX;
    lcd.fillScreen(ST77XX_BLACK);
    switch (next)
    {
    case screen::menu:
    case screen::espnow_list:
    case screen::mesh_list:
    case screen::flood_list:
    case screen::routed_list:
    case screen::lora_list:
    case screen::tools: draw_menu(next, now); break;
    case screen::lora_connected: draw_lora_connected(now); break;
    case screen::volume: draw_volume_page(); break;
    case screen::status: draw_status_page(); break;
    case screen::routes: draw_routes_page(now); break;
    case screen::connecting: draw_connecting(now); break;
    case screen::request: draw_request(); break;
    case screen::connected: draw_connected(now); break;
    case screen::ended: draw_ended(); break;
    case screen::none: break;
    }
    // Evidence for the design review's 400 ms UI response target.
    Serial.printf("UI screen=%s draw_ms=%lu\n", screen_name(next),
        static_cast<unsigned long>(millis() - started));
    ui_screen_since_ms = millis();
    ui_toast_drawn = false;  // The new screen covered it.
}

// The banner covers the two hint lines at the bottom of every screen.
constexpr int16_t toast_top = 206;

void draw_toast()
{
    lcd.fillRoundRect(4, toast_top, lcd.width() - 8, lcd.height() - toast_top
        - 2, 6, ui_toast_color);
    const uint8_t size = text_width(ui_toast_text, 2) <= lcd.width() - 16 ?
        2 : 1;
    draw_centered(ui_toast_text,
        toast_top + (lcd.height() - toast_top - 2 - 8 * size) / 2, size,
        ST77XX_BLACK);
}

// Shows a new incoming hello, draws the banner over the current screen,
// and puts the hint lines back when it times out.
void service_toast(uint32_t now)
{
    if (text_in_count != ui_text_seen)
    {
        ui_text_seen = text_in_count;
        toast(text_in, highlight_color, now);
    }
    if (shown == screen::none)
    {
        return;
    }
    if (toast_showing(now))
    {
        if (!ui_toast_drawn)
        {
            draw_toast();
            ui_toast_drawn = true;
        }
        return;
    }
    if (ui_toast_drawn)
    {
        ui_toast_drawn = false;
        lcd.fillRect(0, toast_top, lcd.width(), lcd.height() - toast_top,
            ST77XX_BLACK);
        if (is_list(shown))
        {
            draw_menu_notice(now);
        }
        draw_hints(ui_hint_first, ui_hint_second);
    }
}

// The screen the connection state calls for.
screen wanted_screen(uint32_t now)
{
    switch (ui_calls().current())
    {
    case call::state::calling: return screen::connecting;
    case call::state::ringing: return screen::request;
    case call::state::connected:
        return ui_is_lora() ? screen::lora_connected : screen::connected;
    case call::state::idle: break;
    }
    if (ui_end_pending)
    {
        if (now - ui_end_shown_ms < app_config::ui_end_screen_ms)
        {
            return screen::ended;
        }
        ui_end_pending = false;
    }
    return page;
}

// Buttons

void move_selection(int step)
{
    const uint8_t previous = selected;
    selected = static_cast<uint8_t>((selected + step + row_count) % row_count);
    // Scroll so the selection stays in view (wrapping jumps to the end).
    const uint8_t old_first = first_visible;
    if (selected < first_visible)
    {
        first_visible = selected;
    }
    else if (selected >= first_visible + visible_rows)
    {
        first_visible = static_cast<uint8_t>(selected - visible_rows + 1);
    }
    if (first_visible != old_first)
    {
        draw_menu_rows();
        return;
    }
    draw_menu_row(previous);
    draw_menu_row(selected);
}

// RIGHT while connected: to the connected board only.
void say_hello(uint32_t now)
{
    switch (send_hello(now))
    {
    case text_result::sent:
        toast("Hello sent", listen_color, now);
        break;
    case text_result::offline:
        toast("Not reachable", talk_color, now);
        refused_beep();
        break;
    case text_result::too_soon:
        break;  // A second press within a second: the first is on its way.
    case text_result::not_connected:
        break;  // Only offered on the Connected screen.
    }
}

void change_volume(int step)
{
    speaker::set_volume(static_cast<uint8_t>(speaker::volume() + step));
    Serial.printf("UI volume %u\n", speaker::volume());
}

void ptt_down_from_ui(uint32_t now)
{
    if (ui_ptt_held ||
        way(voice_via()).calls.current() != call::state::connected)
    {
        return;
    }
    if (ptt_pressed())
    {
        ui_ptt_held = true;
        return;
    }
    // Refused. While the other side talks the box already says so (half
    // duplex), and a beep would play over their voice.
    if (rx_play_at_us.load() != 0)
    {
        return;
    }
    notice("Can't talk", can_send_via(voice_via(), peer_id) ? "try again" :
        "no session", now);
    refused_beep();
}

void ptt_up_from_ui()
{
    if (ui_ptt_held)
    {
        ui_ptt_held = false;
        ptt_released();
    }
}

// OK on a Tools row: switches its test setting, says so in the notice line,
// and redraws the rows (a label or dot may change).
void tool_activate(const menu_row &row, uint32_t now)
{
    switch (row.kind)
    {
    case row_kind::tool_block:
    {
        const bool on = !relay_is_blocked(row.peer);
        relay_set_block(row.peer, on);
        notice(device_name(row.peer), on ? "blocked" : "unblocked", now);
        break;
    }
    case row_kind::tool_rate:
    {
        const unsigned next = radio::rate_mbps() == 1 ? 6 : 1;
        radio::configure(network_config::channel, next);
        notice("Rate", next == 6 ? "6 Mbps" : "1 Mbps", now);
        break;
    }
    case row_kind::tool_mesh_leave:
        if (mesh_switched_off)
        {
            mesh_rejoin(now);
            notice("Mesh", "rejoining", now);
        }
        else
        {
            mesh_leave();
            notice("Mesh", "left", now);
        }
        break;
    case row_kind::tool_mesh_root:
        mesh_force_root(now);
        notice("Mesh", "becoming root", now);
        break;
    case row_kind::tool_mute_flood:
        relay_set_muted(flood_net, !flood_net.muted);
        notice("Flood", flood_net.muted ? "muted" : "unmuted", now);
        break;
    case row_kind::tool_mute_routed:
        relay_set_muted(routed_net, !routed_net.muted);
        notice("Routed", routed_net.muted ? "muted" : "unmuted", now);
        break;
    case row_kind::tool_mute_mesh:
        mesh_set_muted(!mesh_muted);
        notice("Mesh", mesh_muted ? "muted" : "unmuted", now);
        break;
    case row_kind::tool_mute_lora:
        lora_set_muted(!lora_muted);
        notice("LoRa", lora_muted ? "muted" : "unmuted", now);
        break;
    default:
        return;
    }
    ui_drawn_tools = tools_state();
    draw_menu_rows();
    draw_menu_notice(now);
}

void handle_press(button_id id, uint32_t now)
{
    if (id == button_id::ptt)
    {
        ptt_down_from_ui(now);  // Works on any screen while connected.
        return;
    }
    const bool back = id == button_id::up || id == button_id::left;
    const bool forward = id == button_id::down || id == button_id::right;
    switch (shown)
    {
    case screen::menu:
    case screen::espnow_list:
    case screen::mesh_list:
    case screen::flood_list:
    case screen::routed_list:
    case screen::lora_list:
    case screen::tools:
        if (back || forward)
        {
            move_selection(back ? -1 : 1);
        }
        else if (id == button_id::cancel && shown != screen::menu)
        {
            page = screen::menu;
        }
        else if (id == button_id::confirm)
        {
            const menu_row &row = rows[selected];
            switch (row.kind)
            {
            case row_kind::espnow_page: page = screen::espnow_list; break;
            case row_kind::mesh_page: page = screen::mesh_list; break;
            case row_kind::flood_page: page = screen::flood_list; break;
            case row_kind::routed_page: page = screen::routed_list; break;
            case row_kind::lora_page: page = screen::lora_list; break;
            case row_kind::volume: page = screen::volume; break;
            case row_kind::status: page = screen::status; break;
            case row_kind::routes_page: page = screen::routes; break;
            case row_kind::tools_page: page = screen::tools; break;
            case row_kind::tool_block:
            case row_kind::tool_rate:
            case row_kind::tool_mesh_leave:
            case row_kind::tool_mesh_root:
            case row_kind::tool_mute_flood:
            case row_kind::tool_mute_routed:
            case row_kind::tool_mute_mesh:
            case row_kind::tool_mute_lora:
                tool_activate(row, now);
                break;
            case row_kind::connect:
                if (!place_call(row.peer, now))
                {
                    notice(device_name(row.peer), "not reachable", now);
                    draw_menu_notice(now);
                    refused_beep();
                }
                break;
            case row_kind::mesh_connect:
                if (!place_mesh_call(row.peer, now))
                {
                    notice(device_name(row.peer), mesh_ready() ?
                        "not reachable" : "not in mesh", now);
                    draw_menu_notice(now);
                    refused_beep();
                }
                break;
            case row_kind::flood_connect:
            case row_kind::routed_connect:
                if (!place_relay_call(row.kind == row_kind::routed_connect ?
                        routed_net : flood_net, row.peer, now))
                {
                    notice(device_name(row.peer), "not reachable", now);
                    draw_menu_notice(now);
                    refused_beep();
                }
                break;
            case row_kind::lora_connect:
                if (!place_lora_call(row.peer, now))
                {
                    notice(device_name(row.peer), lora_ready() ?
                        "no LoRa link yet" : "LoRa not ready", now);
                    draw_menu_notice(now);
                    refused_beep();
                }
                break;
            }
        }
        break;
    case screen::volume:
        if (id == button_id::up || id == button_id::right)
        {
            change_volume(1);
        }
        else if (id == button_id::down || id == button_id::left)
        {
            change_volume(-1);
        }
        else if (id == button_id::confirm)
        {
            // Any new incoming stream unmutes by itself too.
            speaker::set_muted(!speaker::muted());
            Serial.printf("UI speaker %s\n", speaker::muted() ? "muted" :
                "unmuted");
        }
        else if (id == button_id::cancel)
        {
            page = screen::menu;
        }
        break;
    case screen::status:
    case screen::routes:
        if (id == button_id::up || id == button_id::down)
        {
            scroll_page(id == button_id::down ? 1 : -1, now);
        }
        else if (id == button_id::cancel || id == button_id::confirm)
        {
            page = screen::menu;
        }
        break;
    case screen::connecting:
        if (id == button_id::cancel)
        {
            ui_calls().hang_up(now);
        }
        break;
    case screen::request:
        // Signed: `now` is from before the screen was drawn.
        if (static_cast<int32_t>(now - ui_screen_since_ms) <
            static_cast<int32_t>(app_config::ui_request_guard_ms))
        {
            Serial.println("BUTTON ignored: the request has just appeared");
        }
        else if (id == button_id::confirm)
        {
            ui_calls().answer(now);
        }
        else if (id == button_id::cancel)
        {
            ui_calls().decline(now);
        }
        break;
    case screen::connected:
        if (id == button_id::confirm)
        {
            ptt_down_from_ui(now);
        }
        else if (id == button_id::up)
        {
            change_volume(1);
        }
        else if (id == button_id::down || id == button_id::left)
        {
            change_volume(-1);
        }
        else if (id == button_id::right)
        {
            say_hello(now);
        }
        else if (id == button_id::cancel)
        {
            ptt_up_from_ui();
            ui_calls().hang_up(now);  // ESP-NOW or ESP-MESH.
        }
        break;
    case screen::lora_connected:
        if (id == button_id::up)
        {
            say_lora(lora_word::hello, now);
        }
        else if (id == button_id::down)
        {
            say_lora(lora_word::bye, now);
        }
        else if (id == button_id::cancel)
        {
            lora_calls.hang_up(now);
        }
        break;
    case screen::ended:
        ui_end_pending = false;
        break;
    case screen::none:
        break;
    }
}

void handle_release(button_id id)
{
    if (id == button_id::confirm || id == button_id::ptt)
    {
        ptt_up_from_ui();
    }
}

// Samples every button before acting on any, so a slow redraw does not
// delay reading the others.
void poll_buttons(uint32_t now)
{
    uint8_t presses = 0;
    uint8_t releases = 0;
    for (size_t i = 0; i < button_count; ++i)
    {
        button &b = buttons[i];
        if (b.pin < 0)
        {
            continue;
        }
        const bool sampled = read_pressed(b);
        if (sampled != b.raw_pressed)
        {
            b.raw_pressed = sampled;
            b.raw_changed_ms = now;
        }
        if (b.raw_pressed == b.stable_pressed ||
            now - b.raw_changed_ms < app_config::debounce_ms)
        {
            continue;
        }
        b.stable_pressed = b.raw_pressed;
        if (b.stable_pressed)
        {
            presses |= 1u << i;
        }
        else
        {
            releases |= 1u << i;
        }
    }
    for (size_t i = 0; i < button_count; ++i)
    {
        if (releases & (1u << i))
        {
            handle_release(buttons[i].id);
        }
        if (presses & (1u << i))
        {
            Serial.printf("BUTTON %s\n", buttons[i].name);
            handle_press(buttons[i].id, now);
        }
    }
}

// Serial buttons, for testing without hands on the D-pad: `press NAME
// [MS]`, `hold NAME`, `release [NAME]`. They go through handle_press and
// handle_release like the real buttons, so the screens, the request guard
// and hold-OK-to-talk behave the same. Each button's serial state is
// separate from its pin, so PTT works from serial even though it is not
// wired.
struct serial_button
{
    bool held;
    bool timed;           // Released by service_serial_buttons().
    uint32_t release_at;  // Loop time of the timed release.
};
serial_button serial_buttons[button_count] = {};
constexpr uint32_t serial_tap_ms = 100;
constexpr uint32_t serial_hold_max_ms = 60000;

int find_button(const char *name)
{
    for (size_t i = 0; i < button_count; ++i)
    {
        if (strcasecmp(name, buttons[i].name) == 0)
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void serial_press(size_t i, uint32_t now)
{
    if (serial_buttons[i].held)
    {
        return;
    }
    serial_buttons[i].held = true;
    Serial.printf("BUTTON %s (serial)\n", buttons[i].name);
    handle_press(buttons[i].id, now);
}

void serial_release(size_t i)
{
    if (!serial_buttons[i].held)
    {
        return;
    }
    serial_buttons[i] = {};
    Serial.printf("BUTTON %s released (serial)\n", buttons[i].name);
    handle_release(buttons[i].id);
}

void service_serial_buttons(uint32_t now)
{
    for (size_t i = 0; i < button_count; ++i)
    {
        const serial_button &b = serial_buttons[i];
        if (b.held && b.timed &&
            static_cast<int32_t>(now - b.release_at) >= 0)
        {
            serial_release(i);
        }
    }
}

const char *talk_view_name(talk_view view)
{
    switch (view)
    {
    case talk_view::idle: return "idle";
    case talk_view::talking: return "talking";
    case talk_view::sending: return "sending";
    case talk_view::listening: return "listening";
    case talk_view::notice: return "notice";
    }
    return "?";
}

// What the screen shows, for a serial-driven test that can't see the LCD.
void print_ui(uint32_t now)
{
    Serial.printf("UI | screen %s", screen_name(shown));
    if (is_list(shown) && row_count > 0)
    {
        char label[24];
        row_label(rows[selected], label, sizeof(label));
        Serial.printf(" | selected %u/%u \"%s\"", selected + 1, row_count,
            label);
        const bool *dots = row_dots(rows[selected].kind);
        if (dots)
        {
            Serial.printf(" %s", dots[rows[selected].peer] ? "(green)" :
                "(grey)");
        }
        else if (rows[selected].kind >= row_kind::tool_block)
        {
            uint16_t color = outline_color;
            if (tool_dot(rows[selected], &color))
            {
                Serial.printf(" %s", color == talk_color ? "(red: on)" :
                    color == listen_color ? "(green)" : "(off)");
            }
        }
    }
    if (shown == screen::connecting || shown == screen::request)
    {
        Serial.printf(" | %s %s", device_name(ui_calls().peer()),
            ui_radio_name());
    }
    if (shown == screen::lora_connected)
    {
        Serial.printf(" | sent \"%s\" (%s) | received \"%s\"",
            lora_text_out, lora_delivery_name(lora_delivery_state),
            lora_text_in);
    }
    if (shown == screen::connected)
    {
        Serial.printf(" | %s | box %s | %s", ui_radio_label(),
            talk_view_name(current_view(now)), ui_drawn_route);
    }
    if (shown == screen::ended)
    {
        Serial.printf(" | \"%s\" (%s)", end_text(ui_calls().last_end()),
            ui_radio_label());
    }
    if (shown == screen::status || shown == screen::routes)
    {
        Serial.printf(" | lines %u-%u of %u",
            static_cast<unsigned>(page_scroll + 1),
            static_cast<unsigned>(page_scroll + page_shown),
            static_cast<unsigned>(page_line_count));
    }
    if (notice_showing(now))
    {
        Serial.printf(" | notice \"%s %s\"", ui_notice_title,
            ui_notice_text);
    }
    if (toast_showing(now))
    {
        Serial.printf(" | banner \"%s\"", ui_toast_text);
    }
    Serial.printf(" | volume %u\n", speaker::volume());
}

// Returns false if the line is not a button command.
bool run_button_command(const char *line, uint32_t now)
{
    char name[12] = {};
    unsigned ms = 0;
    char extra = 0;
    const int pressed = sscanf(line, "press %11s %u %c", name, &ms, &extra);
    if (pressed == 1 || pressed == 2)
    {
        const int i = find_button(name);
        if (i < 0 || (pressed == 2 && (ms == 0 || ms > serial_hold_max_ms)))
        {
            Serial.println("BUTTON usage: press up|down|left|right|ok|"
                           "cancel|ptt [1..60000 ms]");
            return true;
        }
        serial_press(static_cast<size_t>(i), now);
        serial_buttons[i].timed = true;
        serial_buttons[i].release_at = now + (pressed == 2 ? ms :
            serial_tap_ms);
        return true;
    }
    if (sscanf(line, "hold %11s %c", name, &extra) == 1)
    {
        const int i = find_button(name);
        if (i < 0)
        {
            Serial.println("BUTTON usage: hold NAME, then release NAME");
            return true;
        }
        serial_press(static_cast<size_t>(i), now);
        serial_buttons[i].timed = false;
        return true;
    }
    if (strcmp(line, "release") == 0)
    {
        for (size_t i = 0; i < button_count; ++i)
        {
            serial_release(i);
        }
        return true;
    }
    if (sscanf(line, "release %11s %c", name, &extra) == 1)
    {
        const int i = find_button(name);
        if (i < 0)
        {
            Serial.println("BUTTON usage: release [NAME]");
            return true;
        }
        serial_release(static_cast<size_t>(i));
        return true;
    }
    if (strcmp(line, "ui") == 0)
    {
        print_ui(now);
        return true;
    }
    return false;
}

// Redraws the live parts of the current screen that changed.
void refresh(uint32_t now)
{
    if (now - ui_last_refresh_ms < app_config::ui_refresh_ms)
    {
        return;
    }
    ui_last_refresh_ms = now;
    switch (shown)
    {
    case screen::menu:
    case screen::espnow_list:
    case screen::mesh_list:
    case screen::flood_list:
    case screen::routed_list:
    case screen::lora_list:
    case screen::tools:
        if (shown == screen::tools)
        {
            const uint32_t tools = tools_state();
            if (tools != ui_drawn_tools)
            {
                ui_drawn_tools = tools;
                draw_menu_rows();  // Changed from serial, or by the mesh.
            }
        }
        for (uint8_t i = 0; i < row_count; ++i)
        {
            bool *dots = row_dots(rows[i].kind);
            if (!dots)
            {
                continue;
            }
            const bool up = row_up(rows[i], now);
            if (up != dots[rows[i].peer])
            {
                dots[rows[i].peer] = up;
                draw_menu_row(i);
            }
        }
        if (ui_drawn_lora != lora_radio::current())
        {
            ui_drawn_lora = lora_radio::current();
            for (uint8_t i = 0; i < row_count; ++i)
            {
                if (rows[i].kind == row_kind::lora_page)
                {
                    draw_menu_row(i);
                }
            }
            if (shown == screen::lora_list && !toast_showing(now))
            {
                draw_field(lora_hint(), 8, 226, 1, muted_color, 224);
                ui_hint_second = ui_lora_hint;
            }
        }
        if (ui_drawn_mesh != mesh_radio::current())
        {
            ui_drawn_mesh = mesh_radio::current();
            for (uint8_t i = 0; i < row_count; ++i)
            {
                if (rows[i].kind == row_kind::mesh_page)
                {
                    draw_menu_row(i);
                }
            }
        }
        if (shown == screen::mesh_list && !toast_showing(now))
        {
            // Layer and board count change without the state changing.
            char previous[sizeof(ui_mesh_hint)];
            memcpy(previous, ui_mesh_hint, sizeof(previous));
            if (strcmp(previous, mesh_hint()) != 0)
            {
                draw_field(ui_mesh_hint, 8, 226, 1, muted_color, 224);
                ui_hint_second = ui_mesh_hint;
            }
        }
        if (ui_notice_on && !notice_showing(now))
        {
            draw_menu_notice(now);
        }
        break;
    case screen::volume:
        if (ui_drawn_volume != speaker::volume() ||
            ui_drawn_muted != speaker::muted())
        {
            draw_volume_page_value();
        }
        break;
    case screen::status:
        if (now / 1000 != ui_drawn_second)
        {
            ui_drawn_second = now / 1000;
            draw_status_values();
        }
        break;
    case screen::routes:
        if (now / 1000 != ui_drawn_second)
        {
            ui_drawn_second = now / 1000;
            draw_routes_values(now);
        }
        break;
    case screen::connecting:
        if ((now - ui_calls().since_ms()) / 1000 != ui_drawn_elapsed)
        {
            draw_elapsed("Waiting", 8, 140, 2, now);
        }
        break;
    case screen::lora_connected:
        if (lora_delivery_count != ui_shown_delivery)
        {
            show_lora_delivery(now);
        }
        else if (lora_notice_showing(now) != ui_drawn_lora_notice)
        {
            draw_lora_box(now);
        }
        if ((now - lora_calls.since_ms()) / 1000 != ui_drawn_elapsed)
        {
            draw_elapsed("", 160, 48, 1, now);
        }
        if (ui_drawn_lora_in != lora_text_in_count ||
            ui_drawn_lora_out != lora_text_out_count ||
            now / 1000 != ui_drawn_second)
        {
            draw_lora_last(now);
        }
        break;
    case screen::connected:
        if (current_view(now) != ui_drawn_view)
        {
            draw_talk_box(now);
        }
        if (ui_drawn_volume != speaker::volume() ||
            ui_drawn_muted != speaker::muted())
        {
            draw_call_volume();
        }
        if ((now - ui_calls().since_ms()) / 1000 != ui_drawn_elapsed)
        {
            draw_elapsed("", 160, 48, 1, now);
        }
        {
            char route[sizeof(ui_drawn_route)];
            const uint16_t color = route_line(now, route, sizeof(route));
            if (strcmp(route, ui_drawn_route) != 0)
            {
                draw_route(route, color);  // A middleman came or went.
            }
        }
        if (ui_drawn_heard != rx_last_end_ms ||
            (last_expected != 0 && now / 1000 != ui_drawn_second))
        {
            draw_last_heard(now);
        }
        break;
    case screen::request:
    case screen::ended:
    case screen::none:
        break;
    }
}

void setup_ui()
{
    // Set the level before enabling the output so BLK never glitches low.
    digitalWrite(app_config::lcd_backlight_pin, HIGH);
    pinMode(app_config::lcd_backlight_pin, OUTPUT);
    lcd_spi.begin(app_config::lcd_sclk_pin, -1, app_config::lcd_mosi_pin,
        app_config::lcd_cs_pin);
    lcd.init(app_config::lcd_width, app_config::lcd_height, SPI_MODE3);
    lcd.setSPISpeed(app_config::lcd_spi_hz);
    lcd.setRotation(app_config::lcd_rotation);
    lcd.invertDisplay(app_config::lcd_inverted);
    lcd.setTextWrap(false);

    for (button &b : buttons)
    {
        if (b.pin >= 0)
        {
            pinMode(b.pin, active_low(b) ? INPUT_PULLUP : INPUT_PULLDOWN);
        }
    }
    delay(1);  // Let the pulls settle before the first readings.
    for (button &b : buttons)
    {
        if (b.pin < 0)
        {
            continue;
        }
        // A button held at start-up is ignored until released.
        b.raw_pressed = b.stable_pressed = read_pressed(b);
        Serial.printf("BUTTON %-6s GPIO%d to %s%s\n", b.name, b.pin,
            active_low(b) ? "GND" : "3V3",
            b.stable_pressed ? " (held at startup, ignored)" : "");
    }
    if (app_config::ptt_pin < 0)
    {
        Serial.println("BUTTON PTT not wired: hold OK to talk");
    }
    show(screen::menu, millis());
}

// Start-up failed: say why on screen; only `reboot` works. The MAC is
// shown so a new board can be provisioned from its screen.
void show_stopped(const char *reason)
{
    lcd.fillScreen(ST77XX_BLACK);
    lcd.fillRect(0, 0, lcd.width(), title_height, talk_color);
    char title[24];
    snprintf(title, sizeof(title), "Stopped (ID %u)", own_id);
    draw_text(title, 8, 9, 2, ST77XX_WHITE);
    draw_text(reason, 8, 50, 1, ST77XX_WHITE);
    uint8_t mac[6] = {};
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK)
    {
        char line[24];
        snprintf(line, sizeof(line), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0],
            mac[1], mac[2], mac[3], mac[4], mac[5]);
        draw_text("This board's MAC:", 8, 80, 1, muted_color);
        draw_text(line, 8, 94, 2, highlight_color);
    }
    draw_hints("Details in the serial log", "RESET button: restart");
}

void service_ui(uint32_t now)
{
    poll_buttons(now);
    service_serial_buttons(now);
    for (size_t i = 0; i < connection_count; ++i)
    {
        if (connections[i]->end_count() != ui_shown_ends[i])
        {
            ui_shown_ends[i] = connections[i]->end_count();
            ui_last_ended = connections[i];
            ui_end_pending = true;
            ui_end_shown_ms = now;
        }
    }
    if (ui_ptt_held && (tx == tx_phase::idle ||
            way(tx_via).calls.current() != call::state::connected))
    {
        ui_ptt_held = false;  // The stream ended by itself or was cut.
    }
    const screen next = wanted_screen(now);
    if (next != shown)
    {
        show(next, now);
    }
    else
    {
        refresh(now);
    }
    service_toast(now);
}
