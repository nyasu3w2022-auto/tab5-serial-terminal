# M5Stack TAB5 Serial Terminal

M5Stack TAB5 (ESP32-P4) 向けの VT100 互換スタンドアロンシリアルターミナルです。USBホスト、Port A TTL UART、MBUS TTL UARTを選択して、シリアル機器と双方向通信を行えます。

## 特徴

- **USB ホスト CDC-ACM 対応** — VCP チップ（CH34x / CP210x / FTDI）および標準 CDC-ACM デバイス（Raspberry Pi USB ガジェット等）に対応
- **VT100 ターミナルエミュレーション** — カーソル移動・スクロール領域・画面消去・SGR カラー（8色 + 輝度、256色、Truecolor 近似）
- **UTF-8 日本語表示** — IPAゴシックフォントを内蔵し、UTF-8 マルチバイト文字（ひらがな、カタカナ、漢字等）の表示に対応
- **フォントサイズ切り替え** — 設定画面から「Small (16px, 160×43)」と「Large (28px, 91×25)」を切り替え可能
- **pending wrap（遅延折り返し）** — VT100 仕様に準拠した行末処理。余分なスクロールを防止
- **DSR / DA 応答** — `ESC[6n`（カーソル位置報告要求）および `ESC[c`（デバイス属性要求）に応答。bash/readline がブロックしない
- **共有 RX 16 KB リングバッファ** — USB・Port A・MBUS UARTの大量出力時にデータ取りこぼしを防止
- **GUI 設定画面** — `Ctrl+Alt+S` で設定画面を開き、接続・表示設定と**Japanese Input (SKK)**設定を分けて表示。二択項目はタッチで直接切替、多値項目は固定候補パネルから一度のタッチで選択し、Save & CloseでNVSへ保存。`Esc`は候補パネル、次の`Esc`は未保存の設定画面を取り消す
- **ローカル Echo Back** — 接続先に設定コマンドを送らず、Tab5自身が送信済みキー入力を表示するON/OFF設定
- **ローカルかな漢字変換** — TAB5上でローマ字をひらがなへ変換し、内蔵SKK-JISYO.S辞書から候補を選択。確定したUTF-8だけをシリアル接続へ送信
- **差分描画** — 変更行のみ再描画する行単位ダーティフラグで高速表示

## ハードウェア構成

| コンポーネント | 仕様 |
|:---|:---|
| メインボード | M5Stack TAB5 (ESP32-P4 rev1.0) |
| ディスプレイ | 5インチ IPS TFT 1280×720 (MIPI-DSI) |
| キーボード | TAB5 Keyboard (I2C 0x6D, SDA=GPIO0, SCL=GPIO1, INT=GPIO50) |
| USB ホスト | USB Type-A ポート (USB 2.0 High-Speed) |
| TTL UART | Port A（GPIO53/54, UART1）またはMBUS（GPIO6/7, UART2）を設定画面から選択 |

## ターミナル仕様

| 項目 | Small フォント | Large フォント（デフォルト） |
|:---|:---|:---|
| 表示列数 | 160 列 | 91 列 |
| 表示行数 | 43 行 | 25 行 |
| フォントサイズ | 16 px | 28 px |
| フォント種別 | IPAゴシック (lv_font_cjk_16) | IPAゴシック (lv_font_cjk_28) |
| カラー | 16色（ANSI 8色 + 輝度ビット）、256色近似、Truecolor 近似 | 16色（ANSI 8色 + 輝度ビット）、256色近似、Truecolor 近似 |

## キーバインド

### ローカル操作（Tab5 上で処理）

| キー | 動作 |
|:---|:---|
| Ctrl+C | 画面クリア |
| Ctrl+L | 画面強制再描画 |
| Ctrl+Alt+S | 設定画面を開く / 閉じる |
| Esc（設定画面） | 候補パネルを取り消す。候補パネルがなければ、未保存の設定変更を破棄して設定画面を閉じる |
| Ctrl+Alt+J | TAB5ローカルの入力モードを Direct / Japanese (SKK) の間で一時的に切り替える |

### ローカル Echo Back

設定画面の **Echo Back** は、Tab5がキーボードから送信した入力を**Tab5上だけで先行描画する**機能です。接続先の設定は変更せず、`stty` を含む設定コマンドも送信しません。初期値は **OFF** で、選択内容はNVSに保存され、再起動後も維持されます。

