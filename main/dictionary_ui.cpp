/*
 * dictionary_ui.cpp — Local user-dictionary editor overlay for TAB5 SKK.
 *
 * SPDX-License-Identifier: MIT
 */

#include "dictionary_ui.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "display.h"
#include "lvgl.h"
#include "lvgl_port.h"
#include "terminal.h"

extern "C" const lv_font_t lv_font_cjk_16;
extern "C" const lv_font_t lv_font_cjk_28;

namespace {

enum class editor_field_t : uint8_t {
    READING = 0,
    OKURI,
    CANDIDATE,
    COUNT,
};

enum class editor_action_t : uint8_t {
    ADD = 0,
    REMOVE,
};

static lv_obj_t *s_overlay = nullptr;
static lv_obj_t *s_field_box[(size_t)editor_field_t::COUNT] = {};
static lv_obj_t *s_field_label[(size_t)editor_field_t::COUNT] = {};
static lv_obj_t *s_status_label = nullptr;
static ime_skk_t *s_ime = nullptr;
static editor_field_t s_focus = editor_field_t::READING;
static std::string s_reading;
static char s_okuri = '\0';
static std::string s_candidate;
static char s_status[192] = "Enter reading and candidate; Ctrl+S=Add, Ctrl+X=Delete";

static const lv_font_t *active_font()
{
    return (TERM_FONT_H <= 16) ? &lv_font_cjk_16 : &lv_font_cjk_28;
}

static const char *field_name(editor_field_t field)
{
    switch (field) {
    case editor_field_t::READING: return "Reading (hiragana)";
    case editor_field_t::OKURI: return "Okuri initial (a-z)";
    case editor_field_t::CANDIDATE: return "Candidate";
    default: return "";
    }
}

static std::string field_value(editor_field_t field)
{
    switch (field) {
    case editor_field_t::READING: return s_reading;
    case editor_field_t::OKURI: return s_okuri == '\0' ? "" : std::string(1, s_okuri);
    case editor_field_t::CANDIDATE: return s_candidate;
    default: return "";
    }
}

static void set_status(const char *text)
{
    snprintf(s_status, sizeof(s_status), "%s", text ? text : "");
}

static void update_editor_locked()
{
    if (s_overlay == nullptr) return;

    for (size_t i = 0; i < (size_t)editor_field_t::COUNT; ++i) {
        editor_field_t field = (editor_field_t)i;
        std::string value = field_value(field);
        char text[256] = {};
        snprintf(text, sizeof(text), "%s: %s", field_name(field),
                 value.empty() ? "(empty)" : value.c_str());
        lv_label_set_text(s_field_label[i], text);
        lv_obj_set_style_border_color(
            s_field_box[i],
            field == s_focus ? lv_color_make(80, 180, 255) : lv_color_make(80, 80, 120), 0);
        lv_obj_set_style_bg_color(
            s_field_box[i],
            field == s_focus ? lv_color_make(35, 65, 105) : lv_color_make(28, 28, 45), 0);
    }
    lv_label_set_text(s_status_label, s_status);
    lv_obj_move_foreground(s_overlay);
}

static bool is_editor_ready()
{
    return s_overlay != nullptr && s_ime != nullptr;
}

static void set_focus(editor_field_t field)
{
    s_focus = field;
    update_editor_locked();
}

static void focus_event_cb(lv_event_t *event)
{
    const uintptr_t raw = (uintptr_t)lv_event_get_user_data(event);
    set_focus((editor_field_t)raw);
}

static void apply_action(editor_action_t action)
{
    if (!is_editor_ready()) return;

    bool ok = false;
    if (action == editor_action_t::ADD) {
        ok = s_ime->register_user_candidate(s_reading, s_okuri, s_candidate);
        set_status(ok ? "Saved to user dictionary" : "Cannot add: check Reading, Okuri, Candidate");
    } else {
        ok = s_ime->remove_user_candidate(s_reading, s_okuri, s_candidate);
        set_status(ok ? "Removed from user dictionary" : "Candidate was not in user dictionary");
    }
    update_editor_locked();
}

static void action_event_cb(lv_event_t *event)
{
    const uintptr_t raw = (uintptr_t)lv_event_get_user_data(event);
    apply_action((editor_action_t)raw);
}

static lv_obj_t *create_field(lv_obj_t *parent, editor_field_t field, int y)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_size(box, 920, 64);
    lv_obj_set_pos(box, 180, y);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_radius(box, 6, 0);
    lv_obj_set_style_pad_all(box, 10, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(box, focus_event_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)field);

