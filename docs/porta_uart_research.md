# M5Stack Tab5 Port A UART 調査メモ

調査日: 2026-08-13

## 確定したPort Aの信号

M5Stack公式のTab5ハードウェア資料では、HY2.0-4P Port Aの配線は次のとおりである。

| HY2.0-4P線色 | 信号 | ESP32-P4 GPIO | UARTでの役割 |
|:---|:---|---:|:---|
| 黒 | GND | — | 共通GND |
| 赤 | 5V | — | 外部電源（UART信号線ではない） |
| 黄 | Port A data | GPIO53 | Tab5 TX（接続先RXへ） |
| 白 | Port A data | GPIO54 | Tab5 RX（接続先TXから） |

GPIO53/54は標準ではユーザーI2Cバスとしても利用可能である。UARTとして使用する間は、同じPort A上のI2C機器を併用してはならない。

## 実装方針

- ESP-IDFのハードウェアUARTドライバを使用する。
- UART番号は `UART_NUM_1` を使用し、GPIO53をTX、GPIO54をRXに明示的に割り当てる。
- USBとUARTは同時に有効化せず、NVS設定 `serial_if` に応じて片方だけを起動する。
- ターミナルのRX処理は既存の16KBリングバッファへ統合し、既存VT100パーサーと描画処理を共用する。
- UART選択時の接続状態は物理的な検出ができないため、ボーレート設定済みの「Ready」と表示する。

## 参照

1. M5Stack Tab5公式資料: https://docs.m5stack.com/en/core/Tab5
   - HY2.0-4P Port A: GPIO53 / GPIO54、GND、5Vの回路図情報。
2. Jasionf/M5Stack-Tab5 UART実装例: https://github.com/Jasionf/M5Stack-Tab5
   - UART TX=GPIO53、RX=GPIO54として使用する例。
3. M5Stack Community TermTab5スレッド: https://community.m5stack.com/topic/8194/termtab5
   - Port AのGPIO53/54はユーザーI2Cとの共用であり、ハードウェアUARTを使用できるとの補足。

## 配線上の注意

- UART信号はTTLレベルで接続する。PCのRS-232（±電圧）信号を直接接続してはならない。
- Tab5 TX (GPIO53) → 接続先 RX、Tab5 RX (GPIO54) ← 接続先 TX、GND同士を接続する。
- 5Vは必要な場合だけ接続し、接続先の電源仕様を必ず確認する。
- 標準的なGrove/HY2.0-4Pの黄/白線はI2C向けの名称であるが、本実装ではUART信号として再用途する。
