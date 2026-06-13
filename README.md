# M5Stack TAB5 Serial Terminal

ESP-IDF ベースの VT100 互換シリアルターミナルプロジェクト（M5Stack TAB5 + キーボードアクセサリ向け）。

## 概要

本プロジェクトは、M5Stack TAB5 の USB ホスト機能を利用して USB シリアルデバイスと通信を行う、VT100 互換のスタンドアロンシリアルターミナルを実現するものです。

## 開発ステップ

### ステップ 1: ハードウェア確認（現在）

- TAB5 ボードの初期化（m5_tab5_component）
- LVGL による画面描画（等幅フォントでのテキスト表示）
- I2C 経由でのキーボード入力取得（Character/String モード）
- USB-A ポートの 5V 電源有効化

### ステップ 2: USB ホスト通信（予定）

- USB Host Library の初期化
- CDC-ACM / VCP（FTDI, CP210x, CH34x）ドライバの統合
- シリアルデータの送受信

### ステップ 3: VT100 エミュレーション（予定）

- ANSI エスケープシーケンスパーサーの実装
- カーソル移動、画面消去、文字属性の処理
- スクロール処理

### ステップ 4: 最適化・機能拡張（予定）

- 差分描画による高速化
- ボーレート設定 UI
- スクロールバック機能

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

# フラッシュ
idf.py -p /dev/ttyACM0 flash monitor
```

## ハードウェア構成

| コンポーネント | 仕様 |
|:---|:---|
| メインボード | M5Stack TAB5 (ESP32-P4) |
| ディスプレイ | 5インチ IPS TFT 1280x720 (MIPI-DSI) |
| キーボード | TAB5 Keyboard (I2C 0x6D, SDA=GPIO0, SCL=GPIO1, INT=GPIO50) |
| USB ホスト | USB Type-A ポート (USB 2.0 High-Speed) |

## キーバインド（ステップ 1）

| キー | 動作 |
|:---|:---|
| 通常キー | 画面に文字を表示 |
| Enter | 改行 |
| Backspace | 1文字削除 |
| Ctrl+C | 画面クリア |
| Ctrl+L | 画面再描画 |

## ライセンス

MIT License
