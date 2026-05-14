# piece_emu — LLVMエージェント向け使用ガイド

**対象読者**: LLVM S1C33 バックエンドの実装・テストを担当するAIエージェント  
**エミュレータ実装**: `src/`  
**現行バイナリ**: `build-src/piece-emu`  
**実装状況**: フルシステム emulation 実装済み（CPU全命令 + ELFローダ + セミホスティング + GDB RSP + 全周辺デバイス + SIF3/HSDMA + S6B0741 LCD + PFIロード + カーネルブート）。本ガイドはベアメタル ELF テスト用途に絞って解説する。フルシステム側は `docs/headless-system-script.md` / `docs/peripheral-implementation-status.md` を参照

---

## 目的

このエミュレータは、LLVM S1C33 バックエンドが生成したコードをホスト上で実行・検証するためのツールである。実機はデバッグポートを持たないため、コード生成バグの調査にはエミュレータ上でのトレース・ブレークポイント・ステップ実行が不可欠。

---

## ビルド

```bash
cmake -S src -B build-src -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=/home/autch/src/vcpkg/scripts/buildsystems/vcpkg.cmake
ninja -C build-src
```

バイナリ: `build-src/piece-emu`

---

## コマンドライン

```
piece-emu [options] (<elf-file> | --pfi <file.pfi>)

実行対象（どちらか一方）:
  <elf-file>                   ベアメタル ELF をロードして実行
  --pfi <file.pfi>             P/ECE フラッシュイメージをロードして実行

実行制御:
  --trace                      全命令の逆アセンブルを stderr に出力
  --max-cycles N               N サイクル後に強制停止（0 = 無制限）
  --sram-size N / --flash-size N   外部 SRAM / Flash サイズをバイト指定

デバッグ（CpuRunner と同セマンティクス、いずれも繰り返し指定可）:
  --break-at ADDR              PC == ADDR でレジスタダンプ
  --wp-write ADDR[:SIZE]       書き込みウォッチポイント
  --wp-read  ADDR[:SIZE]       読み出しウォッチポイント
  --wp-rw    ADDR[:SIZE]       読み書きウォッチポイント

GDB RSP:
  --gdb                        GDB/LLDB 接続を待ってから実行（同期モード）
  --gdb-port N                 RSP リッスンポート（デフォルト 1234、--gdb 必須）
  --debug-rsp                  RSP パケットを stderr にログ（--gdb 必須）

Flash writeback（--pfi 使用時のみ意味を持つ）:
  --enable-flash-writeback     カーネル由来の flash 書き換えをホスト PFI に反映（headless デフォルト OFF）
  --read-only                  --enable-flash-writeback が指定されても read-only を強制
  --writeback-debounce-ms N    dirty セクタを flush するまでのアイドル間隔（デフォルト 2000）
```

### 終了コード

| コード | 意味 |
|--------|------|
| 0 | PASS（TEST_RESULT=0 またはクリーン HALT） |
| 1 | FAIL（TEST_RESULT ≠ 0、未定義命令、ELFロードエラー） |

---

## 基本的な使い方

```bash
# 実行（TEST_RESULT ポートで自動終了）
./build-src/piece-emu test.elf
echo "Exit: $?"          # 0=PASS, 1=FAIL

# 命令トレース付き実行（デバッグ時）
./build-src/piece-emu --trace test.elf 2>trace.log

# サイクル数を制限（無限ループ防止）
./build-src/piece-emu --max-cycles 2000000 test.elf

# GDB RSP デバッグ（別ターミナルから lldb/gdb で接続）
./build-src/piece-emu --gdb --gdb-port 1234 test.elf &
lldb -o "gdb-remote 1234"
```

---

## メモリマップ

| アドレス範囲 | 領域 | 容量 |
|-------------|------|------|
| 0x000000–0x001FFF | IRAM（内蔵 RAM、0-wait） | 8 KB |
| 0x030000–0x07FFFF | I/O + セミホスティングポート | — |
| 0x100000–0x13FFFF | SRAM（外部 SRAM） | 256 KB |
| 0xC00000〜 | Flash ROM | 512 KB |

ELF のプログラムヘッダに従って各セグメントをバスに書き込む。エントリポイントに PC をセットして実行開始。

---

## セミホスティングポート

ベース: `0x060000`（I/O 領域内）

| オフセット | ポート名 | 動作 |
|-----------|---------|------|
| +0x00 | CONSOLE_CHAR | 書き込んだ値の低バイトを stdout に出力 |
| +0x02 | CONSOLE_STR | 書き込んだ 16 bit 値をポインタとして、null 終端文字列を stdout に出力 |
| +0x08 | TEST_RESULT | **0 を書くと PASS 停止、非 0 を書くと FAIL 停止**（エミュレータ即停止） |