| 設定 | 動作 |
|:---|:---|
| OFF（初期値） | Tab5は接続先から受信したデータのみを表示します。接続先がエコーする通常のシェルや端末ではこちらを使用します。 |
| ON | キー入力の送信成功後、通常文字列、Enter、Tab、Backspace、Delete/Del、左/右カーソルをTab5上でローカル描画します。送信内容自体は選択中の接続先へ従来どおり転送されます。 |

> **注意:** 接続先も同じ入力をエコーする状態でONにすると、入力文字が二重に表示されます。接続先が入力を表示しない機器やアプリケーションで使用してください。上/下、Home/End、Page、Insert、ファンクション、Escape、Alt/Ctrl修飾キーは、接続先側の状態に依存するためローカルには描画しません。

### ローカルかな漢字変換（Japanese / SKK）

設定画面の **Input Mode** で起動時の入力モードを選べます。既定値は **Direct** で、従来のシリアル端末入力を変更しません。`Ctrl+Alt+J` は実行中だけの Direct / Japanese (SKK) 切替であり、接続先へは送信されません。Japaneseモードでは小文字ローマ字で入力したかなをTAB5のオーバーレイで保持し、**Spaceで読み全体の候補検索**を行います。大文字はローカルのSKK変換状態へ遷移するだけで、先行する未確定かなを送信しません。

| 操作 | TAB5上の動作 | 接続先への送信 |
|:---|:---|:---|
| 小文字ローマ字 | ひらがなまたはカタカナへ変換して未確定表示 | 送信しない |
| `/`（未確定入力なし） | ASCII略語入力（SKK Abbrev）を開始 | 送信しない |
| `q`（未入力） | ひらがな／カタカナ入力モードを切替。ステータスバーに`JP-HIRA`／`JP-KATA`を表示 | 送信しない |
| `q`（未確定かな入力中） | 現在の文字列を反対のかな種別へ変換して確定 | 変換したUTF-8だけを送信 |
| 先頭の大文字 | SKK見出し語（▽）を開始 | 送信しない |
| 変換中の次の大文字 | 送り仮名を開始 | 送信しない |
| Space（未確定かな／▽） | 読み全体をユーザー・補助・システムSKK辞書の順に検索し候補を表示 | 送信しない |
| Space / Right / Left（▼） | 次候補 / 次候補 / 前候補を選択 | 送信しない |
| Enter（▽／▼） | 読みまたは候補を確定 | 確定したUTF-8だけを送信 |
| Ctrl+J（未確定かな／▽／▼） | SKK確定（CRなし） | 確定したUTF-8だけを送信 |
| Esc（▽／▼） | 変換または候補を取り消す | 送信しない |
| Enter（未確定かな状態） | 未確定かなを確定し、CRを付与 | UTF-8とCRを送信 |
| Tab、Delete、Up/Down、Home/End、Page、Insert、Fキー（未確定時） | 未確定入力・候補を取り消す | 対応するVT100シーケンスを送信 |
| Alt修飾またはCtrl+J以外のCtrl制御キー（未確定時） | 未確定入力・候補を取り消す | 従来どおりESCプレフィックスまたは制御文字を送信 |
| Esc（確定かな状態） | 従来の端末動作 | 従来どおり送信 |

無入力時の`q`は、入力モードを切り替えます。さらに未確定かながあるときの`q`は、SKKの相互かな変換・確定です。たとえば **`JP-HIRA`で`suittiq`** と入力すると「スイッチ」を確定し、**`JP-KATA`で`suittiq`** と入力すると「すいっち」を確定します。どちらもCRは付加せず、入力モード自体は変わりません。

### 記号・句読点・ASCII入力

設定画面の**Punctuation**で、Japanese（`.`→`。`、`,`→`、`、`-`→`ー`）、ASCII（そのまま）、Fullwidth（`．`、`，`、`－`）を選べます。この設定はJapanese (SKK)モードだけに適用され、Directモードの記号送信は変更しません。

| 入力 | 出力 | 備考 |
|:---|:---|:---|
| `z/` | `・` | 中黒 |
| `z-` | `〜` | 波ダッシュ |
| `z.` / `z,` | `…` / `‥` | 三点・二点リーダ |
| `z[` / `z]` | `『` / `』` | 和文引用符 |
| `z{` / `z}` | `【` / `】` | 角括弧 |
| `z!` / `z?` | `！` / `？` | 全角感嘆符・疑問符 |
| 未確定文字列がない状態の`l` | temporary ASCII入力へ入る | ASCII文字を直接送信。EscでJapaneseかな入力へ戻る。`ll`で文字`l`を送信可能。 |

