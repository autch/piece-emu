# piece-emu-headless-system — スクリプトと出力フォーマット

**対象読者**: gcc33 ビルドと LLVM ビルドの P/ECE アプリを画面出力で比較するワークフローを実行する人 / エージェント
**対応バイナリ**: `build-src/piece-emu-headless-system`
**実装ファイル**: `src/headless_system_main.cpp`, `src/headless_cli_config.{hpp,cpp}`

---

## 目的

カーネル + アプリを `.pfi` から起動し、

- **フレームごとの VRAM ハッシュ列**を stdout に出力
- **スクリプトファイル**でボタン入力 / PNG 保存 / 終了タイミングを指定
- **RTC を固定値にピン留め**して、同一入力なら **bit-identical な stdout** を返す

これを 2 種類のバイナリ（gcc33 リファレンス vs LLVM 候補）に対して走らせ、`diff` で最初に乖離するフレームを特定するためのフロントエンドです。

---

## 起動方法

```sh
./build-src/piece-emu-headless-system <path.pfi> --script <path.txt> [options]
```

### 必須オプション

| フラグ | 意味 |
|---|---|
| `<path.pfi>` | P/ECE フラッシュイメージ（read-only。ホスト PFI への writeback は行わない） |
| `--script <path>` | 入力 / 動作スクリプトファイル |

### 主要オプション

| フラグ | デフォルト | 意味 |
|---|---|---|
| `--max-frames N` | 7200 (≈2 分@60Hz) | LCD フレーム N 個出力したら停止 |
| `--max-cycles N` | 0 (無制限) | CPU サイクル N で停止 |
| `--wall-timeout SECONDS` | 60 | 実時間 N 秒で強制停止（exit 3）。0 で無効 |
| `--rtc-fixed ISO8601` | `2026-01-01T00:00:00` | RTC を固定 ISO-8601 タイムスタンプにピン留め |
| `--hash-every N` | 1 | N フレームごとにハッシュ行を出力（1 で毎フレーム） |
| `--snapshot-dir DIR` | `.` | `snapshot` コマンドの PNG 書き出し基底ディレクトリ |

### デバッグ系（CpuRunner と同等セマンティクス）

| フラグ | 意味 |
|---|---|
| `--trace` | 命令ごとに逆アセンブルを stderr に出す |
| `--break-at ADDR` | PC == ADDR でレジスタダンプ |
| `--wp-write ADDR[:SIZE]` | 書き込みウォッチポイント |
| `--wp-read  ADDR[:SIZE]` | 読み出しウォッチポイント |
| `--wp-rw    ADDR[:SIZE]` | 読み書きウォッチポイント |

`--gdb-port` / `--gdb-debug` はまだ実装されておらず、指定するとエラー終了 (exit 2) します。インタラクティブ GDB が必要な場合は `piece-emu-system` を使ってください。

---

## スクリプト文法

プレーンテキスト、1 行 1 コマンド、`#` 以降はコメント、空行は無視。
**フレーム番号は絶対値、かつ非減少**でなければなりません。

```text
frame <N> press   <BUTTON>           # ボタンを押下状態にする
frame <N> release <BUTTON>           # ボタンを解放状態にする
frame <N> snapshot <filename>        # フレーム N の LCD を PNG に保存
frame <N> hash                       # --hash-every>1 でも N でハッシュを強制出力
frame <N> quit                       # 正常終了
wait  <K> frames                     # 直前で言及したフレームから K フレーム待つ糖衣構文
```

### ボタン名

case-insensitive。P/ECE 物理ボタンに対応：

| 名前 | レジスタ:bit | 物理 |
|---|---|---|
| `UP` | K6:bit3 | 十字キー上 |
| `DOWN` | K6:bit2 | 十字キー下 |
| `LEFT` | K6:bit1 | 十字キー左 |
| `RIGHT` | K6:bit0 | 十字キー右 |
| `A` | K6:bit5 | A ボタン |
| `B` | K6:bit4 | B ボタン |
| `START` | K5:bit4 | START |
| `SELECT` | K5:bit3 | SELECT |

### コマンド意味論

すべて「**指定フレームの先頭**」で評価されます。1 つのフレームに複数コマンドを書けます（例: 同一フレームで A 押下 + B 解放）。

```text
# 例: フレーム 60 の頭で A を押し、フレーム 120 の頭で離す
frame  60 press A
frame 120 release A
```

スクリプト中で言及していないボタン状態は前フレームから持ち越されます。「press → 自動 release」のような短押し糖衣構文はありません。

### `wait` 構文