    lv_obj_t *label = lv_label_create(box);
    lv_obj_set_size(label, 890, 44);
    lv_obj_set_pos(label, 0, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(label, active_font(), 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);

    s_field_box[(size_t)field] = box;
    s_field_label[(size_t)field] = label;
    return box;
}

static void ensure_editor_locked()
{
    if (s_overlay != nullptr) return;

    lv_obj_t *screen = lv_scr_act();
    s_overlay = lv_obj_create(screen);
    lv_obj_set_size(s_overlay, LVGL_W, LVGL_H - STATUS_BAR_H);
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, lv_color_make(18, 24, 42), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_overlay, 0, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_obj_create(s_overlay);
    lv_obj_set_size(title, LVGL_W, 48);
    lv_obj_set_pos(title, 0, 0);
    lv_obj_set_style_bg_color(title, lv_color_make(70, 50, 145), 0);
    lv_obj_set_style_bg_opa(title, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(title, 0, 0);
    lv_obj_set_style_pad_all(title, 0, 0);
    lv_obj_clear_flag(title, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title_label = lv_label_create(title);
    lv_label_set_text(title_label, "  TAB5 SKK Dictionary Editor  (Ctrl+Alt+D: close)");
    lv_obj_set_style_text_font(title_label, &lv_font_unscii_16, 0);
    lv_obj_set_style_text_color(title_label, lv_color_white(), 0);
    lv_obj_align(title_label, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *help = lv_label_create(s_overlay);
    lv_label_set_text(help,
        "Tab: next field   Ctrl+J: IME commit field   Ctrl+S: Add   Ctrl+X: Delete   Esc: cancel/close");
    lv_obj_set_style_text_font(help, &lv_font_unscii_16, 0);
    lv_obj_set_style_text_color(help, lv_color_make(180, 205, 235), 0);
    lv_obj_set_pos(help, 180, 70);

    create_field(s_overlay, editor_field_t::READING, 108);
    create_field(s_overlay, editor_field_t::OKURI, 188);
    create_field(s_overlay, editor_field_t::CANDIDATE, 268);

    lv_obj_t *add_button = lv_button_create(s_overlay);
    lv_obj_set_size(add_button, 250, 64);
    lv_obj_set_pos(add_button, 340, 370);
    lv_obj_set_style_bg_color(add_button, lv_color_make(0, 125, 60), 0);
    lv_obj_set_style_bg_color(add_button, lv_color_make(0, 175, 80), LV_STATE_PRESSED);
    lv_obj_add_event_cb(add_button, action_event_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)editor_action_t::ADD);
    lv_obj_t *add_label = lv_label_create(add_button);
    lv_label_set_text(add_label, "Ctrl+S  Add / Promote");
    lv_obj_set_style_text_font(add_label, &lv_font_unscii_16, 0);
    lv_obj_set_style_text_color(add_label, lv_color_white(), 0);
    lv_obj_center(add_label);

    lv_obj_t *remove_button = lv_button_create(s_overlay);
    lv_obj_set_size(remove_button, 250, 64);
    lv_obj_set_pos(remove_button, 690, 370);
    lv_obj_set_style_bg_color(remove_button, lv_color_make(145, 50, 50), 0);
    lv_obj_set_style_bg_color(remove_button, lv_color_make(195, 70, 70), LV_STATE_PRESSED);
    lv_obj_add_event_cb(remove_button, action_event_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)editor_action_t::REMOVE);
    lv_obj_t *remove_label = lv_label_create(remove_button);
    lv_label_set_text(remove_label, "Ctrl+X  Delete Exact");
    lv_obj_set_style_text_font(remove_label, &lv_font_unscii_16, 0);
    lv_obj_set_style_text_color(remove_label, lv_color_white(), 0);
    lv_obj_center(remove_label);

    s_status_label = lv_label_create(s_overlay);
    lv_obj_set_size(s_status_label, 1100, 56);
    lv_obj_set_pos(s_status_label, 90, 470);
    lv_label_set_long_mode(s_status_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(s_status_label, active_font(), 0);
    lv_obj_set_style_text_color(s_status_label, lv_color_make(255, 225, 145), 0);
}

static void erase_last_utf8(std::string *value)
{
    if (value == nullptr || value->empty()) return;
    size_t pos = value->size() - 1;
    while (pos > 0 && (((uint8_t)(*value)[pos] & 0xC0) == 0x80)) pos--;
    value->erase(pos);
}

}  // namespace

void dictionary_ui_open(ime_skk_t *ime)
{
    if (ime == nullptr) return;
    lvgl_port_lock(0);
    s_ime = ime;
    s_focus = editor_field_t::READING;
    s_reading.clear();
    s_okuri = '\0';
    s_candidate.clear();
    set_status("Enter reading and candidate; Ctrl+S=Add, Ctrl+X=Delete");
    ensure_editor_locked();
    update_editor_locked();
    lvgl_port_unlock();
}

void dictionary_ui_close(void)
{
    if (s_overlay == nullptr) return;
    lvgl_port_lock(0);
    lv_obj_delete(s_overlay);
    s_overlay = nullptr;
    memset(s_field_box, 0, sizeof(s_field_box));
    memset(s_field_label, 0, sizeof(s_field_label));
    s_status_label = nullptr;
    s_ime = nullptr;
    lvgl_port_unlock();

    term_mark_all_dirty();
    term_refresh_display();
    update_status_bar();
}

bool dictionary_ui_is_open(void)
{
    return s_overlay != nullptr;
}

bool dictionary_ui_add_or_promote(void)
{
    if (!is_editor_ready()) return false;
    lvgl_port_lock(0);
    apply_action(editor_action_t::ADD);
    lvgl_port_unlock();
    return true;
}

bool dictionary_ui_delete_exact(void)
{
    if (!is_editor_ready()) return false;
    lvgl_port_lock(0);
    apply_action(editor_action_t::REMOVE);
    lvgl_port_unlock();
    return true;
}

bool dictionary_ui_accept_raw_text(const char *text, size_t len)
{
    if (!is_editor_ready() || text == nullptr || len == 0 || s_focus != editor_field_t::OKURI) return false;
    const unsigned char c = (unsigned char)text[0];
    if (len != 1 || !std::isalpha(c) || c > 0x7f) {
        lvgl_port_lock(0);
        set_status("Okuri must be one ASCII letter (a-z)");
        update_editor_locked();
        lvgl_port_unlock();
        return true;
    }
    lvgl_port_lock(0);
    s_okuri = (char)std::tolower(c);
    set_status("Okuri initial updated");
    update_editor_locked();
    lvgl_port_unlock();
    return true;
}

bool dictionary_ui_accept_ime_commit(const std::string &text)
{
    if (!is_editor_ready() || text.empty() || s_focus == editor_field_t::OKURI) return false;
    lvgl_port_lock(0);
    if (s_focus == editor_field_t::READING) {
        s_reading += text;
    } else {
        s_candidate += text;
    }
    set_status("Committed local IME text into selected field");
    update_editor_locked();
    lvgl_port_unlock();
    return true;
}

bool dictionary_ui_handle_special_key(const char *name, const ime_skk_t &ime)
{
    if (!is_editor_ready() || name == nullptr) return false;

    if (strcasecmp(name, "tab") == 0) {
        lvgl_port_lock(0);
        const size_t next = ((size_t)s_focus + 1) % (size_t)editor_field_t::COUNT;
        set_focus((editor_field_t)next);
        lvgl_port_unlock();
        return true;
    }
    if (strcasecmp(name, "backspace") == 0) {
        if (ime.state() != ime_state_t::IDLE) return false;
        lvgl_port_lock(0);
        if (s_focus == editor_field_t::READING) erase_last_utf8(&s_reading);
        else if (s_focus == editor_field_t::OKURI) s_okuri = '\0';
        else erase_last_utf8(&s_candidate);
        set_status("Deleted last character from selected field");
        update_editor_locked();
        lvgl_port_unlock();
        return true;
    }
    if (strcasecmp(name, "escape") == 0 || strcasecmp(name, "esc") == 0) {
        if (ime.state() != ime_state_t::IDLE) return false;
        dictionary_ui_close();
        return true;
    }
    if (strcasecmp(name, "enter") == 0) {
        // Enter is never passed to the remote while editing a local dictionary.
        return true;
    }
    return false;
}

void dictionary_ui_update(const ime_skk_t &ime)
{
    if (!is_editor_ready()) return;
    lvgl_port_lock(0);
    if (ime.state() != ime_state_t::IDLE) {
        set_status("IME preedit active: Ctrl+J commits text to selected field");
    }
    update_editor_locked();
    lvgl_port_unlock();
}