**注意**: TEST_RESULT は 0x060008 = bit 18 が立つアドレス。`ld.w` で書き込むには EXT 命令が 2 個必要になる（19 bit 符号拡張で負値になるため）。`semihosting.h` の `semi_exit()` はこれを正しく処理している。

---

## テストプログラムの書き方

### semihosting.h を使う

`src/tests/bare_metal/semihosting.h` をインクルードする（またはそのまま自テストにコピーする）:

```c
#include <stdint.h>

static inline void semi_putchar(char c) {
    *(volatile uint16_t*)0x060000u = (uint16_t)(unsigned char)c;
}

static inline void semi_puts(const char* s) {
    *(volatile uint32_t*)0x060002u = (uint32_t)(uintptr_t)s;
}

__attribute__((noreturn))
static inline void semi_exit(int result) {
    *(volatile uint32_t*)0x060008u = (uint32_t)result;
    for (;;) {}
}
```

### テスト本体のパターン

```c
// test_foo.c
#include "semihosting.h"

int main(void) {
    // 検証したい動作
    if (foo(2, 3) != 5) return 1;   // 失敗コード（0 以外）
    if (bar(-1)   != 0) return 2;

    return 0; // PASS
}
```

`main()` の戻り値が `_start_c` 経由で `semi_exit()` に渡される（後述の crt0 参照）。

---

## ベアメタルテストインフラ（src/tests/bare_metal/）

既存のテストスイートをそのまま利用するか、同じパターンで新テストを追加できる。

### 構成ファイル

| ファイル | 役割 |
|---------|------|
| `crt0.s` | スタートアップ：SP を IRAM 末尾（0x2000）に設定して `_start_c` を呼ぶ |
| `crt_init.c` | `_start_c`：BSS ゼロクリア後に `main()` を呼び、戻り値を `semi_exit()` に渡す |
| `iram.ld` | リンカスクリプト：全セクションを IRAM (0x000000–0x001FFF) に配置 |
| `semihosting.h` | セミホスティングヘルパー関数 |
| `Makefile` | ビルド・実行ルール |

### ビルドと実行

```bash
cd src/tests/bare_metal

make              # 全テスト ELF を生成
make run          # 全テストをエミュレータで実行（結果サマリ表示）
make clean        # 生成物を削除
```

`make run` の出力例:
```
test_basic.elf:                PASS
test_ext_imm.elf:              PASS
...
Results: 9 passed, 0 failed
```

### 新しいテストを追加する手順

1. `src/tests/bare_metal/test_<name>.c` を作成し、`main()` でアサートして `return 失敗コード` / `return 0` を返す。
2. `Makefile` の `TESTS` 変数に `test_<name>.elf` を追記する。
3. `make run` で全テストを実行する。

---

## コンパイル手順（LLVM S1C33 ツールチェーン）

ツールチェーン: `/home/autch/src/llvm-c33/build/bin/`

```bash
C33HOME=/home/autch/src/llvm-c33
CC=$C33HOME/build/bin/clang
LD=$C33HOME/build/bin/ld.lld
SYSROOT=$C33HOME/sysroot/s1c33-none-elf

# コンパイル
$CC --target=s1c33-none-elf --sysroot=$SYSROOT \
    -O1 -ffreestanding -fno-builtin \
    -c test_foo.c -o test_foo.o

# リンク（iram.ld を使用）
$LD -T src/tests/bare_metal/iram.ld --entry _start \
    crt0.o crt_init.o test_foo.o -o test_foo.elf

# 実行
./build-src/piece-emu test_foo.elf
```

---

## GDB RSP によるデバッグ

接続・レジスタ・メモリ・ステップ実行に加え、**ソフトウェア／ハードウェアブレークポイントも実装済み**（`b main` のような lldb のコマンドがそのまま機能する）。

```bash
# ターミナル 1: エミュレータを GDB 待ち状態で起動
./build-src/piece-emu --gdb --gdb-port 1234 test_foo.elf

# ターミナル 2: lldb で接続
lldb
(lldb) gdb-remote 1234
(lldb) register read     # レジスタ読み出し
(lldb) memory read 0x100000
(lldb) b main            # ブレークポイント設定
(lldb) s                 # 1命令ステップ
(lldb) c                 # ブレークポイント / HALT まで実行継続
```

実装済みの GDB パケット:

| パケット | 機能 | 状態 |
|---------|------|------|
| `g` | レジスタ読み出し（R0–R15, PC, SP, PSR, ALR, AHR） | ✅ |
| `G` | レジスタ書き込み | ✅ |
| `m addr,len` | メモリ読み出し | ✅ |
| `M addr,len:data` | メモリ書き込み | ✅ |
| `s` | 1命令ステップ実行 | ✅ |
| `c` | ブレークポイント / HALT まで実行継続 | ✅ |
| `Ctrl-C` | 実行中断 | ✅ |
| `Z0`/`z0` | ソフトウェアブレークポイント（毎ステップ後に PC を照合） | ✅ |
| `Z1`/`z1` | ハードウェアブレークポイント（`Z0` と同一実装） | ✅ |

