# piece-emu — Aquaplus P/ECE Diagnostic Emulator

実機なら黙って流す怪しい動作を積極的に検出する、アクアプラス P/ECE（EPSON S1C33209 SoC）向けの diagnostic エミュレータです。
GDB RSP とセミホスティングにより、コンパイラ開発のデバッグ基盤として機能します。ゲームの動作はまだサポートしていません。

A diagnostic S1C33209 emulator for the Aquaplus P/ECE handheld.
Catches dubious-but-silently-ignored patterns that real hardware would let slip.
GDB RSP + semihosting for compiler development. No game support yet.

---

## 概要 / Overview

| 項目 | 内容 |
|---|---|
| ターゲット CPU | EPSON S1C33000 (S1C33209) — 32-bit RISC, 16-bit fixed-width instructions |
| ターゲットデバイス | Aquaplus P/ECE |
| 言語 | C++20 |
| ビルドシステム | CMake + Ninja |
| ステータス | **ベアメタルモード実装済み** — 全命令実装・GDB RSP・セミホスティング |

### 設計方針 / Design Philosophy

**実機より厳しく（diagnostic emulator）。**
実機では黙って無視される不正な操作に対しても、エミュレータは積極的に警告・エラーを報告します。
コンパイラのバグを早期に検出するためであり、このエミュレータの最大の付加価値です。

**Stricter than the real hardware (diagnostic emulator).**
The emulator actively reports warnings and errors for operations that the real hardware silently ignores.
This is the primary value of running tests on an emulator rather than on real hardware.

具体例 / Examples:
- 未定義オペコード → エラー停止 / Undefined opcode → error halt
- `ext` + シフト命令（`ext` 非対応）→ 警告 / `ext` before a shift instruction → warning
- ディレイスロットへの不正命令 → 警告 / Illegal instruction in delay slot → warning
- 非アラインドアクセス → エラー（実機ではアドレス不整例外）/ Misaligned access → error
- `jp.d %rb` → エラー（ハードウェアバグ）/ `jp.d %rb` → error (hardware bug)

---

## リポジトリ構成 / Repository Layout

```
piece-emu/
├── src/
│   ├── cpu_class{0-6}.cpp      S1C33 命令実装（ISA クラス 0〜6）
│   │                           S1C33 instruction implementation (ISA classes 0–6)
│   ├── cpu_disasm.cpp          逆アセンブラ / Disassembler
│   ├── cpu_core.cpp            CPU コア・ディスパッチ / CPU core and dispatch
│   ├── elf_loader.cpp          ELF ローダ / ELF loader
│   ├── gdb_rsp.cpp             GDB RSP スタブ / GDB RSP stub
│   ├── semihosting.cpp         セミホスティング / Semihosting
│   ├── bus.cpp                 バスコントロールユニット（BCU）/ Bus Control Unit
│   ├── main.cpp                CLI フロントエンド / CLI front-end
│   ├── CMakeLists.txt
│   ├── vcpkg.json              依存ライブラリ（GTest）/ Dependencies (GTest)
│   └── tests/
│       ├── unit/               C++ ユニットテスト（GTest）
│       │   ├── test_cpu_instructions.cpp
│       │   ├── test_disasm.cpp
│       │   ├── test_ext_imm.cpp
│       │   ├── test_psr_flags.cpp
│       │   ├── test_shift_decode.cpp
│       │   └── test_bcu.cpp
│       └── bare_metal/         S1C33 ベアメタルテスト（LLVM ツールチェーン使用）
│           ├── test_basic.c
│           ├── test_alu_flags.c
│           ├── test_branches.c
│           ├── test_load_store.c
│           ├── test_multiply.c
│           ├── test_shifts.c
│           ├── test_ext_imm.c
│           ├── test_div.c
│           ├── test_misc.c
│           └── crt0.s          スタートアップコード / Startup code
├── docs/
│   └── s1c33000_quick_reference.md  命令セット・レジスタ・エンコーディング早見表
│                                    Instruction set, registers, and encoding quick reference
└── PIECE_EMULATOR_DESIGN.md    エミュレータ設計仕様書 / Emulator design specification
```

---

## セットアップ / Setup

### 必要なツール / Prerequisites

```sh
# Debian/Ubuntu
sudo apt install git cmake ninja-build g++
```

vcpkg（GTest の自動インストールに使用）が必要です。
vcpkg is required (used to install GTest automatically).

```sh
git clone https://github.com/microsoft/vcpkg.git ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh
```

