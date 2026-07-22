# M5Stack TAB5 Serial Terminal

M5Stack TAB5 (ESP32-P4) 向けの VT100 互換スタンドアロンシリアルターミナルです。USB ホスト機能を利用して USB シリアルデバイスと双方向通信を行います。

## 特徴

- **USB ホスト CDC-ACM 対応** — VCP チップ（CH34x / CP210x / FTDI）および標準 CDC-ACM デバイス（Raspberry Pi USB ガジェット等）に対応
- **VT100 ターミナルエミュレーション** — カーソル移動・スクロール領域・画面消去・SGR カラー（8色 + 輝度、256色、Truecolor 近似）
- **pending wrap（遅延折り返し）** — VT100 仕様に準拠した行末処理。余分なスクロールを防止
- **DSR / DA 応答** — `ESC[6n`（カーソル位置報告要求）および `ESC[c`（デバイス属性要求）に応答。bash/readline がブロックしない
- **USB RX 16 KB リングバッファ** — 大量出力時のデータ取りこぼしを防止
- **ボーレート切り替え** — Ctrl+B で 9600 / 19200 / 38400 / 57600 / 115200 / 230400 / 460800 / 921600 bps を順番に切り替え
- **ウィンドウサイズ通知** — 接続時に `ESC[8;<rows>;<cols>t` を送信し、リモートシェルの `stty size` を自動設定
- **差分描画** — 変更行のみ再描画する行単位ダーティフラグで高速表示

## ハードウェア構成

| コンポーネント | 仕様 |
|:---|:---|
| メインボード | M5Stack TAB5 (ESP32-P4 rev1.0) |
| ディスプレイ | 5インチ IPS TFT 1280×720 (MIPI-DSI) |
| キーボード | TAB5 Keyboard (I2C 0x6D, SDA=GPIO0, SCL=GPIO1, INT=GPIO50) |
| USB ホスト | USB Type-A ポート (USB 2.0 High-Speed) |

## ターミナル仕様

| 項目 | 値 |
|:---|:---|
| 表示列数 | 80 列 |
| 表示行数 | 43 行（ステータスバー除く） |
| フォント | lv_font_unscii_16（16×16 px 等幅） |
| カラー | 16色（ANSI 8色 + 輝度ビット）、256色近似、Truecolor 近似 |

## キーバインド

### ローカル操作（Tab5 上で処理）

| キー | 動作 |
|:---|:---|
| Ctrl+C | 画面クリア |
| Ctrl+L | 画面強制再描画 |
| Ctrl+B | ボーレートを順番に切り替え（VCP 接続時のみ有効） |

### リモートへの送信（USB 経由でそのまま転送）

| キー | 送信シーケンス |
|:---|:---|
| Enter | `\r` (CR) |
| Backspace | `0x7F` (DEL) |
| Tab | `\t` |
| 矢印キー（上/下/左/右） | `ESC[A` / `ESC[B` / `ESC[C` / `ESC[D` |
| Home / End | `ESC[H` / `ESC[F` |
| Page Up / Page Down | `ESC[5~` / `ESC[6~` |
| Insert | `ESC[2~` |
| Delete / Del | `ESC[3~` |
| F1〜F4 | `ESC O P` 〜 `ESC O S` |
| F5〜F12 | `ESC[15~` 〜 `ESC[24~` |
| Escape / Esc | `ESC` (0x1B) |
| Ctrl+[A-Z] | 対応する制御文字 (0x01〜0x1A) |

## 対応 VT100 / ANSI シーケンス

### CSI シーケンス（ESC [ ... ）

| シーケンス | 機能 |
|:---|:---|
| `CUU / CUD / CUF / CUB` (A/B/C/D) | カーソル上下左右移動 |
| `CNL / CPL` (E/F) | カーソル次行/前行 |
| `CHA` (G) | カーソル水平絶対位置 |
| `CUP / HVP` (H/f) | カーソル位置指定 |
| `VPA` (d) | カーソル垂直絶対位置 |
| `ED` (J) | 画面消去（0=カーソル以降、1=カーソル以前、2=全画面） |
| `EL` (K) | 行消去（0=カーソル以降、1=カーソル以前、2=全行） |
| `IL / DL` (L/M) | 行挿入 / 行削除 |
| `DCH` (P) | 文字削除 |
| `ECH` (X) | 文字消去 |
| `SU / SD` (S/T) | スクロールアップ / スクロールダウン |
| `DECSTBM` (r) | スクロール領域設定 |
| `SGR` (m) | 文字属性（色・輝度） |
| `DSR` (n) | デバイス状態報告（param=5: 状態、param=6: カーソル位置） |
| `DA` (c) | デバイス属性（VT100 として応答） |
| `SCP / RCP` (s/u) | カーソル位置保存 / 復元 |
| `DECTCEM` (?25h/l) | カーソル表示 / 非表示 |

