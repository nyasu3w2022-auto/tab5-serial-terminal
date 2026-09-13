# Host-side tests

`ime_skk_test.cpp` はESP-IDFやLVGLに依存しないローマ字・SKK変換コアの単体テストです。リポジトリ直下で次のコマンドを実行できます。

```bash
g++ -std=c++17 -Wall -Wextra -Werror -I. \
    main/ime_skk.cpp tests/ime_skk_test.cpp -o /tmp/test_ime_skk
/tmp/test_ime_skk
```

このテストは、ローマ字からひらがなへの変換、促音、`ん`、候補の検索・移動・確定・取消、未変換時のSpace送信判定、および同梱の `assets/SKK-JISYO.S.txt` を使った実辞書検索を確認します。