ブレークポイントは PC アドレス集合への登録で、実行継続中に毎ステップ照合される。`--gdb` は同期モード（RSP サーバが 1 スレッドで CPU を直接ステップ）。`piece-emu-system` の非同期モードについては親プロジェクトの `CLAUDE.md`「GDB RSP Modes」を参照。

---

## フルシステム回帰 diff（piece-emu-headless-system）

ベアメタル ELF テストとは別系統の検証パスとして、**カーネル + P/ECE アプリを `.pfi` から起動し、フレームごとの VRAM ハッシュ列を stdout に出力する** SDL 非依存フロントエンド `piece-emu-headless-system` がある。バックエンド検証エージェントにとっての用途は **gcc33 ビルドと LLVM ビルドの同一アプリを差分比較すること**。両者を同じ `.pfi` + 同じ入力スクリプトで走らせ、ハッシュ列を `diff` すれば、最初に乖離したフレームが「どこで codegen が壊れたか」を指す。

```bash
# gcc33 リファレンスビルドと LLVM 候補ビルドをそれぞれ走らせる
./build-src/piece-emu-headless-system ref-gcc33.pfi --script run.txt --rtc-fixed 2026-01-01T00:00:00 > ref.hashes
./build-src/piece-emu-headless-system cand-llvm.pfi --script run.txt --rtc-fixed 2026-01-01T00:00:00 > cand.hashes
diff ref.hashes cand.hashes | head   # 最初の乖離フレーム = 回帰箇所
```

- `--rtc-fixed ISO8601` で RTC をピン留めすると、同一 PFI + 同一スクリプトは **bit-identical な stdout** を返す（決定論保証）。
- `--wall-timeout SECONDS`（デフォルト 60）が、LCD を更新しなくなったアプリ（カーネルの省電力待ち等）の暴走を防ぐ。
- スクリプトは `frame <N> press|release|snapshot|hash|quit` と `wait <K> frames` の糖衣構文。ボタン名は UP/DOWN/LEFT/RIGHT/A/B/START/SELECT（case-insensitive）。
- `--trace` / `--break-at` / `--wp-write|read|rw` は `piece-emu` と同じセマンティクスで使える。`--gdb-port` は headless では未実装（インタラクティブ GDB は `piece-emu-system` を使う）。

スクリプト文法・出力フォーマット・終了コード・end-to-end の diff 例は **`docs/headless-system-script.md`** を参照。

---

## エージェントの典型ワークフロー

```
1. コードを修正 (piece-toolchain-llvm/)
2. ビルド: cd /home/autch/src/piece-emu && make -C src/tests/bare_metal
3. 実行:   make -C src/tests/bare_metal run
4. PASS → 次のタスクへ
   FAIL → トレースで調査:
           ./build-src/piece-emu --trace test_foo.elf 2>&1 | less
5. 特定命令でフォルトする場合 → --trace で oops ダンプを確認
6. 修正して 2 に戻る
```

---

## このガイドの対象範囲

このガイドは **ベアメタル ELF のテスト**（`piece-emu` + `src/tests/bare_metal/`）に焦点を当てている。この用途には全機能が揃っている。

P1 完了以降、フルシステム emulation（SIF3 / HSDMA / S6B0741 LCD / ボタン入力 / PWM サウンド / PFI ロード / カーネルブート）も実装済みで、`piece-emu-system`（SDL3 GUI フロントエンド）と `piece-emu-headless-system`（上記の回帰 diff 用）が利用できる。フルシステム側の周辺デバイス実装状況は `docs/peripheral-implementation-status.md`、カーネルブートの詳細は `docs/kernel-source-reference.md` を参照。

---

## 実装済みテスト一覧（全 PASS 確認済み）

| ファイル | 検証内容 |
|---------|---------|
| `test_basic.c` | 基本 ALU、関数呼び出し、グローバル変数、配列 |
| `test_ext_imm.c` | ext 命令（1個・2個）の即値拡張と符号規則 |
| `test_alu_flags.c` | PSR N/Z/V/C フラグ更新（全 ALU 命令） |
| `test_load_store.c` | LD/ST 全幅、sign/zero extend、post-increment、SP 相対 |
| `test_shifts.c` | SRL/SLL/SRA/SLA/RR/RL（即値・レジスタ量）、PSR フラグ |
| `test_branches.c` | 全条件分岐 10 種 |
| `test_multiply.c` | MLT.H/MLTU.H/MLT.W/MLTU.W、MAC |
| `test_div.c` | ステップ除算シーケンス（DIV0U/DIV1 × 32） |
| `test_misc.c` | SCAN0/1/SWAP/MIRROR、ADC/SBC、BTST/BSET/BCLR/BNOT、PUSHN/POPN、ALR/AHR/PSR |