### ビルド / Build

```sh
cmake -S src -B build-src -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=~/vcpkg/scripts/buildsystems/vcpkg.cmake
ninja -C build-src
```

ビルド成果物 / Build artifacts:
- `build-src/piece-emu` — CLI フロントエンド（ベアメタルモード）/ CLI front-end (bare-metal mode)
- `build-src/libpiece_core.a` — エミュレーションコアライブラリ / Emulation core library

---

## 使用方法 / Usage

```sh
# ベアメタル ELF を実行（終了コード 0=PASS）
# Run a bare-metal ELF (exit code 0=PASS)
./build-src/piece-emu test.elf

# 逆アセンブルトレース付きで実行
# Run with disassembly trace
./build-src/piece-emu --trace test.elf

# GDB RSP モード（デフォルトポート 1234）
# GDB RSP mode (default port 1234)
./build-src/piece-emu --gdb 1234 test.elf &
lldb
(lldb) gdb-remote 1234

# 最大実行サイクル数を指定
# Limit maximum execution cycles
./build-src/piece-emu --max-cycles 1000000 test.elf
```

### セミホスティングポート / Semihosting Ports

テストプログラムはセミホスティングポートへの I/O 書き込みで結果を出力します。
Test programs report results by writing to semihosting I/O ports.

| アドレス / Address | 機能 / Function |
|---|---|
| `0x060000` | CONSOLE_CHAR — 1文字出力 / output one character |
| `0x060002` | CONSOLE_STR — 文字列出力 / output a null-terminated string |
| `0x060008` | TEST_RESULT — 0=PASS, 非0=FAIL / 0=PASS, non-zero=FAIL |

`TEST_RESULT` ポートへのアクセスには **2つの `ext` 命令**が必要です（ビット 18 がセットされているため、19ビット符号拡張で負のアドレスになる）。

`TEST_RESULT` requires **2 `ext` instructions** (bit 18 is set, so the address sign-extends to a negative value in 19-bit form).

### エミュレータメモリマップ / Emulator Memory Map

```
0x000000–0x001FFF   IRAM  (8 KB, 0-wait)
0x030000–0x07FFFF   I/O + semihosting
0x100000–0x13FFFF   SRAM  (256 KB)
0xC00000+           Flash
```

---

## テスト / Testing

### C++ ユニットテスト / C++ Unit Tests

```sh
ninja -C build-src test
```

命令デコード・逆アセンブラ・PSR フラグ・BCU を網羅するユニットテストを含みます。
Covers instruction decoding, disassembler, PSR flag behavior, and BCU.

### ベアメタルテスト / Bare-metal Tests

S1C33 用 LLVM ツールチェーンを使って実際の S1C33 バイナリを生成し、エミュレータで実行します。
Compiles real S1C33 binaries using the LLVM toolchain and runs them on the emulator.

```sh
cd src/tests/bare_metal && make && make run
```

テスト内容 / Test coverage:
- 基本 ALU 演算 / Basic ALU operations
- PSR フラグ・条件分岐 / PSR flags and conditional branches
- ロード・ストア（バイト/ハーフワード/ワード）/ Load/store (byte, halfword, word)
- `ext` 即値拡張の符号規則 / `ext` immediate sign rules
- シフト・ローテート / Shifts and rotates
- ハードウェア乗算器 (`mlt.w`/`mlt.h`) / Hardware multiplier
- ステップ除算シーケンス / Step-division sequence

---

## ドキュメント / Documentation

| ファイル | 内容 |
|---|---|
| [`PIECE_EMULATOR_DESIGN.md`](PIECE_EMULATOR_DESIGN.md) | 設計仕様（CPU タイミング・周辺デバイス・GDB RSP・セミホスティング・テスト方針） |
| [`docs/s1c33000_quick_reference.md`](docs/s1c33000_quick_reference.md) | S1C33000 命令セット・エンコーディング・レジスタ早見表 |

参考資料（`docs/*.pdf`、日本語）:
Reference PDFs in `docs/` (Japanese):

- `S1C33000_コアCPUマニュアル_2001-03.pdf` — 命令セット・エンコーディング・パイプライン
- `S1C33_Family_Cコンパイラパッケージ.pdf` — ABI (§6.5)・SRF 形式仕様
- `S1C33209_201_222テクニカルマニュアル_PRODUCT_FUNCTION.pdf` — メモリマップ・周辺機器
- `S1C33_family_スタンダードコア用アプリケーションノート.pdf` — 割り込み・ブート手順

---