直前で言及した絶対フレーム番号にオフセットを足します。順序付け補助なので、必ず `frame ... <verb>` の前に置きます。

```text
frame  60 press A
wait 60 frames                  # 内部状態: last_frame = 120
frame  +X release A             # ← この書き方は不可。常に絶対値
frame 120 release A             # OK
```

実装は単に「`last_frame += K`」しているだけで、次の `frame <N> ...` の N が `last_frame` 以上であることをチェックするのに使います。

### よくあるエラー

- フレーム番号が降順 → `script.txt:lineno: frames must be non-decreasing`
- 知らないボタン名 → `script.txt:lineno: unknown button: <name>`
- 知らない verb → `script.txt:lineno: unknown verb: <verb>`
- 引数不足 → `script.txt:lineno: '<verb>' needs <ARG>`

エラーは CPU を起動する**前に**全て検出され、非ゼロで終了します。

---

## 出力フォーマット

stdout は 1 行 1 レコードの ASCII テキスト。stderr にはローダログ / クロック変化 / 警告等、stdout には絶対にハッシュ行と関連カラムのみが出ます。

### ハッシュ行

```text
frame=00000 cycles=00000647710 hash=0x431e45c7e0679325
frame=00200 cycles=00007790852 hash=0x3d45479dc82eb27e  press=A
frame=00300 cycles=00008977422 hash=0x431e45c7e0679325  snapshot=/tmp/frame300.png
```

| カラム | 説明 |
|---|---|
| `frame=NNNNN` | 0 始まり 5 桁ゼロ詰め。HSDMA Ch0 完了 1 回が 1 フレーム |
| `cycles=NNNNNNNNNNN` | 累積 CPU サイクル数（11 桁ゼロ詰め） |
| `hash=0xHHHHHHHHHHHHHHHH` | FNV-1a 64-bit ハッシュ over LCD 2bpp pixel buffer (88×128 = 11264 byte) |
| `press=A,B,...` | このフレームで `press` した押下イベント（複数可、カンマ区切り） |
| `release=...` | このフレームで `release` したイベント |
| `snapshot=path` | このフレームで保存した PNG パス（実際に書けたパス） |

カラム順は固定 (`press=` → `release=` → `snapshot=`)。スクリプトでイベントが発生しない場合は当該カラムを省略します。**この順序はリグレッションログ互換のため将来も変えません。**

### `--hash-every N` の挙動

- N=1（デフォルト）: 毎フレーム出力
- N>1: `frame_no % N == 0` のフレームだけ出力
- どのフレームでも `frame <N> hash` を書くと強制出力（`force_hash` 経路）
- `frame <N> quit` を含むフレームは必ず出力

### stderr の例

```text
PFI SYSTEMINFO: sram_size=0x40000 bytes
PFI SYSTEMINFO: flash_size=0x80000 bytes
Loaded PFI '...': flash=0x80000 bytes, sys_clock=24000000 Hz, ...
PFI loaded, reset vector=0xC02004
[CLK] CPU clock: 3 MHz
[CLK] CPU clock: 24 MHz
[SNAP] saved: /tmp/frame300.png
Stopped after 10680223 cycles, 401 frames
```

stderr は決定論保証の対象外（タイミングメッセージ等が混ざる）。**比較は必ず stdout だけで行ってください**。

---

## 終了コード

| code | 意味 |
|---|---|
| 0 | 正常終了 (`frame <N> quit` / `--max-frames` 到達 / SIGINT / `--max-cycles` 到達) |
| 1 | エラー（PFI ロード失敗 / スクリプト解析失敗 / CPU フォルト等） |
| 2 | 未実装オプション指定 (`--gdb-port` 等) |
| 3 | `--wall-timeout` で実時間上限到達 |

CI で結果を判定する場合、ハッシュ列の `diff` だけでなく**両側とも exit 0**であることを確認してください。片方が `--wall-timeout` で打ち切られていれば（exit 3）、その時点で測定が成り立っていないと判断できます。

---

## 決定論性

同一の `(.pfi, --script, --rtc-fixed)` 三つ組と同一のエミュレータビルドであれば、2 回走らせて stdout が **bit-identical** になることを保証します。実現要素は以下のとおり：

- **CPU は完全に決定的** — peripherals 含む内部状態は host time に依存しない
- **RTC** — `--rtc-fixed ISO8601` で `host_seconds_since_2000()` を定数化。256 Hz プリスケーラはすでに CPU サイクル駆動なので影響なし
- **オーディオデバイス未オープン** — SDL audio device を開かない。emulated sound 内部状態のみ計算
- **シングルスレッド** — CPU・スクリプト処理・ハッシュ計算・PNG 書き出しが全部メインスレッド。スレッドスケジューリングの揺らぎが入らない
- **リアルタイムペーシングなし** — SDL_DelayNS 等は呼ばない

