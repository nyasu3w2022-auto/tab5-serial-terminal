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

## `SKK-JISYO.TAB5.txt`

このファイルは、同梱する小辞書`SKK-JISYO.S`で不足する基本的な標準SKK見出し語を補う、小規模な補助辞書です。起動時には`/skk/SKK-JISYO.TAB5.txt`として利用され、ユーザー辞書とシステム辞書の間にある第2優先の辞書として検索されます。

| 検索順 | 辞書 | 更新方法 |
|:---:|:---|:---|
| 1 | `/skk-user/SKK-JISYO.user.txt` | TAB5の自動学習・Dictionary Editor |
| 2 | `/skk/SKK-JISYO.TAB5.txt` | プロジェクト資産を更新して再ビルド |
| 3 | `/skk/SKK-JISYO.S.txt` | SKK Development Teamの小辞書 |

補助辞書の`きt /来/`は、標準`SKK-JISYO.L`にある「来た」の送り仮名キーを取り込んだものです。SKK辞書データとしてGPL-2.0-or-laterを適用し、同ディレクトリの[`GPL-2.0.txt`](GPL-2.0.txt)をライセンス本文として保持します。