`z`の後に対応する記号が来ない場合は、通常のローマ字入力として扱います。たとえば`za`は従来どおり「ざ」になります。

### 略語入力（`/`）

Japanese (SKK)モードで未確定入力がないときに`/`を押すと、**ASCII略語入力**へ入ります。この状態では英字、数字、記号をTAB5内で未確定のASCII見出し語として保持するため、接続先へは送信されません。Spaceで、通常の変換と同じ順序（ユーザー辞書 → TAB5補助辞書 → システム辞書）でASCII見出し語を検索します。

たとえば **`/github` → Space → Enter** は補助辞書の候補「GitHub」を確定します。候補が複数ある場合はSpaceまたはRight／Leftで選びます。候補がない場合も、入力したASCII文字列そのものを候補として表示するため、**`/curl` → Space → Enter** は`curl`を確定します。Enterと`Ctrl+J`はどちらもCRを付加しません。候補表示中のEscは候補を閉じ、もう一度Escを押すと略語入力を破棄します。Backspaceは未確定ASCIIを1文字削除します。

補助辞書には`git`→`Git`、`github`→`GitHub`、`ssh`→`SSH`、`url`→`URL`、`wifi`→`Wi-Fi`を収録しています。独自の略語は、SDカードの`/TAB5-SKK/SKKUSER.TXT`へ、たとえば`myhost /MyHost/`や`usr/bin /USR-BIN/`のような通常のSKK行を追加してImport Mergeできます。Dictionary EditorのReading欄はかな入力を前提としているため、**ASCII見出し語の直接登録は今回の実装範囲に含めません**。略語候補の確定は日本語候補の学習対象にもなりません。

送り仮名付きの語では、語頭と送り仮名の先頭を大文字にします。送り仮名なしの語は、たとえば **`atama` → Space → Enter** のように入力すると、候補「頭」を選択できます。促音「っ」で始まる送り仮名は、重子音を使います。たとえば **`HashiTta` → Space → Enter** は「走った」、**`ITta` → Space → Enter** は「言った」を確定します。たとえば **`MiRu` → Space → Enter** は辞書キー`みr`を検索して「見る」を確定し、**`KaKu` → Space → Enter** は「書く」を確定します。先行する未確定かながある場合は、最初の大文字も送り仮名開始として扱います。したがって **`miRu` → Space → Enter** も「見る」へ変換できます。**`atamaKara` → Space → Enter** はまず`あたまk`を検索し、候補がない場合は読み`あたま`へフォールバックして「頭」に送り仮名「から」を連結し、「頭から」を確定します。いずれも候補の確定またはEnterまで接続先へ送信されません。

> **注意:** ▽／▼状態でのEnter、または未確定状態での`Ctrl+J`は、日本語文字列を確定するだけでCRは送信しません。リモートシェルでコマンドを実行するには、確定後にもう一度Enterを押してください。`Ctrl+J`は標準的なSKKの確定操作です。未確定入力を残したままTab・ナビゲーション・Delete・ファンクションキー・Alt修飾・通常のCtrl制御キーを使う場合は、リモート端末との編集状態の乖離を防ぐため、TAB5側の未確定入力を取消してから該当キーを送信します。接続先側もUTF-8を解釈できる必要があります。Local Echo BackをONにした場合は、確定後のUTF-8文字列だけがローカル表示されます。

辞書は、ユーザー辞書、TAB5補助辞書、システム辞書の順に検索します。システム辞書`assets/SKK-JISYO.S.txt`と補助辞書`assets/SKK-JISYO.TAB5.txt`はビルド時にSPIFFSの`skk`パーティションへ組み込まれ、実行時にはそれぞれ`/skk/SKK-JISYO.S.txt`、`/skk/SKK-JISYO.TAB5.txt`として利用されます。学習済み候補はSPIFFSの別パーティション`userdict`にあるユーザー辞書`/skk-user/SKK-JISYO.user.txt`へ保存できます。ユーザー辞書はシステム辞書の再フラッシュでは保持されますが、`idf.py erase-flash`または`userdict`パーティションの消去では初期化されます。辞書が読み込めない場合でもひらがな入力・確定は利用できますが、漢字候補は表示されません。変換方式の詳細は [`docs/japanese_ime_design.md`](docs/japanese_ime_design.md) を参照してください。