### ESC 単独シーケンス

| シーケンス | 機能 |
|:---|:---|
| `ESC 7 / ESC 8` | カーソル位置保存 / 復元 |
| `ESC D` | Index（カーソル下移動、必要に応じてスクロール） |
| `ESC M` | Reverse Index（カーソル上移動、必要に応じてスクロール） |
| `ESC E` | Next Line |
| `ESC c` | Full Reset (RIS) |
| `ESC # 8` | DECALN（画面テスト用 E 文字フィル） |
| `ESC ( ) * +` | 文字セット指定（無視） |

## ビルド方法

### 前提条件

- ESP-IDF v5.3 以上
- ESP32-P4 ターゲットのサポート

### ビルド手順

```bash
# ESP-IDF 環境のセットアップ
. $HOME/esp/esp-idf/export.sh

# ターゲット設定
idf.py set-target esp32p4

# ビルド
idf.py build

# フラッシュ＆モニタ
idf.py -p /dev/ttyACM0 flash monitor
```

### 依存コンポーネント

| コンポーネント | バージョン |
|:---|:---|
| `espressif/esp_lvgl_port` | ^2.7.0 |
| `espressif/usb_host_cdc_acm` | ^2.3 |
| `espressif/usb_host_vcp` | * |
| `espressif/usb_host_ch34x_vcp` | ^2.2 |
| `espressif/usb_host_cp210x_vcp` | ^2.2 |
| `espressif/usb_host_ftdi_vcp` | ^2.1 |

## Raspberry Pi との接続

Raspberry Pi を USB シリアルガジェット（`g_serial`）として使用する場合、以下の設定が必要です。

### ラズパイ側の設定

```bash
# /boot/firmware/config.txt に追記
dtoverlay=dwc2

# /etc/modules に追記
dwc2
g_serial use_acm=1
```

### 接続時の環境変数設定

```bash
# .bashrc 等に追記
export TERM=xterm-color
```

> **注意:** `TERM=vt220` では色が表示されません。`TERM=xterm-color` を推奨します。

### ウィンドウサイズについて

接続時に `ESC[8;43;80t` を自動送信しますが、`g_serial` ガジェットはこのシーケンスに応答しません。必要に応じてラズパイ側で手動設定してください。

```bash
stty rows 43 cols 80
```

## ESP-IDF ライブラリへのパッチ

Raspberry Pi の `g_serial` ガジェットは USB コンフィギュレーション #2 を使用します。ESP-IDF の USB ホストライブラリのデフォルト動作では正しく認識されないため、`enum.c.diff` に記載のパッチを適用する必要があります。

```bash
# ESP-IDF のソースに適用
patch -p0 < enum.c.diff
```

## アーキテクチャ

```
main_task (メインループ)
  ├── screen_log_queue  ← 内部メッセージ表示
  ├── usb_rx_ringbuf    ← USB RX データ（16 KB リングバッファ）
  │     └── vt100_process_byte() → term_buffer → term_refresh_display()
  └── key_queue         ← キーボード入力
        └── USB TX (s_vcp_dev->tx_blocking)

vcp_task
  ├── usb_lib_task      ← USB ホストライブラリ常駐タスク
  └── cdc_acm_host      ← CDC-ACM ドライバ
        └── usb_rx_cb() → usb_rx_ringbuf への書き込み

keyboard_event_cb() → key_queue への書き込み
```

## 既知の制限・今後の予定

- **UTF-8 / 日本語表示** — 現在は ASCII のみ。マルチバイト文字は未対応
- **UART 対応** — USB のみ。Port A (UART) 経由の接続は未実装
- **設定画面** — ボーレート等の設定は Ctrl+B のみ。GUI 設定画面は未実装
- **スクロールバック** — 画面外にスクロールしたデータは参照不可
- **起動時のまれなハング** — USB ホスト初期化中に稀に停止することがある（調査中）

## ライセンス

MIT License
