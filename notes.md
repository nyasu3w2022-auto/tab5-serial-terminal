# TAB5 Serial Terminal - 調査メモ

## LVGL座標系
- 物理ディスプレイ: 720x1280 (縦長)
- LV_DISPLAY_ROTATION_90 + PPA使用
- LVGL論理座標: 1280x720 (横長)
- TERM_COLS = 80, TERM_ROWS = 43, LVGL_W = 1280, LVGL_H = 720

## str_modifier ビット定義（M5Tab5 Keyboard STRINGモード）
| 値 | 意味 |
|----|------|
| 0  | 通常（修飾キーなし） |
| 1  | Ctrl |
| 4  | Alt |
| 5  | Ctrl+Alt |

## キーボードから送られるキー名
- ESCキー: "esc"（"escape"ではない）
- 矢印キー: "up", "down", "left", "right"
- 通常文字: 小文字で送られる（例: "c", "l", "b"）

## LVGL v9 lv_color_t 定義
- `lv_color_t` は常に RGB888 構造体: `uint8_t blue, green, red`
- `lv_color_to_u16()`: ((r & 0xF8) << 8) + ((g & 0xFC) << 3) + ((b & 0xF8) >> 3)

## lv_font_unscii_16 フォーマット
- bpp = 1 (A1形式: 1ビット/ピクセル、MSBファースト)
- bitmap_format = 0

## lv_font_glyph_dsc_t フィールド
- box_w, box_h: バウンディングボックス
- ofs_x, ofs_y: オフセット (ofs_y: ベースラインからの上方向オフセット)
- format: LV_FONT_GLYPH_FORMAT_A1 = 0x01

## 色表示の解決策（最終）
- lv_canvas + lv_draw_label() 方式は動作不安定（デッドロック懸念）
- RGB565バッファに直接ピクセル書き込み方式に変更
- lv_font_get_glyph_dsc() + lv_font_get_glyph_bitmap() でフォントビットマップ取得
- lv_color_to_u16() でRGB888→RGB565変換
- lv_obj_invalidate() で再描画トリガー

## lv_label_set_recolor
- LVGL v9では廃止（存在しない）

## ウィンドウサイズ通知
- ESC[8;<rows>;<cols>t でxterm window resizeシーケンスを送信
- ラズパイ側でTIOCSWINSZが更新される
