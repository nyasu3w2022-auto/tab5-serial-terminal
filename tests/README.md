# Host-side tests

`ime_skk_test.cpp` はESP-IDFやLVGLに依存しないローマ字・SKK変換コアの単体テストです。リポジトリ直下で次のコマンドを実行できます。

```bash
g++ -std=c++17 -Wall -Wextra -Werror -I. \
    main/ime_skk.cpp tests/ime_skk_test.cpp -o /tmp/test_ime_skk
/tmp/test_ime_skk
```

このテストは、ローマ字からひらがなへの変換、カタカナ切替、促音、`ん`、SKK大文字開始、送り仮名付き候補（`MiRu` → 「見る」）、候補の検索・移動・確定・取消を確認します。さらに、補助辞書`assets/SKK-JISYO.TAB5.txt`の`KiTa`→「来た」、ユーザー辞書の手動登録・先頭優先化・完全一致削除、辞書間の候補重複除去、および同梱の `assets/SKK-JISYO.S.txt` を使った実辞書検索も検証します。
