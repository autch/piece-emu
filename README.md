# piece-emu — Aquaplus P/ECE Diagnostic Emulator

実機なら黙って流す怪しい動作を積極的に検出する、アクアプラス P/ECE（EPSON S1C33209 SoC）向けの diagnostic エミュレータです。
GDB RSP とセミホスティングにより、コンパイラ開発のデバッグ基盤として機能します。
P2-1 フェーズ実装済み：PFI フラッシュイメージからのフルシステムブートに対応し、MMC カーネルが AppStart まで到達することを確認済み。

A diagnostic S1C33209 emulator for the Aquaplus P/ECE handheld.
Catches dubious-but-silently-ignored patterns that real hardware would let slip.
GDB RSP + semihosting for compiler development.
P2-1 complete: full-system boot from PFI flash image; MMC kernel reaches AppStart.

---

## 概要 / Overview

| 項目 | 内容 |
|---|---|
| ターゲット CPU | EPSON S1C33000 (S1C33209) — 32-bit RISC, 16-bit fixed-width instructions |
| ターゲットデバイス | Aquaplus P/ECE |
| 言語 | C++20 |
| ビルドシステム | CMake + Ninja |
| ステータス | **P2-1 フェーズ実装済み** — 全命令・周辺デバイス・GDB RSP・セミホスティング・HLT/SLEEP 高速スキップ・PFI ブート・カーネル AppStart 到達確認 |

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

## 実装状況 / Implementation Status

### CPU コア / CPU Core ✅

全 ISA クラス（Class 0–6）実装済み。逆アセンブラ、PSR フラグ、EXT 即値拡張、ディレイスロット検証を含む。

All ISA classes (0–6) implemented. Includes disassembler, PSR flags, EXT immediate extension, and delay-slot validation.

### 周辺デバイス / Peripherals ✅

| 周辺デバイス | アドレス | 実装内容 |
|---|---|---|
| INTC（割り込みコントローラ）| 0x040260 | ISR/IEN/優先度レジスタ、トラップ配送 |
| ClkCtl（クロック制御）| 0x040140 | CPU クロック選択（24/48 MHz）、P07 連携 |
| Timer8bit × 4 | 0x048100– | PTRUN/PSET/PRLD、アンダーフロー IRQ、`next_wake_cycle()` |
| Timer16bit × 6 | 0x048180– | PRUN/PRESET/CRA/CRB 比較、SELFM、`next_wake_cycle()` |
| PortCtrl | 0x0402C0– | K5/K6 入力、P ポート出力、P07→ClkCtl 通知、KEY IRQ |
| BcuAreaCtrl | 0x040020 | トラップテーブルベースレジスタ（TTBR）書き込み |
| WDT（ウォッチドッグ）| 0x040170 | NMI 発火（1 ms 周期）、ClockTicks インクリメント経路 |
| RTC（計時タイマ）| 0x040150 | 1 Hz クロックフラグ（rRTCSEL bit3）トグル、GetSysClock() 対応 |
| SIF3（シリアル I/F）| 0x0401F4 | TXD/STATUS/CTL レジスタ、HSDMA Ch0 インライン DMA |
| HSDMA（高速 DMA）| 0x048220 | 4 チャネル、Ch0 = LCD 転送（SIF3 連動）、Ch1 = サウンド転送コールバック |

### HLT/SLEEP 高速スキップ / Fast-forward on HLT ✅

`slp`/`halt` 命令で CPU が停止すると、全周辺デバイスの `next_wake_cycle()` から
最早イベント時刻を計算し、サイクルカウンタをその時刻に飛ばしてから周辺デバイスを起こします。
カーネルの `ei; slp` パターンに対応し、実時間でのビジーウェイトを回避します。

When the CPU halts via `slp`/`halt`, the emulator fast-forwards to the earliest
`next_wake_cycle()` across all peripherals, fires the relevant interrupt, and resumes.
Supports the kernel's `ei; slp` pattern without busy-waiting.

### セミホスティング / Semihosting ✅

全ポート実装済み（`src/debug/semihosting.cpp`）。
テストプログラム向けヘッダ：`src/tests/bare_metal/semihosting.h`、`src/tests/bare_metal/piece_emu_debug.h`

All ports implemented (`src/debug/semihosting.cpp`).
Test headers: `src/tests/bare_metal/semihosting.h`, `src/tests/bare_metal/piece_emu_debug.h`