### 決定論が崩れる条件（注意）

- `--gdb-port` が将来実装されたとき、デバッガアタッチタイミング次第で stdout が変動 → ヘルプに `voids determinism guarantee` と明記
- `--trace` を有効にすると stderr に逆アセンブルが流れるが、**stdout** は変わらない
- エミュレータバイナリ自身を rebuild してハッシュ実装や peripheral タイミングを変えると当然変動。リファレンスログを取り直すこと

---

## フレーム境界の定義

「1 フレーム」は **HSDMA Ch0 完了 1 回**です。これは P/ECE カーネル / SDK が LCD を SIF3 経由でフレーム転送する完了通知で、通常アプリ実行中は ~60 Hz で発生します。

`frame=0` は CpuRunner 起動後の最初の `LcdFrameBuf::push()` 相当（HSDMA Ch0 完了 1 回目）です。ブート時にカーネルがスプラッシュ等を出す場合、それも frame 0+ に含まれます。

### 既知の限界

- **HSDMA Ch0 を一定期間止めるアプリがある**: メニュー待ちやスリープ系の処理に入るとカーネルが LCD 転送を止めることがあり、スクリプトの「frame N で quit」が永遠に届かないことがあります。`--wall-timeout` で実時間上限を入れて防御してください
- **アプリ間でフレームレートが異なる**: 60 Hz 固定とは限らない。同じスクリプトを別アプリに使い回すと frame N の意味が変わるので、**スクリプトはアプリごとに用意**するのが基本

---

## 使用例

### gcc33 リファレンスと LLVM 候補を diff する

```sh
# リファレンス（gcc33 build）
./build-src/piece-emu-headless-system \
    reference-gcc33.pfi \
    --script tests/scripts/blackwings_title_to_stage1.txt \
    --rtc-fixed 2026-01-01T00:00:00 \
    > /tmp/ref.log

# 候補（LLVM build）
./build-src/piece-emu-headless-system \
    candidate-clang.pfi \
    --script tests/scripts/blackwings_title_to_stage1.txt \
    --rtc-fixed 2026-01-01T00:00:00 \
    > /tmp/cand.log

# 完全一致なら何も出ない。最初の不一致行で codegen 退行が露見
diff /tmp/ref.log /tmp/cand.log | head
```

### タイトル画面の PNG だけ取りたい

```text
# script: take_title.txt
frame 200 snapshot title.png
frame 210 quit
```

```sh
./build-src/piece-emu-headless-system my.pfi \
    --script take_title.txt --hash-every 9999 --snapshot-dir snaps/
```

`--hash-every` を大きくすると hash 行はほぼ出ず PNG だけ取れます。

### 退行分離のためのフレーム特定

```sh
# 全フレームハッシュを取って初差分を見る
./build-src/piece-emu-headless-system ref.pfi --script s.txt > ref.log
./build-src/piece-emu-headless-system cand.pfi --script s.txt > cand.log
diff -u ref.log cand.log | grep '^-frame' | head -1
# → frame=00437 cycles=... の付近で乖離 ⇒ そのフレーム時点で
#   piece-emu-system --gdb-port 1234 を起動して LLDB MCP でアタッチ
```

---

## 参考実装ポインタ

| 機能 | 場所 |
|---|---|
| メインループ + ハッシュ + スクリプト駆動 | `src/headless_system_main.cpp` |
| CLI11 オプション定義 | `src/headless_cli_config.{hpp,cpp}` |
| RTC 固定時刻 API (`set_fixed_epoch`) | `src/soc/peripheral_rtc.{hpp,cpp}` |
| ボタン名 → ButtonState | `src/system/named_button_input.{hpp,cpp}` |
| PNG ライタ | `src/host/screenshot.{hpp,cpp}` (`piece_imageio` static lib) |
| フレーム境界 (HSDMA Ch0 callback) | `src/soc/peripheral_hsdma.{hpp,cpp}` |
| VRAM → 2bpp pixel array | `S6b0741::to_pixels`, `src/board/s6b0741.cpp` |

ハッシュは FNV-1a 64-bit (基数 `0xCBF29CE484222325`、乗数 `0x100000001B3`)、`pixels[88][128]` を行優先で 11264 byte 連続バイトとしてハッシュします。
