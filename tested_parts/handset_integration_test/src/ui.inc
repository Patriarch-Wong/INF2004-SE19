// Included inside main.cpp's anonymous namespace, after handset.inc.
//
// D-pad and ST7789 screen, from dpad_menu_test. Screens:
//
//   Menu          "Connect <name>" per handset (scrolls past 4 rows),
//                 Volume, Status
//   Connecting    waiting for the other handset to accept; CANCEL stops
//   Request       who wants to connect; OK accepts, CANCEL declines
//   Connected     hold OK (or PTT) to talk, UP/DOWN volume,
//                 CANCEL disconnects (both sides drop)
//   Disconnected  why; any button or 3 s returns to the menu
//   Volume        UP/RIGHT louder, DOWN/LEFT quieter, CANCEL back
//   Status        IDs, sessions and counters, CANCEL back
//
// The connection state picks the screen: a request replaces whatever is
// shown. Screens redraw fully only when they change (about 100 ms at
// 10 MHz); live values are redrawn in place. Only the loop calls this code.

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
    volume,
    status,
    connecting,
    request,
    connected,
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
constexpr uint8_t max_rows = max_id + 1;  // Every other handset + 2 pages.
constexpr int16_t box_top = 84;
constexpr int16_t box_height = 64;
constexpr int16_t volume_top = 158;
constexpr int16_t heard_top = 182;

Adafruit_ST7789 lcd(&SPI, app_config::lcd_cs_pin, app_config::lcd_dc_pin,
    app_config::lcd_reset_pin);

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

// Menu rows: handsets to connect to, then the two pages.
enum class row_kind : uint8_t
{
    connect,
    volume,
    status,
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

screen shown = screen::none;
screen page = screen::menu;  // Menu, volume or status when not connected.
bool ui_ptt_held = false;    // A stream started from a button is running.
uint32_t ui_shown_ends = 0;
uint32_t ui_end_shown_ms = 0;
bool ui_end_pending = false;
uint32_t ui_last_refresh_ms = 0;
char ui_notice_title[16] = {};
char ui_notice_text[20] = {};
uint32_t ui_notice_ms = 0;
bool ui_notice_on = false;
bool ui_reachable[max_id + 1] = {};

// Last values drawn in place, so unchanged ones are not redrawn.
talk_view ui_drawn_view = talk_view::idle;
int ui_drawn_volume = -1;
bool ui_drawn_muted = false;
uint32_t ui_drawn_heard = UINT32_MAX;
uint32_t ui_drawn_second = UINT32_MAX;
uint32_t ui_drawn_elapsed = UINT32_MAX;

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
    draw_centered(device_name(calls.peer()), top, 3, highlight_color);
}

// "Label mm:ss" since the connection state was entered.
void draw_elapsed(const char *label, int16_t x, int16_t top, uint8_t size,
    uint32_t now)
{
    const uint32_t seconds = (now - calls.since_ms()) / 1000;
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
    case row_kind::connect:
        snprintf(label, capacity, "Connect %s", device_name(row.peer));
        break;
    case row_kind::volume:
        snprintf(label, capacity, "Volume");
        break;
    case row_kind::status:
        snprintf(label, capacity, "Status");
        break;
    }
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
    if (row.kind == row_kind::connect)
    {
        // Green: this handset is online (presence pings arriving), so it
        // can be asked.
        lcd.fillCircle(row_margin + width - 16, y + row_height / 2, 6,
            ui_reachable[row.peer] ? listen_color : outline_color);
    }
}