| オフセット | 名前 | 方向 | 機能 |
|---|---|---|---|
| +0x00 | CONSOLE_CHAR | W | 1 文字出力（下位 8 bit）|
| +0x02/+0x04 | CONSOLE_STR | W | 文字列ポインタ（32 bit を 2 回の 16 bit 書き込みで渡す）|
| +0x08 | TEST_RESULT | W | 0=PASS, 非0=FAIL でエミュレータ停止 |
| +0x0C/+0x0E | CYCLE_COUNT_LO | R | 総サイクル数の下位 32 bit |
| +0x10/+0x12 | CYCLE_COUNT_HI | R | 総サイクル数の上位 32 bit |
| +0x14 | REG_SNAPSHOT | W | 全レジスタ（R0–R15, SP, PSR, ALR, AHR）を stderr へ出力 |
| +0x18 | TRACE_CTL | W | 1=命令トレース開始、0=停止 |
| +0x1C/+0x1E | BKPT_SET | W | 32 bit アドレスにソフトウェア BP を設定（ヘッドレスモード専用）|
| +0x20/+0x22 | BKPT_CLR | W | ソフトウェア BP を解除 |
| +0x24/+0x26 | HOST_TIME_MS | R | ホスト側ウォールクロック（ミリ秒）|

> **注意:** BKPT_SET はヘッドレスモード専用です。GDB 接続中は SIGTRAP として伝わりません。
> 詳細と修正方針は `PIECE_EMULATOR_DESIGN.md` §4.5 を参照してください。

---

## リポジトリ構成 / Repository Layout

```
piece-emu/
├── src/
│   ├── core/                       libpiece_core.a — CPU コア / BCU / 逆アセンブラ
│   │   ├── cpu_class{0-6}.cpp      S1C33 命令実装（ISA クラス 0〜6）
│   │   ├── cpu_disasm.cpp          逆アセンブラ / Disassembler
│   │   ├── cpu_core.cpp            CPU コア・ディスパッチ / CPU core and dispatch
│   │   ├── bus.cpp                 バスコントロールユニット（BCU）/ Bus Control Unit
│   │   ├── tick.hpp                ITickable インタフェース / ITickable interface
│   │   └── CMakeLists.txt
│   ├── soc/                        libpiece_soc.a — S1C33209 オンチップ周辺デバイス
│   │   ├── peripheral_intc.cpp/hpp 割り込みコントローラ / Interrupt controller
│   │   ├── peripheral_clkctl.cpp/hpp クロック制御 / Clock control
│   │   ├── peripheral_t8.cpp/hpp   8 bit タイマ × 4 / 8-bit timers ×4
│   │   ├── peripheral_t16.cpp/hpp  16 bit タイマ × 6 / 16-bit timers ×6
│   │   ├── peripheral_portctrl.cpp/hpp K/P ポート・キー割り込み / K/P ports and key IRQ
│   │   ├── peripheral_bcu_area.cpp/hpp BCU エリア制御（TTBR）
│   │   ├── peripheral_wdt.cpp/hpp  ウォッチドッグタイマ / Watchdog timer
│   │   ├── peripheral_rtc.cpp/hpp  計時タイマ（RTC）/ Real-time clock
│   │   ├── peripheral_sif3.cpp/hpp SIF3 シリアル I/F（LCD 転送、HSDMA 連動）
│   │   ├── peripheral_hsdma.cpp/hpp HSDMA 高速 DMA（4 チャネル）
│   │   └── CMakeLists.txt
│   ├── board/                      piece_board (INTERFACE) — 外付けデバイス（将来）
│   │   └── CMakeLists.txt          S6B0741 LCD / NAND Flash / PDIUSBD12 USB (未実装)
│   ├── debug/                      libpiece_debug.a — ELF ローダ / セミホスティング / GDB RSP
│   │   ├── elf_loader.cpp          ELF ローダ / ELF loader
│   │   ├── pfi_loader.cpp/hpp      PFI フラッシュイメージローダ / PFI flash image loader
│   │   ├── semihosting.cpp         セミホスティング（全ポート実装済み）
│   │   ├── gdb_rsp.cpp             GDB RSP スタブ / GDB RSP stub
│   │   ├── gdb_rsp_regs.cpp        GDB レジスタアクセス層 / Register access layer
│   │   └── CMakeLists.txt
│   ├── host/                       将来: SDL3 フロントエンド / Future SDL3 frontend
│   │   └── CMakeLists.txt
│   ├── main.cpp                    CLI フロントエンド / CLI front-end
│   ├── CMakeLists.txt
│   ├── vcpkg.json                  依存ライブラリ（GTest, CLI11）/ Dependencies
│   └── tests/
│       ├── unit/                   C++ ユニットテスト（GTest）158 テスト
│       │   ├── test_cpu_instructions.cpp
│       │   ├── test_disasm.cpp
│       │   ├── test_ext_imm.cpp
│       │   ├── test_psr_flags.cpp
│       │   ├── test_shift_decode.cpp
│       │   ├── test_bcu.cpp
│       │   ├── test_peripheral_intc.cpp
│       │   ├── test_peripheral_t8.cpp
│       │   ├── test_peripheral_t16.cpp
│       │   ├── test_peripheral_portctrl.cpp
│       │   ├── test_peripheral_rtc.cpp
│       │   └── test_peripheral_sif3.cpp
│       └── bare_metal/             S1C33 ベアメタルテスト（LLVM ツールチェーン使用）
│           ├── semihosting.h       セミホスティングヘルパ（`semi_*` 関数群）
│           ├── piece_emu_debug.h   デバッグポートヘルパ（`EMU_*` マクロ）
│           ├── test_basic.c
│           ├── test_alu_flags.c
│           ├── test_branches.c
│           ├── test_load_store.c
│           ├── test_multiply.c
│           ├── test_shifts.c
│           ├── test_ext_imm.c
│           ├── test_div.c
│           ├── test_misc.c
│           └── crt0.s              スタートアップコード / Startup code
├── docs/
│   ├── s1c33000_quick_reference.md     命令セット・レジスタ・エンコーディング早見表
│   ├── peripheral-implementation-status.md  周辺デバイス実装状況・レジスタマップ・落とし穴
│   └── kernel-source-reference.md      カーネルソース（sdk/pcekn/）要点まとめ
└── PIECE_EMULATOR_DESIGN.md        エミュレータ設計仕様書 / Emulator design specification
```

