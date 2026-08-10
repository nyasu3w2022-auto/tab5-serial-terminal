# M5Stack TAB5 Serial Terminal

M5Stack TAB5 (ESP32-P4) 向けの VT100 互換スタンドアロンシリアルターミナルです。USB ホスト機能を利用して USB シリアルデバイスと双方向通信を行います。

## 特徴

- **USB ホスト CDC-ACM 対応** — VCP チップ（CH34x / CP210x / FTDI）および標準 CDC-ACM デバイス（Raspberry Pi USB ガジェット等）に対応
- **VT100 ターミナルエミュレーション** — カーソル移動・スクロール領域・画面消去・SGR カラー（8色 + 輝度、256色、Truecolor 近似）
- **UTF-8 日本語表示** — IPAゴシックフォントを内蔵し、UTF-8 マルチバイト文字（ひらがな、カタカナ、漢字等）の表示に対応
- **フォントサイズ切り替え** — 設定画面から「Small (16px, 160×43)」と「Large (28px, 91×25)」を切り替え可能
- **pending wrap（遅延折り返し）** — VT100 仕様に準拠した行末処理。余分なスクロールを防止
- **DSR / DA 応答** — `ESC[6n`（カーソル位置報告要求）および `ESC[c`（デバイス属性要求）に応答。bash/readline がブロックしない
- **USB RX 16 KB リングバッファ** — 大量出力時のデータ取りこぼしを防止
- **GUI 設定画面** — `Ctrl+Alt+S` で設定画面を開き、ボーレート、ログレベル、フォントサイズを変更可能（NVSに自動保存）
- **差分描画** — 変更行のみ再描画する行単位ダーティフラグで高速表示

## ハードウェア構成

| コンポーネント | 仕様 |
|:---|:---|
| メインボード | M5Stack TAB5 (ESP32-P4 rev1.0) |
| ディスプレイ | 5インチ IPS TFT 1280×720 (MIPI-DSI) |
| キーボード | TAB5 Keyboard (I2C 0x6D, SDA=GPIO0, SCL=GPIO1, INT=GPIO50) |
| USB ホスト | USB Type-A ポート (USB 2.0 High-Speed) |

## ターミナル仕様

| 項目 | Small フォント | Large フォント（デフォルト） |
|:---|:---|:---|
| 表示列数 | 160 列 | 91 列 |
| 表示行数 | 43 行 | 25 行 |
| フォントサイズ | 16 px | 28 px |
| フォント種別 | IPAゴシック (lv_font_cjk_16) | IPAゴシック (lv_font_cjk_28) |
| カラー | \multicolumn{2}{l|}{16色（ANSI 8色 + 輝度ビット）、256色近似、Truecolor 近似} |

## キーバインド

### ローカル操作（Tab5 上で処理）

| キー | 動作 |
|:---|:---|
| Ctrl+C | 画面クリア |
| Ctrl+L | 画面強制再描画 |
| Ctrl+Alt+S | 設定画面を開く / 閉じる |

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

**注意:** CJKフォントデータ（約750KB）を格納するため、カスタムパーティションテーブル（factory 3MB）を使用しています。初めてフラッシュする際は、NVS領域も含めて初期化するため、必ずフルフラッシュ（または `idf.py erase-flash` 後にフラッシュ）を行ってください。

### 依存コンポーネント

| コンポーネント | バージョン |
|:---|:---|
| `espressif/esp_lvgl_port` | ^2.7.0 |
| `espressif/usb_host_cdc_acm` | ^2.3 |
| `espressif/usb_host_vcp` | * |
| `espressif/usb_host_ch34x_vcp` | ^2.2 |
| `espressif/usb_host_cp210x_vcp` | ^2.2 |
| `espressif/usb_host_ftdi_vcp` | ^2.1 |

## ESP-IDF ライブラリへのパッチ

Raspberry Pi の `g_serial` ガジェットは USB コンフィギュレーション #2 を使用します。ESP-IDF の USB ホストライブラリはデフォルトでコンフィギュレーション #1 を選択するため、そのままでは認識されません。リポジトリに含まれる `enum.c.diff` を適用する必要があります。

### パッチ対象ファイル

```
{ESP-IDF インストールディレクトリ}/components/usb/host/enum.c
```

Windows（ESP-IDF Tools Installer）の場合の典型的なパス：
```
C:\Espressif\frameworks\esp-idf-v5.x.x\components\usb\host\enum.c
```

### 適用方法

```bash
# ESP-IDF のソースディレクトリに移動して適用
cd C:\Espressif\frameworks\esp-idf-v5.x.x
patch -p0 < path/to/tab5-serial-terminal/enum.c.diff
```

**注意:** このパッチは ESP-IDF のシステムファイルを変更します。ESP-IDF をアップデートした場合は再度適用が必要です。なお、一般的な USB シリアルデバイス（CH34x、CP210x、FTDI 等）はコンフィギュレーション #1 を使用するため、このパッチの影響を受けません。

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

ターミナル側のフォントサイズに応じて、ラズパイ側でウィンドウサイズを手動設定する必要があります。

```bash
# Large フォント (28px) の場合
stty rows 25 cols 91

# Small フォント (16px) の場合
stty rows 43 cols 160
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

- **UART 対応** — USB のみ。Port A (UART) 経由の接続は未実装
- **スクロールバック** — 画面外にスクロールしたデータは参照不可
- **起動時のまれなハング** — USB ホスト初期化中に稀に停止することがある（調査中）

## ライセンス

### プログラムコード
MIT License

### 組み込みフォント
本ソフトウェアは [IPAフォント](https://moji.or.jp/ipafont/) (IPAゴシック) をビットマップデータとして組み込んで使用しています。
IPAフォントのライセンスは `IPA_Font_License_Agreement_v1.0.txt` に従います。
