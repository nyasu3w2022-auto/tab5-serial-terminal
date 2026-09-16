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

`ime_skk_test.cpp`は、ローマ字からひらがなへの変換、カタカナ切替、促音、`ん`、SKK大文字開始、送り仮名付き候補（`MiRu` → 「見る」）、候補の検索・移動・確定・取消を確認します。さらに、補助辞書`assets/SKK-JISYO.TAB5.txt`の`KiTa`→「来た」、ユーザー辞書の手動登録・先頭優先化・完全一致削除、辞書間の候補重複除去、および同梱の `assets/SKK-JISYO.S.txt` を使った実辞書検索も検証します。

`dictionary_transfer_test.cpp`は、UTF-8/LF SKKユーザー辞書の原子的エクスポート、候補重複を除くマージ、明示的置換、不正形式の拒否と既存辞書非破壊、入力ファイル不在を確認します。microSDの物理マウントはESP-IDF実機依存のため、このホストテストの対象外です。