---

## セットアップ / Setup

### 必要なツール / Prerequisites

```sh
# Debian/Ubuntu
sudo apt install git cmake ninja-build g++
```

vcpkg（GTest・CLI11 の自動インストールに使用）が必要です。
vcpkg is required (used to install GTest and CLI11 automatically).

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
- `build-src/libpiece_core.a` — CPU コア・BCU・逆アセンブラ / CPU core, BCU, disassembler
- `build-src/libpiece_soc.a` — S1C33209 オンチップ周辺デバイス / On-chip peripherals
- `build-src/libpiece_debug.a` — ELF ローダ・セミホスティング・GDB RSP / ELF loader, semihosting, GDB RSP

---

## 使用方法 / Usage

```sh
# ベアメタル ELF を実行（終了コード 0=PASS）
./build-src/piece-emu test.elf

# PFI フラッシュイメージからフルシステムブート
./build-src/piece-emu --pfi piece.pfi

# 逆アセンブルトレース付きで実行
./build-src/piece-emu --trace test.elf

# GDB RSP モード（デフォルトポート 1234）
./build-src/piece-emu --gdb test.elf &
lldb
(lldb) gdb-remote 1234

# 最大実行サイクル数を指定
./build-src/piece-emu --max-cycles 1000000 test.elf

# 外部メモリサイズを変更（Flash 改造 P/ECE 向け）
./build-src/piece-emu --flash-size 2097152 test.elf  # 2 MB Flash
```

### エミュレータメモリマップ / Emulator Memory Map

```
0x000000–0x001FFF   IRAM  (8 KB, 0-wait)
0x030000–0x07FFFF   I/O + semihosting (0x060000)
0x100000–0x13FFFF   SRAM  (デフォルト 256 KB、--sram-size で変更可)
0xC00000+           Flash (デフォルト 512 KB、--flash-size で変更可)
```

---

## テスト / Testing

### C++ ユニットテスト / C++ Unit Tests

```sh
ninja -C build-src test
```

158 テストが通過します。CPU 命令・逆アセンブラ・PSR フラグ・BCU・INTC・T8・T16・PortCtrl・RTC・SIF3/HSDMA を網羅。

158 tests pass. Covers CPU instructions, disassembler, PSR flags, BCU, INTC, T8, T16, PortCtrl, RTC, SIF3, and HSDMA.

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
- ステップ除算シーケンス (`div0u`/`div1`×32) / Step-division sequence

---

## ドキュメント / Documentation

| ファイル | 内容 |
|---|---|
| [`PIECE_EMULATOR_DESIGN.md`](PIECE_EMULATOR_DESIGN.md) | 設計仕様（CPU・周辺デバイス・EXT 不可分性・GDB RSP・セミホスティング・テスト方針）|
| [`docs/s1c33000_quick_reference.md`](docs/s1c33000_quick_reference.md) | S1C33000 命令セット・エンコーディング・レジスタ早見表 |
| [`docs/peripheral-implementation-status.md`](docs/peripheral-implementation-status.md) | 周辺デバイス実装状況・レジスタマップ・既知の落とし穴（P1 フェーズ）|
| [`docs/kernel-source-reference.md`](docs/kernel-source-reference.md) | カーネルソース要点（ブートシーケンス・GetSysClock・InitLCD・SIF3/HSDMA）|

参考資料（`docs/`、日本語）:
Reference PDFs in `docs/` (Japanese):

- `S1C33000 コアCPUマニュアル 2001-03.pdf` — 命令セット・エンコーディング・パイプライン
- `S1C33 Family Cコンパイラパッケージ.pdf` — ABI (§6.5)・SRF 形式仕様
- `S1C33209,201,222テクニカルマニュアル PRODUCT・FUNCTION.pdf` — メモリマップ・周辺機器
- `S1C33 family スタンダードコア用アプリケーションノート.pdf` — 割り込み・ブート手順

---