### 学習保存モード

設定画面の**Learning**で、候補確定時の優先学習とFlash保存方式を選択します。値をタッチすると固定候補パネルが開くため、`Off (no learning)`、`Deferred (batch)`、`Manual save`から直接選べます。初期値は**Deferred (batch)**です。

| 設定画面の選択肢 | 日本語での意味 | 候補確定後 | Flash保存 |
|:---|:---|:---|:---|
| `Off (no learning)` | 学習しない | 候補順を変更しない | 行わない |
| `Deferred (batch)` | 保留保存 | RAM上で直ちに候補順へ反映 | 60秒間の無操作時、または16件の保留時にまとめて保存 |
| `Manual save` | 手動保存 | RAM上で直ちに候補順へ反映 | Dictionary Editorの**Save Learning**または`Ctrl+W`だけで保存 |

保留中の候補順は、再変換時に再起動前から反映されます。手動保存を行わずに電源断・リセットした場合、最後の保存以降の候補順位だけが失われる可能性があります。補助辞書・システム辞書と、すでに保存済みのユーザー辞書は失われません。

### ユーザー辞書の手動登録・削除

**Ctrl+Alt+D**でTAB5ローカルのDictionary Editorを開きます。編集画面を開いている間、キー入力は接続先へ送信されません。Reading、Okuri、Candidateの3フィールドをTabまたはタッチで選び、Japanese (SKK)の未確定文字列・候補は`Ctrl+J`で選択中フィールドへ格納します。Candidate候補を`Ctrl+J`で格納する場合は、送り仮名を含めず候補語幹だけを保存します。Editor下部の**IME -> フィールド名**表示には、入力中の未確定かなと候補を常時表示します。したがって、`Ctrl+J`を押す前にも現在の入力内容と確定先を確認できます。Tab5 Keyboardには独立したF5／F8キーがないため、登録・削除には物理キーボード上のCtrlと英字キーの組合せを使用します。

| 操作 | 動作 |
|:---|:---|
| Tab | Reading → Okuri → Candidateの順にフィールドを移動 |
| `Ctrl+J` | 未確定かなまたは選択候補を選択中フィールドへ確定。接続先へは送信しない |
| Okuriフィールドで`a`〜`z` | 送り仮名の先頭英字を設定。不要なら空欄 |
| Ctrl+S | Reading + Okuri + Candidateをユーザー辞書へ登録し、同じキーの先頭候補へ昇格 |
| Ctrl+X | 完全一致するCandidateをユーザー辞書から削除。最後の候補ならキー行も削除 |
| Ctrl+W | 保留中の学習候補をFlashへ保存し、保存後の完全なユーザー辞書をSDカードへバックアップ |
| Backspace | IME未確定文字列があればIME編集、なければ選択フィールド末尾を削除 |
| Esc | IME未確定文字列を取消。未確定文字列がなければEditorを閉じる |
| `Ctrl+Alt+D` | Editorを閉じ、未保存のフィールド内容を破棄 |

たとえば小辞書にない語を登録する場合、Readingに`ki`→`Ctrl+J`で「き」、Okuriに`t`、Candidateに既存の`KuRu`→Spaceで得た「来」を`Ctrl+J`で入れ、**Ctrlを押したままS**を押します。以後は通常の`KiTa`→Space→Enterで「来た」を変換できます。TAB5補助辞書にはこの`きt /来/`を初期収録しているため、追加登録なしでも「来た」を変換できます。

`User dictionary storage unavailable: reflash updated partition table`と表示された場合は、書換え用`userdict`パーティションを含むファームウェアがまだフラッシュされていません。`idf.py fullclean`、`idf.py build`、`idf.py flash`を順に実行してください。`Cannot add: Reading/Candidate required; Okuri must be a-z`は、ReadingまたはCandidateが空、あるいはOkuriがASCII英字1文字ではない場合に表示されます。

### microSDによるユーザー辞書のバックアップ・移行

