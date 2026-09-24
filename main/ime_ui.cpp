/*
 * ime_ui.cpp — LVGL overlay for TAB5 local Japanese input.
 *
 * SPDX-License-Identifier: MIT
 */

#include "ime_ui.h"

#include <stdio.h>
#include <string.h>

#include "display.h"
#include "lvgl.h"
#include "lvgl_port.h"
#include "terminal.h"

extern "C" const lv_font_t lv_font_cjk_16;
extern "C" const lv_font_t lv_font_cjk_28;

namespace {

static lv_obj_t *s_overlay = nullptr;
static lv_obj_t *s_status_label = nullptr;
static lv_obj_t *s_preedit_label = nullptr;
static lv_obj_t *s_candidate_label = nullptr;

static const lv_font_t *active_font()
{
    return (TERM_FONT_H <= 16) ? &lv_font_cjk_16 : &lv_font_cjk_28;
}

static void ensure_overlay()
{
    if (s_overlay != nullptr) return;

    lv_obj_t *screen = lv_scr_act();
    s_overlay = lv_obj_create(screen);
    lv_obj_set_size(s_overlay, LVGL_W, 92);
    lv_obj_set_pos(s_overlay, 0, LVGL_H - STATUS_BAR_H - 92);
    lv_obj_set_style_bg_color(s_overlay, lv_color_make(10, 18, 42), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_90, 0);
    lv_obj_set_style_border_color(s_overlay, lv_color_make(70, 150, 240), 0);
    lv_obj_set_style_border_width(s_overlay, 2, 0);
    lv_obj_set_style_radius(s_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_overlay, 4, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);

    s_status_label = lv_label_create(s_overlay);
    lv_obj_set_pos(s_status_label, 8, 2);
    lv_obj_set_style_text_font(s_status_label, &lv_font_unscii_16, 0);
    lv_obj_set_style_text_color(s_status_label, lv_color_make(130, 210, 255), 0);

    s_preedit_label = lv_label_create(s_overlay);
    lv_obj_set_pos(s_preedit_label, 8, 22);
    lv_obj_set_style_text_font(s_preedit_label, active_font(), 0);
    lv_obj_set_style_text_color(s_preedit_label, lv_color_white(), 0);
    lv_obj_set_style_bg_color(s_preedit_label, lv_color_make(50, 70, 120), 0);
    lv_obj_set_style_bg_opa(s_preedit_label, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_left(s_preedit_label, 4, 0);
    lv_obj_set_style_pad_right(s_preedit_label, 4, 0);
    lv_label_set_long_mode(s_preedit_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(s_preedit_label, LVGL_W - 16);

    s_candidate_label = lv_label_create(s_overlay);
    lv_obj_set_pos(s_candidate_label, 8, 56);
    lv_obj_set_style_text_font(s_candidate_label, active_font(), 0);
    lv_obj_set_style_text_color(s_candidate_label, lv_color_make(230, 230, 230), 0);
    lv_label_set_long_mode(s_candidate_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(s_candidate_label, LVGL_W - 16);
}

static void build_candidate_text(const ime_skk_t &ime, char *out, size_t out_size)
{
    if (out == nullptr || out_size == 0) return;
    out[0] = '\0';
    size_t used = 0;

    for (size_t i = 0; i < ime.candidate_count(); ++i) {
        const std::string &candidate = ime.candidate_at(i);
        int written = snprintf(out + used, out_size - used,
                               "%s%s%s",
                               (i == ime.candidate_index()) ? "[" : " ",
                               candidate.c_str(),
                               (i == ime.candidate_index()) ? "]" : " ");
        if (written < 0 || (size_t)written >= out_size - used) break;
        used += (size_t)written;
    }
}

}  // namespace

void ime_ui_update(bool japanese_mode, const ime_skk_t &ime)
{
    if (!japanese_mode || ime.state() == ime_state_t::IDLE) {
        ime_ui_hide();
        return;
    }

    lvgl_port_lock(0);
    ensure_overlay();

    const bool candidates = ime.state() == ime_state_t::CANDIDATE;
    const char *state_text = nullptr;
    if (candidates && ime.is_abbrev_mode()) {
        state_text = " SKK ABBREV: Space/Left/Right=Select  Enter/C-J=Commit  Esc=Cancel";
    } else if (candidates) {
        state_text = " SKK: Space/Left/Right=Select  Enter/C-J=Commit  Esc=Cancel";
    } else if (ime.is_ascii_mode()) {
        state_text = " SKK ASCII: direct key send  Esc=Kana";
    } else if (ime.is_abbrev_mode()) {
        state_text = " SKK ABBREV: ASCII  Space=Convert  Enter/C-J=Commit  Esc=Cancel";
    } else if (ime.is_conversion_active() && ime.is_okuri_active()) {
        state_text = " SKK OKURI: Space=Convert  Enter=Kana+CR  C-J=Kana  Esc=Cancel";
    } else if (ime.is_conversion_active()) {
        state_text = " SKK HENKAN: Space=Convert  Enter=Kana+CR  C-J=Kana  Esc=Cancel";
    } else if (ime.kana_mode() == ime_kana_mode_t::KATAKANA) {
        state_text = " SKK KATA: q=Hiragana Commit  /=Abbrev  C-J=Kana";
    } else {
        state_text = " SKK HIRA: q=Katakana Commit  /=Abbrev  C-J=Kana";
    }
    lv_label_set_text(s_status_label, state_text);
    lv_obj_set_style_text_font(s_preedit_label, active_font(), 0);
    lv_obj_set_style_text_font(s_candidate_label, active_font(), 0);
    lv_label_set_text(s_preedit_label, ime.preedit_text().c_str());

    char candidate_text[512] = {};
    if (candidates) build_candidate_text(ime, candidate_text, sizeof(candidate_text));
    lv_label_set_text(s_candidate_label, candidate_text);
    lv_obj_move_foreground(s_overlay);
    lvgl_port_unlock();
}

void ime_ui_hide(void)
{
    if (s_overlay == nullptr) return;

    lvgl_port_lock(0);
    lv_obj_delete(s_overlay);
    s_overlay = nullptr;
    s_status_label = nullptr;
    s_preedit_label = nullptr;
    s_candidate_label = nullptr;
    lvgl_port_unlock();
}
