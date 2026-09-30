# Host-side tests

`ime_skk_test.cpp` はESP-IDFやLVGLに依存しないローマ字・SKK変換コアの単体テストです。リポジトリ直下で次のコマンドを実行できます。

```bash
g++ -std=c++17 -Wall -Wextra -Werror -I. \
    main/ime_skk.cpp tests/ime_skk_test.cpp -o /tmp/test_ime_skk
/tmp/test_ime_skk

g++ -std=c++17 -Wall -Wextra -Werror -I. \
    main/dictionary_transfer.cpp tests/dictionary_transfer_test.cpp -o /tmp/test_dictionary_transfer
/tmp/test_dictionary_transfer
```

`ime_skk_test.cpp`は、ローマ字からひらがなへの変換、カタカナ切替、促音、`ん`、SKK大文字開始、送り仮名付き候補（`MiRu` → 「見る」）、候補の検索・移動・確定・取消を確認します。さらに、補助辞書`assets/SKK-JISYO.TAB5.txt`の`KiTa`→「来た」、ユーザー辞書の手動登録・先頭優先化・完全一致削除、辞書間の候補重複除去、および同梱の `assets/SKK-JISYO.S.txt` を使った実辞書検索も検証します。学習保存については、学習しない・保留保存・手動保存の各モード、RAM保留中の候補優先化、明示フラッシュを確認します。

`dictionary_transfer_test.cpp`は、UTF-8/LF SKKユーザー辞書の原子的エクスポート、候補重複を除くマージ、明示的置換、不正形式の拒否と既存辞書非破壊、入力ファイル不在を確認します。microSDの物理マウントはESP-IDF実機依存のため、このホストテストの対象外です。

`terminal_scrollback_test.cpp`は、PSRAMを模したアロケータの下で、全画面スクロールの履歴化、部分スクロール領域・`CSI M`の非履歴化、履歴閲覧中の受信補正、`CSI 3 J`、固定512行リング、フォント切替時の履歴破棄を確認します。

`sixel_basic_test.cpp`は、固定面のDEC SixelデコーダとVT100統合を確認します。7/8-bit DCS、RGB/HLS、透明・不透明背景、repeat、異常DCSの破棄、Sixel capabilityを含むDA応答、`CSI 2 J`／`CSI 3 J`、スクロール、RXオーバーフロー後・DCS終端欠落後の復帰を対象にします。

後者2件は`esp_heap_caps.h`、LVGL色型、ESPログを最小スタブ化してASan/UBSan付きで実行しています。スタブはESP-IDF本体の動作を置き換えるものではないため、PSRAM残量、MIPI-DSI描画、USB/UART連続受信、タッチ操作、SDMMCはTab5実機での`idf.py build`・フラッシュ後に確認してください。