microSDカードを**FAT32**でフォーマットしてTab5へ挿入します。Dictionary Editor内では、**Ctrl+E**または`Export SD`でFlashに保存済みのユーザー辞書を`/TAB5-SKK/SKKUSER.TXT`として書き出します。Export SDはFlashを更新しません。この名前は、long file name機能を有効にしていないFATFSでも動作する8.3形式です。書込み時は既存ファイルを8.3形式のバックアップ名へ退避してから新しいファイルを公開するため、FATFSの既存ファイル上書き制約を回避します。

PCで同ファイルをUTF-8／LFのSKK形式のまま編集した後、microSDへ戻してください。**Ctrl+I**または`Import Merge`は既存の候補順を保ったまま、ファイル側にのみある候補を追加します。**Ctrl+R**または`Import Replace`は、1回目に確認を表示し、2回目でSPIFFS上のユーザー辞書をSDカード側の有効な内容へ置き換えます。置換は既存の学習内容を失うため、通常はマージを利用してください。

Dictionary Editorの**Add / Promote**、**Delete Exact**、**Save Learning**、および有効なインポートは、Flash保存後に同じ完全な辞書をSDカードへ自動バックアップします。SD未挿入・書込み失敗時もFlashの保存済み内容は取り消さず、黄色い状態欄にSDバックアップ失敗を表示します。次回の手動保存またはExport SDでバックアップを再試行できます。


### リモートへの送信（選択中の接続方式へそのまま転送）

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
| F1〜F4 | `ESC O P` 〜 `ESC O S`。送信テーブルは実装済みだが、Tab5 Keyboardには独立したFキーがないため現行キーボードからは生成されない。 |
| F5〜F12 | `ESC[15~` 〜 `ESC[24~`。送信テーブルは実装済みだが、Tab5 Keyboardには独立したFキーがないため現行キーボードからは生成されない。 |
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

- **ESP-IDF v5.5.4**（検証済み）。本プロジェクトは`driver/sdmmc_host.h`と`esp_driver_sdmmc`を使用するため、v5.5.4未満はサポート対象外です。v5.5.4より新しい版は未検証です。
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

**注意:** CJKフォントデータを格納するため、カスタムパーティションテーブル（factory 3MB）を使用しています。`assets/`のUTF-8 SKKシステム辞書はビルド時にSPIFFS `skk` パーティションへ自動的に組み込まれます。学習済み候補は別の`userdict`パーティションへ保存され、通常のシステム辞書更新では保持されます。初回フラッシュ、パーティション構成の更新、または`main/CMakeLists.txt`の依存関係更新後は、`idf.py fullclean`後にビルド・フラッシュしてください。`idf.py erase-flash`は学習済みユーザー辞書も消去します。

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

## Port A UART 接続

Port A（HY2.0-4P）をTTL UARTとして使用できます。配線は次のとおりです。

| Port A線 | Tab5側 | UART信号 |
|:---|:---|:---|
| 黄 | GPIO53 | TX |
| 白 | GPIO54 | RX |
| 黒 | GND | GND |
| 赤 | 5V | 必要な場合のみ電源供給 |

接続時は **Tab5 TX → 接続先 RX、Tab5 RX ← 接続先 TX、GND 共通** としてください。Port Aは通常I2C用途でも使えるため、UART使用中は同じPort AのI2C機器を同時に使わないでください。[1]

> **注意:** Port Aは**3.3V TTL UART**として扱ってください。PCのRS-232電圧を直接接続してはいけません。接続先が5Vロジックの場合は、双方向レベル変換器を介してください。

設定画面を `Ctrl+Alt+S` で開き、**Interface**の値をタッチして開く候補パネルから **PortA UART (GPIO53/54)** を選び、Save & Close を押すと切り替わります。UARTは物理的なケーブル接続を検出できないため、ステータスバーでは `PortA:Ready` と表示されます。これはドライバが送受信可能な状態を示し、接続先機器の存在を保証するものではありません。

## MBUS UART2 接続

背面の30ピンMBUSでは、UART2をGPIO6/GPIO7へ割り当ててTTL UARTとして使用できます。Port A（UART1）とは異なるUARTコントローラを使用するため、ピン・ドライバ上の競合を避けています。[1] [2]

| MBUSピン | Tab5側 | UART信号 | 接続先 |
|:---:|:---|:---|:---|
| 16 | GPIO6 (`PC_TX`) | TX | 接続先RX |
| 15 | GPIO7 (`PC_RX`) | RX | 接続先TX |
| GND | GND | GND | 接続先GND |

