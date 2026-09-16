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
#include "sd_dictionary.h"
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
    EXPORT,
    IMPORT_MERGE,
    IMPORT_REPLACE,
};

static lv_obj_t *s_overlay = nullptr;
static lv_obj_t *s_field_box[(size_t)editor_field_t::COUNT] = {};
static lv_obj_t *s_field_label[(size_t)editor_field_t::COUNT] = {};
static lv_obj_t *s_preedit_label = nullptr;
static lv_obj_t *s_candidate_preview_label = nullptr;
static lv_obj_t *s_status_label = nullptr;
static ime_skk_t *s_ime = nullptr;
static editor_field_t s_focus = editor_field_t::READING;
static std::string s_reading;
static char s_okuri = '\0';
static std::string s_candidate;
static bool s_replace_import_pending = false;
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

static void build_candidate_preview(const ime_skk_t &ime, char *out, size_t out_size)
{
    if (out == nullptr || out_size == 0) return;
    out[0] = '\0';
    size_t used = 0;
    for (size_t i = 0; i < ime.candidate_count(); ++i) {
        const std::string &candidate = ime.candidate_at(i);
        const int written = snprintf(out + used, out_size - used, "%s%s%s",
                                     i == ime.candidate_index() ? "[" : " ",
                                     candidate.c_str(),
                                     i == ime.candidate_index() ? "]" : " ");
        if (written < 0 || (size_t)written >= out_size - used) break;
        used += (size_t)written;
    }
}

