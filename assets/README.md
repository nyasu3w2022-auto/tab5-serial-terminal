# SPIFFS assets

## `SKK-JISYO.S.txt`

このファイルはSKK Development Teamが配布する `SKK-JISYO.S` を、ESP-IDFのSPIFFSで読み込めるよう **EUC-JPからUTF-8へ変換し、改行をLFへ正規化したもの**です。TAB5ローカル日本語入力のかな漢字変換に使用します。

原典の辞書ヘッダおよびSKK辞書リポジトリの `committers.md` に従い、`SKK-JISYO.S` には **GNU General Public License version 2 以降**が適用されます。辞書データのライセンス本文は同じディレクトリの [`GPL-2.0.txt`](GPL-2.0.txt) に収録しています。TAB5アプリ本体のMITライセンスとは別に、この辞書データのライセンスを保持してください。

| 項目 | 内容 |
|:---|:---|
| 配布元 | https://skk-dev.github.io/dict/ |
| 上流管理 | https://github.com/skk-dev/dict |
| ファイル | `SKK-JISYO.S.gz` |
| 変換 | `iconv -f EUC-JP -t UTF-8`、CRを除去 |
| 格納先 | ビルド時に読み取り専用のSPIFFS `skk` パーティションへ配置され、実行時には `/skk/SKK-JISYO.S.txt` としてマウントされます。確定候補の学習データは別の書換え可能な`userdict`パーティションに `/skk-user/SKK-JISYO.user.txt` として保存されます。 |