// The menu's first hint line, replaced by a notice ("Board 2 not
// reachable") while one shows.
void draw_menu_notice(uint32_t now)
{
    if (notice_showing(now))
    {
        char line[40];
        snprintf(line, sizeof(line), "%s %s", ui_notice_title,
            ui_notice_text);
        draw_field(line, 8, 212, 1, highlight_color, 224);
    }
    else
    {
        draw_field("UP/DOWN: choose   OK: select", 8, 212, 1, muted_color,
            224);
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

void draw_menu(uint32_t now)
{
    draw_title("Menu", title_color);
    draw_menu_rows();
    draw_menu_notice(now);
    draw_hints(nullptr, "Green dot: reachable");
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
    draw_field(ui_drawn_muted ? "Muted (serial m)" : "", 20, 170, 1,
        talk_color, 200);
}

void draw_volume_page()
{
    draw_title("Volume", title_color);
    draw_text("1..32", 8, 44, 1, muted_color);
    draw_volume_page_value();
    draw_hints("UP/RIGHT: louder  DOWN/LEFT: quieter", "CANCEL: back");
}

void draw_status_values()
{
    char line[40];
    int16_t y = 44;
    const network_config::device *self = radio::find_device(own_id);
    snprintf(line, sizeof(line), "ID %u  %02X:%02X:%02X:%02X:%02X:%02X", own_id,
        self->mac[0], self->mac[1], self->mac[2], self->mac[3], self->mac[4],
        self->mac[5]);
    draw_field(line, 8, y, 1, ST77XX_WHITE, 224);
    y += 14;
    snprintf(line, sizeof(line), "Channel %u, %u Mbps, buffer %lu ms",
        network_config::channel, network_config::rate_mbps,
        static_cast<unsigned long>(playout_ms));
    draw_field(line, 8, y, 1, ST77XX_WHITE, 224);
    y += 20;
    for (uint8_t id = 1; id <= max_id; ++id)
    {
        if (!valid_peer(id))
        {
            continue;
        }
        const session::link &link = peers[id].link;
        const bool online = peer_online(id, millis());
        if (online)
        {
            snprintf(line, sizeof(line), "%s: online %08lX", device_name(id),
                static_cast<unsigned long>(link.session_id()));
        }
        else
        {
            snprintf(line, sizeof(line), "%s: offline", device_name(id));
        }
        draw_field(line, 8, y, 1, online ? listen_color : muted_color, 224);
        y += 14;
    }
    y += 6;
    snprintf(line, sizeof(line), "Frames ok %lu  rejected %lu",
        static_cast<unsigned long>(accepted_count),
        static_cast<unsigned long>(rejected_count));
    draw_field(line, 8, y, 1, ST77XX_WHITE, 224);
    y += 14;
    snprintf(line, sizeof(line), "Sent %lu  played %lu  lost %lu",
        static_cast<unsigned long>(total_tx_frames),
        static_cast<unsigned long>(total_rx_played),
        static_cast<unsigned long>(total_rx_lost));
    draw_field(line, 8, y, 1, ST77XX_WHITE, 224);
    y += 14;
    snprintf(line, sizeof(line), "Uptime %lu s",
        static_cast<unsigned long>(millis() / 1000));
    draw_field(line, 8, y, 1, ST77XX_WHITE, 224);
}

void draw_status_page()
{
    draw_title("Status", title_color);
    draw_status_values();
    draw_hints(nullptr, "CANCEL: back");
}

// Connecting and request

void draw_connecting(uint32_t now)
{
    draw_title("Connecting", title_color);
    draw_centered("Asking", 56, 2, ST77XX_WHITE);
    draw_peer_name(84);
    draw_elapsed("Waiting", 8, 140, 2, now);
    draw_hints(nullptr, "CANCEL: stop");
}

void draw_request()
{
    draw_title("Request", highlight_color);
    draw_peer_name(56);
    draw_centered("wants to connect", 96, 2, ST77XX_WHITE);
    // The request opened under the session key, which only the other
    // holder of the pre-shared pair key can make.
    draw_centered("verified by pair key", 124, 1, listen_color);
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
        draw_box_lines(listen_color, device_name(calls.peer()), 3,
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
    if (last_expected == 0 || rx_last_end_ms < calls.since_ms())
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

void draw_connected(uint32_t now)
{
    draw_title("Connected", listen_color);
    char line[24];
    snprintf(line, sizeof(line), "%s", device_name(calls.peer()));
    draw_text(line, 8, 44, 2, highlight_color);
    draw_elapsed("", 160, 48, 1, now);
    lcd.drawFastHLine(8, 70, lcd.width() - 16, outline_color);
    draw_talk_box(now);
    draw_call_volume();
    draw_last_heard(now);
    draw_hints("Hold OK: talk   UP/DOWN: volume", "CANCEL: disconnect");
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
    draw_centered(end_text(calls.last_end()), 116, 2, ST77XX_WHITE);
    draw_hints(nullptr, "Any button: back");
}

const char *screen_name(screen value)
{
    switch (value)
    {
    case screen::menu: return "menu";
    case screen::volume: return "volume";
    case screen::status: return "status";
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
    lcd.fillScreen(ST77XX_BLACK);
    switch (next)
    {
    case screen::menu: draw_menu(now); break;
    case screen::volume: draw_volume_page(); break;
    case screen::status: draw_status_page(); break;
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
}

// The screen the connection state calls for.
screen wanted_screen(uint32_t now)
{
    switch (calls.current())
    {
    case call::state::calling: return screen::connecting;
    case call::state::ringing: return screen::request;
    case call::state::connected: return screen::connected;
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

void change_volume(int step)
{
    speaker::set_volume(static_cast<uint8_t>(speaker::volume() + step));
    Serial.printf("UI volume %u\n", speaker::volume());
}

void ptt_down_from_ui(uint32_t now)
{
    if (ui_ptt_held || calls.current() != call::state::connected)
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
    notice("Can't talk", can_send(peer_id) ? "try again" : "no session",
        now);
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
        if (back || forward)
        {
            move_selection(back ? -1 : 1);
        }
        else if (id == button_id::confirm)
        {
            const menu_row &row = rows[selected];
            if (row.kind == row_kind::connect)
            {
                if (!place_call(row.peer, now))
                {
                    notice(device_name(row.peer), "not reachable", now);
                    draw_menu_notice(now);
                    refused_beep();
                }
            }
            else
            {
                page = row.kind == row_kind::volume ? screen::volume :
                    screen::status;
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
        else if (id == button_id::cancel || id == button_id::confirm)
        {
            page = screen::menu;
        }
        break;
    case screen::status:
        if (id == button_id::cancel || id == button_id::confirm)
        {
            page = screen::menu;
        }
        break;
    case screen::connecting:
        if (id == button_id::cancel)
        {
            calls.hang_up(now);
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
            calls.answer(now);
        }
        else if (id == button_id::cancel)
        {
            calls.decline(now);
        }
        break;
    case screen::connected:
        if (id == button_id::confirm)
        {
            ptt_down_from_ui(now);
        }
        else if (id == button_id::up || id == button_id::right)
        {
            change_volume(1);
        }
        else if (id == button_id::down || id == button_id::left)
        {
            change_volume(-1);
        }
        else if (id == button_id::cancel)
        {
            ptt_up_from_ui();
            calls.hang_up(now);
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
        for (uint8_t i = 0; i < row_count; ++i)
        {
            const menu_row &row = rows[i];
            const bool up = row.kind == row_kind::connect &&
                peer_online(row.peer, now);
            if (row.kind == row_kind::connect && up != ui_reachable[row.peer])
            {
                ui_reachable[row.peer] = up;
                draw_menu_row(i);
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
    case screen::connecting:
        if ((now - calls.since_ms()) / 1000 != ui_drawn_elapsed)
        {
            draw_elapsed("Waiting", 8, 140, 2, now);
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
        if ((now - calls.since_ms()) / 1000 != ui_drawn_elapsed)
        {
            draw_elapsed("", 160, 48, 1, now);
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
    row_count = 0;
    for (uint8_t id = 1; id <= max_id && row_count < max_rows - 2; ++id)
    {
        if (valid_peer(id))
        {
            rows[row_count++] = {row_kind::connect, id};
        }
    }
    rows[row_count++] = {row_kind::volume, 0};
    rows[row_count++] = {row_kind::status, 0};

    // Set the level before enabling the output so BLK never glitches low.
    digitalWrite(app_config::lcd_backlight_pin, HIGH);
    pinMode(app_config::lcd_backlight_pin, OUTPUT);
    SPI.begin(app_config::lcd_sclk_pin, -1, app_config::lcd_mosi_pin,
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
    draw_hints("See the serial log", "Serial: reboot");
}

void service_ui(uint32_t now)
{
    poll_buttons(now);
    if (calls.end_count() != ui_shown_ends)
    {
        ui_shown_ends = calls.end_count();
        ui_end_pending = true;
        ui_end_shown_ms = now;
    }
    if (ui_ptt_held &&
        (tx == tx_phase::idle || calls.current() != call::state::connected))
    {
        ui_ptt_held = false;  // The stream ended by itself or was cut.
    }
    const screen next = wanted_screen(now);
    if (next != shown)
    {
        show(next, now);
        return;
    }
    refresh(now);
}