static void update_ime_preview_locked()
{
    if (s_preedit_label == nullptr || s_candidate_preview_label == nullptr || s_ime == nullptr) return;

    char preedit[512] = {};
    char candidates[512] = {};
    const char *destination = field_name(s_focus);
    if (s_focus == editor_field_t::OKURI) {
        snprintf(preedit, sizeof(preedit), "Okuri: one ASCII initial (a-z), or leave blank");
    } else if (s_ime->state() == ime_state_t::IDLE) {
        snprintf(preedit, sizeof(preedit), "IME -> %s: compose the value, then Ctrl+J stores it", destination);
    } else {
        snprintf(preedit, sizeof(preedit), "IME -> %s: %s", destination,
                 s_ime->preedit_text().c_str());
    }

    if (s_ime->state() == ime_state_t::CANDIDATE && s_ime->candidate_count() > 0) {
        build_candidate_preview(*s_ime, candidates, sizeof(candidates));
        char text[560] = {};
        snprintf(text, sizeof(text), "Candidates: %s   Ctrl+J stores selected candidate", candidates);
        lv_label_set_text(s_candidate_preview_label, text);
    } else if (s_focus == editor_field_t::OKURI) {
        lv_label_set_text(s_candidate_preview_label, "Okuri is optional; it is the first letter of the inflection");
    } else if (s_focus == editor_field_t::READING) {
        lv_label_set_text(s_candidate_preview_label, "Reading must be hiragana (example: き)");
    } else {
        lv_label_set_text(s_candidate_preview_label,
                          "Candidate is the kanji/word stem (example: 来); Space searches candidates");
    }

    lv_obj_set_style_text_font(s_preedit_label, active_font(), 0);
    lv_obj_set_style_text_font(s_candidate_preview_label, active_font(), 0);
    lv_label_set_text(s_preedit_label, preedit);
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
    update_ime_preview_locked();
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

static void set_transfer_status(const char *operation, const dictionary_transfer_result_t &result)
{
    if (result.status == dictionary_transfer_status_t::OK) {
        snprintf(s_status, sizeof(s_status), "%s: %u entries", operation,
                 (unsigned)result.resulting_entries);
        return;
    }
    if (result.status == dictionary_transfer_status_t::TARGET_UNAVAILABLE) {
        if (result.stage == dictionary_transfer_stage_t::SD_MOUNT) {
            snprintf(s_status, sizeof(s_status), "SD mount failed (err=0x%X)",
                     (unsigned)result.system_errno);
        } else {
            set_status("User dictionary storage unavailable: reflash updated partition table");
        }
        return;
    }
    if (result.system_errno != 0) {
        snprintf(s_status, sizeof(s_status), "%s failed: %s (errno=%d)", operation,
                 dictionary_transfer_stage_text(result.stage), result.system_errno);
    } else {
        snprintf(s_status, sizeof(s_status), "%s failed: %s", operation,
                 dictionary_transfer_status_text(result.status));
    }
}

static void apply_action(editor_action_t action)
{
    if (!is_editor_ready()) return;

    bool ok = false;
    if (action == editor_action_t::EXPORT) {
        s_replace_import_pending = false;
        if (!s_ime->user_dictionary_writable()) {
            set_status("User dictionary storage unavailable: reflash updated partition table");
        } else {
            set_transfer_status("Export", sd_dictionary_export_user(s_ime->user_dictionary_path().c_str()));
        }
    } else if (action == editor_action_t::IMPORT_MERGE) {
        s_replace_import_pending = false;
        if (!s_ime->user_dictionary_writable()) {
            set_status("User dictionary storage unavailable: reflash updated partition table");
        } else {
            set_transfer_status("Import merge", sd_dictionary_import_user(
                s_ime->user_dictionary_path().c_str(), dictionary_transfer_mode_t::MERGE));
        }
    } else if (action == editor_action_t::IMPORT_REPLACE) {
        if (!s_replace_import_pending) {
            s_replace_import_pending = true;
            set_status("Replace pending: press Replace again (or Ctrl+R) to confirm");
        } else if (!s_ime->user_dictionary_writable()) {
            s_replace_import_pending = false;
            set_status("User dictionary storage unavailable: reflash updated partition table");
        } else {
            s_replace_import_pending = false;
            set_transfer_status("Import replace", sd_dictionary_import_user(
                s_ime->user_dictionary_path().c_str(), dictionary_transfer_mode_t::REPLACE));
        }
    } else if (!s_ime->user_dictionary_writable()) {
        s_replace_import_pending = false;
        set_status("User dictionary storage unavailable: reflash updated partition table");
    } else if (action == editor_action_t::ADD) {
        s_replace_import_pending = false;
        ok = s_ime->register_user_candidate(s_reading, s_okuri, s_candidate);
        set_status(ok ? "Saved to user dictionary"
                      : "Cannot add: Reading/Candidate required; Okuri must be a-z");
    } else {
        s_replace_import_pending = false;
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
    lv_label_set_text(help, "Tab: field  Ctrl+J: set  Ctrl+S: save  Ctrl+X: delete");
    lv_obj_set_style_text_font(help, &lv_font_unscii_16, 0);
    lv_obj_set_style_text_color(help, lv_color_make(180, 205, 235), 0);
    lv_obj_set_pos(help, 90, 60);

    lv_obj_t *help2 = lv_label_create(s_overlay);
    lv_label_set_text(help2, "Ctrl+E: export SD  Ctrl+I: import merge  Ctrl+R: replace  Esc: close");
    lv_obj_set_style_text_font(help2, &lv_font_unscii_16, 0);
    lv_obj_set_style_text_color(help2, lv_color_make(180, 205, 235), 0);
    lv_obj_set_pos(help2, 90, 82);

    create_field(s_overlay, editor_field_t::READING, 116);
    create_field(s_overlay, editor_field_t::OKURI, 184);
    create_field(s_overlay, editor_field_t::CANDIDATE, 252);

    lv_obj_t *add_button = lv_button_create(s_overlay);
    lv_obj_set_size(add_button, 220, 54);
    lv_obj_set_pos(add_button, 410, 330);
    lv_obj_set_style_bg_color(add_button, lv_color_make(0, 125, 60), 0);
    lv_obj_set_style_bg_color(add_button, lv_color_make(0, 175, 80), LV_STATE_PRESSED);
    lv_obj_add_event_cb(add_button, action_event_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)editor_action_t::ADD);
    lv_obj_t *add_label = lv_label_create(add_button);
    lv_label_set_text(add_label, "Add / Promote");
    lv_obj_set_style_text_font(add_label, &lv_font_unscii_16, 0);
    lv_obj_set_style_text_color(add_label, lv_color_white(), 0);
    lv_obj_center(add_label);

    lv_obj_t *remove_button = lv_button_create(s_overlay);
    lv_obj_set_size(remove_button, 220, 54);
    lv_obj_set_pos(remove_button, 650, 330);
    lv_obj_set_style_bg_color(remove_button, lv_color_make(145, 50, 50), 0);
    lv_obj_set_style_bg_color(remove_button, lv_color_make(195, 70, 70), LV_STATE_PRESSED);
    lv_obj_add_event_cb(remove_button, action_event_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)editor_action_t::REMOVE);
    lv_obj_t *remove_label = lv_label_create(remove_button);
    lv_label_set_text(remove_label, "Delete Exact");
    lv_obj_set_style_text_font(remove_label, &lv_font_unscii_16, 0);
    lv_obj_set_style_text_color(remove_label, lv_color_white(), 0);
    lv_obj_center(remove_label);

    s_preedit_label = lv_label_create(s_overlay);
    lv_obj_set_size(s_preedit_label, 1100, 40);
    lv_obj_set_pos(s_preedit_label, 90, 406);
    lv_label_set_long_mode(s_preedit_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(s_preedit_label, lv_color_make(215, 240, 255), 0);
    lv_obj_set_style_bg_color(s_preedit_label, lv_color_make(35, 65, 105), 0);
    lv_obj_set_style_bg_opa(s_preedit_label, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_left(s_preedit_label, 6, 0);
    lv_obj_set_style_pad_right(s_preedit_label, 6, 0);

    s_candidate_preview_label = lv_label_create(s_overlay);
    lv_obj_set_size(s_candidate_preview_label, 1100, 38);
    lv_obj_set_pos(s_candidate_preview_label, 90, 454);
    lv_label_set_long_mode(s_candidate_preview_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(s_candidate_preview_label, lv_color_make(235, 235, 235), 0);

    s_status_label = lv_label_create(s_overlay);
    lv_obj_set_size(s_status_label, 1100, 38);
    lv_obj_set_pos(s_status_label, 90, 500);
    lv_label_set_long_mode(s_status_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(s_status_label, active_font(), 0);
    lv_obj_set_style_text_color(s_status_label, lv_color_make(255, 225, 145), 0);

    lv_obj_t *export_button = lv_button_create(s_overlay);
    lv_obj_set_size(export_button, 220, 46);
    lv_obj_set_pos(export_button, 210, 550);
    lv_obj_set_style_bg_color(export_button, lv_color_make(55, 95, 150), 0);
    lv_obj_add_event_cb(export_button, action_event_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)editor_action_t::EXPORT);
    lv_obj_t *export_label = lv_label_create(export_button);
    lv_label_set_text(export_label, "Export SD");
    lv_obj_set_style_text_font(export_label, &lv_font_unscii_16, 0);
    lv_obj_set_style_text_color(export_label, lv_color_white(), 0);
    lv_obj_center(export_label);

    lv_obj_t *merge_button = lv_button_create(s_overlay);
    lv_obj_set_size(merge_button, 220, 46);
    lv_obj_set_pos(merge_button, 530, 550);
    lv_obj_set_style_bg_color(merge_button, lv_color_make(0, 110, 105), 0);
    lv_obj_add_event_cb(merge_button, action_event_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)editor_action_t::IMPORT_MERGE);
    lv_obj_t *merge_label = lv_label_create(merge_button);
    lv_label_set_text(merge_label, "Import Merge");
    lv_obj_set_style_text_font(merge_label, &lv_font_unscii_16, 0);
    lv_obj_set_style_text_color(merge_label, lv_color_white(), 0);
    lv_obj_center(merge_label);

    lv_obj_t *replace_button = lv_button_create(s_overlay);
    lv_obj_set_size(replace_button, 220, 46);
    lv_obj_set_pos(replace_button, 850, 550);
    lv_obj_set_style_bg_color(replace_button, lv_color_make(145, 80, 35), 0);
    lv_obj_add_event_cb(replace_button, action_event_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)editor_action_t::IMPORT_REPLACE);
    lv_obj_t *replace_label = lv_label_create(replace_button);
    lv_label_set_text(replace_label, "Import Replace");
    lv_obj_set_style_text_font(replace_label, &lv_font_unscii_16, 0);
    lv_obj_set_style_text_color(replace_label, lv_color_white(), 0);
    lv_obj_center(replace_label);
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
    s_replace_import_pending = false;
    set_status("Ready: select a field, then enter its value");
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
    s_preedit_label = nullptr;
    s_candidate_preview_label = nullptr;
    s_status_label = nullptr;
    s_ime = nullptr;
    s_replace_import_pending = false;
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

bool dictionary_ui_export_to_sd(void)
{
    if (!is_editor_ready()) return false;
    lvgl_port_lock(0);
    apply_action(editor_action_t::EXPORT);
    lvgl_port_unlock();
    return true;
}

bool dictionary_ui_import_merge_from_sd(void)
{
    if (!is_editor_ready()) return false;
    lvgl_port_lock(0);
    apply_action(editor_action_t::IMPORT_MERGE);
    lvgl_port_unlock();
    return true;
}

bool dictionary_ui_import_replace_from_sd(void)
{
    if (!is_editor_ready()) return false;
    lvgl_port_lock(0);
    apply_action(editor_action_t::IMPORT_REPLACE);
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