> **注意:** MBUS UART2も**3.3V TTL UART**です。RS-232を直接接続してはいけません。また、MBUS対応モジュールを装着する場合は、ピン15/16を別用途で使用していないことを確認してください。

設定画面を `Ctrl+Alt+S` で開き、**Interface**の値をタッチして開く候補パネルから **MBUS UART2 (GPIO6/7)** を選び、Save & Close を押します。ドライバが初期化されるとステータスバーに `MBUS:Ready` と表示されます。TTL UARTでは物理的なケーブル接続を検出できません。

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
  ├── shared_rx_ringbuf ← USB / Port A / MBUS RXデータ（16 KBリングバッファ）
  │     └── vt100_process_byte() → term_buffer → term_refresh_display()
  └── key_queue         ← キーボード入力
        ├── Directモード → 選択中のUSB / Port A / MBUS UARTへTX
        └── Japanese (SKK)モード → ime_skk（未確定表示・候補選択）
              └── 確定UTF-8のみ → 選択中のUSB / Port A / MBUS UARTへTX
                    └── Echo Back=ONかつTX成功時のみ → ローカル端末バッファへ描画

vcp_task
  ├── usb_lib_task      ← USB ホストライブラリ常駐タスク
  └── cdc_acm_host      ← CDC-ACM ドライバ
        └── usb_rx_cb() → shared_rx_ringbufへの書き込み

porta_uart_rx_task / mbus_uart_rx_task
  └── uart_read_bytes() → shared_rx_ringbufへの書き込み

keyboard_event_cb() → key_queueへの書き込み
```

## 既知の制限・今後の予定

- **スクロールバック** — 画面外にスクロールしたデータは参照不可
- **MBUS共有制約** — MBUS UART2を使う間は、MBUSピン15/16（GPIO7/GPIO6）を使用する拡張モジュールを併用不可
- **起動時のまれなハング** — USB ホスト初期化中に稀に停止することがある（調査中）

## ライセンス

### プログラムコード
MIT License

### 組み込みフォント
本ソフトウェアは [IPAフォント](https://moji.or.jp/ipafont/) (IPAゴシック) をビットマップデータとして組み込んで使用しています。`main/fonts/lv_font_cjk_16.c` および `main/fonts/lv_font_cjk_28.c` は、IPA Font License v1.0 における派生プログラムです。ライセンスの全文は `IPA_Font_License_Agreement_v1.0.txt` に収録しています。

#### フォントデータの再生成・置換

IPA Font License v1.0 の条件に従い、受領者が組み込み済みの派生フォントデータをオリジナルのIPAフォントから再生成して置き換える手段を提供します。IPAゴシックのTTFファイルを[公式配布元](https://moji.or.jp/ipafont/)から取得し、`lv_font_conv` をインストールしたうえで、以下を実行してください。

```bash
npm install -g lv_font_conv
./tools/generate_cjk_fonts.sh /path/to/fonts-japanese-gothic.ttf
```

このスクリプトは、16pxおよび28pxのLVGLフォントCソースを `main/fonts/` に再生成します。生成対象の文字範囲および生成オプションは `tools/generate_cjk_fonts.sh` に記載しています。

### かな漢字変換辞書

ユーザー辞書は候補を確定した時点で自動更新されます。初期版では個別の登録・削除UIは持たず、確定候補を同じ読みの先頭候補として記録します。

`assets/SKK-JISYO.S.txt` はSKK Development Teamの `SKK-JISYO.S` をUTF-8/LFへ変換してSPIFFSに組み込む辞書データです。`SKK-JISYO.{SML}` には **GNU General Public License version 2以降**が適用されます。[3] 辞書データのライセンス本文は `assets/GPL-2.0.txt`、出典・変換手順は `assets/README.md` に収録しています。アプリ本体のMITライセンスおよびIPAフォントライセンスとは別に、この辞書データのライセンスを保持してください。

## References

[1]: https://docs.m5stack.com/en/core/Tab5 "M5Stack Tab5 — 公式ハードウェア資料"
[2]: https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32p4/api-reference/peripherals/uart.html "ESP-IDF v5.5.4 ESP32-P4 UART Driver"
[3]: https://github.com/skk-dev/dict/blob/master/committers.md "SKK dictionary licenses and editorial policy"
