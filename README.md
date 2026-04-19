# piece-emu — Aquaplus P/ECE Diagnostic Emulator

実機なら黙って流す怪しい動作を積極的に検出する、アクアプラス P/ECE（EPSON S1C33209 SoC）向けの diagnostic エミュレータです。
GDB RSP とセミホスティングにより、コンパイラ開発のデバッグ基盤として機能します。
P2-4 フェーズ実装済み：SDL3 フルシステムフロントエンド（LCD 表示・ボタン入力・PWM サウンド出力）、CPU/SDL/GDB/Audio のマルチスレッド構成、非同期 GDB RSP 対応、O(1) タイマ解析によるホスト CPU 使用率 ~50% を達成。**未実装デバイスはあるものの（USB / 赤外線 / MMC / Flash 書き込み / ADC / IDMA）、`piece.pfi` からカーネルブートしてゲームが起動・操作・発音する状態に到達しています。**

A diagnostic S1C33209 emulator for the Aquaplus P/ECE handheld.
Catches dubious-but-silently-ignored patterns that real hardware would let slip.
GDB RSP + semihosting for compiler development.
P2-4 complete: SDL3 full-system frontend with LCD display, button input, and PWM sound output; four-thread architecture (SDL/CPU/GDB/audio); async GDB RSP (LLDB-compatible); O(1) timer analytics achieving ~50% host CPU usage. **Not every peripheral is emulated (USB, IrDA, MMC, flash write, ADC, IDMA are stubs or absent), but games boot from `piece.pfi`, display on the LCD, accept input, and produce audio.**

---

## 概要 / Overview

| 項目 | 内容 |
|---|---|
| ターゲット CPU | EPSON S1C33000 (S1C33209) — 32-bit RISC, 16-bit fixed-width instructions |
| ターゲットデバイス | Aquaplus P/ECE |
| 言語 | C++20 |
| ビルドシステム | CMake + Ninja |
| ステータス | **P2-4 フェーズ実装済み** — 全命令・主要周辺デバイス・GDB RSP（同期/非同期）・セミホスティング・HLT/SLEEP 高速スキップ・PFI ブート・SDL3 LCD 表示・ボタン入力・PWM サウンド出力・LLDB MCP 対応・4 スレッド構成（SDL/CPU/GDB/Audio）・O(1) タイマ解析（ホスト CPU ~50%）。実機 PFI からゲームを起動・操作・発音可能 |

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
| PortCtrl | 0x0402C0– | K5/K6 入力、P ポート出力（バイト書き込み対応）、P07→ClkCtl 通知、KEY IRQ、p21d() |
| BcuAreaCtrl | 0x040020 | トラップテーブルベースレジスタ（TTBR）書き込み |
| WDT（ウォッチドッグ）| 0x040170 | NMI 発火（1 ms 周期）、ClockTicks インクリメント経路 |
| RTC（計時タイマ）| 0x040150 | 1 Hz クロックフラグ（rRTCSEL bit3）トグル、GetSysClock() 対応 |
| SIF3（シリアル I/F）| 0x0401F4 | TXD/STATUS/CTL レジスタ、HSDMA Ch0 インライン DMA |
| HSDMA（高速 DMA）| 0x048220 | 4 チャネル、Ch0 = LCD 転送（SIF3 連動）、Ch1 = PWM サンプル一括読み出し |
| Sound（PWM 音声）| 0x048220 Ch1 経由 | HSDMA Ch1 EN 0→1 で PWM サンプルを SPSC リングに格納、Ch1 完了 IRQ (vec 23) を `cnt*(cpu_hz/32000)` サイクル後に配信（IL=1、再入回避） |
| S6B0741 LCD | SIF3 経由 | コマンド/データデコード、128×88 VRAM、4 階調ピクセル変換 |

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
│   │   ├── peripheral_sound.cpp/hpp PWM サウンド（HSDMA Ch1 → SPSC リング）/ PWM audio
│   │   └── CMakeLists.txt
│   ├── board/                      libpiece_board.a — 外付けデバイス / Board external devices
│   │   ├── s6b0741.cpp/hpp         Samsung S6B0741 LCD コントローラ / LCD controller
│   │   └── CMakeLists.txt
│   ├── debug/                      libpiece_debug.a — ELF ローダ / セミホスティング / GDB RSP
│   │   ├── elf_loader.cpp          ELF ローダ / ELF loader
│   │   ├── pfi_loader.cpp/hpp      PFI フラッシュイメージローダ / PFI flash image loader
│   │   ├── semihosting.cpp         セミホスティング（全ポート実装済み）
│   │   ├── gdb_rsp.cpp             GDB RSP スタブ（同期 + 非同期モード）/ GDB RSP stub
│   │   ├── gdb_rsp_regs.cpp        GDB レジスタアクセス層 / Register access layer
│   │   └── CMakeLists.txt
│   ├── host/                       libpiece_host.a — SDL3 フロントエンド / SDL3 frontend
│   │   ├── lcd_renderer.cpp/hpp    S6B0741 VRAM → SDL3 テクスチャ / LCD renderer
│   │   ├── audio_output.cpp/hpp    SDL3 オーディオシンク / SDL3 audio sink
│   │   ├── audio_log.cpp/hpp       `--audio-trace` 診断ログ / Audio diagnostic log
│   │   ├── screenshot.cpp/hpp      F12 PNG スクリーンショット / PNG screenshot
│   │   └── CMakeLists.txt
│   ├── system/                     piece-emu-system 内部モジュール / system binary internals
│   │   ├── cpu_runner.cpp/hpp      CPU 実行スレッド / CPU thread
│   │   ├── piece_peripherals.cpp/hpp 全周辺集約 + attach/reset / Peripheral aggregate
│   │   ├── lcd_framebuf.hpp        CPU→SDL 共有フレームバッファ / Shared frame buffer
│   │   ├── button_input.cpp/hpp    SDL3 キー→K5/K6 マップ / Key→button mapping
│   │   └── cli_config.cpp/hpp      CLI11 オプション解析 / CLI option parser
│   ├── tools/                      ホストユーティリティ（C）/ Host utilities (C)
│   │   ├── mkpfi.c                 PFI イメージ作成 / Create PFI image
│   │   ├── pfar.c                  PFFS アーカイバ / PFFS archiver
│   │   └── ripper.c                USB 経由フラッシュ読み出し / Flash ripper via USB
│   ├── pfi_format.h                PFI/PFFS 共有構造体定義 / Shared PFI struct definitions
│   ├── main.cpp                    CLI フロントエンド (piece-emu) / CLI front-end
│   ├── system_main.cpp             SDL3 フロントエンド (piece-emu-system) / SDL3 front-end
│   ├── CMakeLists.txt
│   ├── vcpkg.json                  依存ライブラリ（GTest, CLI11, SDL3, ImGui; Windows では libusb も）/ Dependencies
│   └── tests/
│       ├── unit/                   C++ ユニットテスト（GTest）161 テスト（core 107 + soc 54）
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
│       │   ├── test_peripheral_sif3.cpp    (SIF3 + HSDMA インライン DMA)
│       │   └── test_peripheral_sound.cpp   (PWM サウンドのリング・IRQ 配信)
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
│           ├── crt0.s              スタートアップコード / Startup code
│           └── gen/                ジェネレータ方式リグレッション試験（1,659 ケース）
│               ├── gen_alu.py      ALU 命令（253）/ ALU instructions
│               ├── gen_mem.py      メモリアクセス（129）/ Memory access
│               ├── gen_sp.py       SP 相対ロード/ストア（72）/ SP-relative
│               ├── gen_shift.py    シフト・ローテートほか（628）/ Shift/rotate + misc
│               ├── gen_muldiv.py   乗除算・ステップ除算（69）/ Mul/div/step-div
│               ├── gen_branch.py   分岐・遅延スロット（236）/ Branch + delay slot
│               ├── gen_pushpop.py  pushn/popn（34）
│               ├── gen_mac.py      MAC 命令（15）/ MAC
│               ├── gen_bitop.py    ビット操作（164）/ Bit ops
│               ├── gen_special.py  特殊レジスタ（37）/ Special regs
│               ├── gen_calldrb.py  call.d %rb（6）
│               └── gen_trap.py     int/reti・trap（16）
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
sudo apt install git cmake ninja-build g++ pkg-config
```

vcpkg（GTest・CLI11・SDL3・ImGui の自動インストールに使用）が必要です。
vcpkg is required (used to install GTest, CLI11, SDL3, and ImGui automatically).

```sh
git clone https://github.com/microsoft/vcpkg.git ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh
```

**ripper ツール**（実機フラッシュ吸い出し）を使う場合、libusb-1.0 が別途必要です。
**ripper tool** (real-hardware flash dump) requires libusb-1.0:

```sh
# Debian/Ubuntu
sudo apt install libusb-1.0-0-dev

# macOS (Homebrew)
brew install libusb
```

Windows では vcpkg がビルド時に自動インストールします（後述の `ripper` フィーチャーを参照）。
On Windows, vcpkg installs libusb automatically when the `ripper` feature is enabled (see below).

### ビルド / Build

```sh
cmake -S src -B build-src -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=~/vcpkg/scripts/buildsystems/vcpkg.cmake
ninja -C build-src
```

**vcpkg フィーチャー / vcpkg features:**

| フィーチャー | 内容 | 追加される依存 |
|---|---|---|
| `system` | SDL3 フルシステムフロントエンド + Dear ImGui | `sdl3`, `imgui` |
| `ripper` | 実機 USB フラッシュ吸い出しツール（Windows のみ vcpkg で libusb を取得） | `libusb`（Windows のみ）|

```sh
# SDL3 フロントエンドをビルド
cmake -S src -B build-src -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=~/vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_MANIFEST_FEATURES=system
ninja -C build-src

# SDL3 + ripper をビルド（Windows; Linux/macOS では libusb-1.0-0-dev を事前にインストール）
cmake -S src -B build-src -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=~/vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_MANIFEST_FEATURES="system;ripper"
ninja -C build-src
```

ビルド成果物 / Build artifacts:
- `build-src/piece-emu` — ヘッドレス CLI（ベアメタル + PFI）/ Headless CLI (bare-metal + PFI)
- `build-src/piece-emu-system` — SDL3 フルシステムフロントエンド（SDL3 必須）/ SDL3 full-system frontend
- `build-src/libpiece_core.a` — CPU コア・BCU・逆アセンブラ / CPU core, BCU, disassembler
- `build-src/libpiece_soc.a` — S1C33209 オンチップ周辺デバイス / On-chip peripherals
- `build-src/libpiece_debug.a` — ELF ローダ・セミホスティング・GDB RSP / ELF loader, semihosting, GDB RSP
- `build-src/tools/mkpfi` — PFI フラッシュイメージ作成ツール / PFI flash image creator
- `build-src/tools/pfar` — PFFS アーカイバ / PFFS archiver
- `build-src/tools/ripper` — USB 経由実機フラッシュ吸い出し（libusb-1.0 必須）/ Real-hardware flash ripper via USB

---

## 使用方法 / Usage

```sh
# ベアメタル ELF を実行（終了コード 0=PASS）
./build-src/piece-emu test.elf

# PFI フラッシュイメージからフルシステムブート（ヘッドレス）
./build-src/piece-emu --pfi images/old/piece.pfi

# SDL3 フルシステムフロントエンド（LCD 表示・ボタン入力）
./build-src/piece-emu-system --pfi images/old/piece.pfi

# SDL3 フロントエンド + GDB RSP（LLDB MCP 対応、非同期モード）
./build-src/piece-emu-system --pfi images/old/piece.pfi --gdb-port 1234
lldb
(lldb) gdb-remote 1234

# ヘッドレスで GDB RSP（同期モード）
./build-src/piece-emu --gdb test.elf &
lldb
(lldb) gdb-remote 1234

# デバッグオプション（ヘッドレスモード）
./build-src/piece-emu --wp-write 0x103EA0:4 --pfi images/old/piece.pfi  # SRAM 書き込みウォッチポイント
./build-src/piece-emu --wp-read 0xADDR:2 test.elf                        # 読み出しウォッチポイント
./build-src/piece-emu --break-at 0xC01234 test.elf                       # PC 到達時レジスタダンプ

# 逆アセンブルトレース付きで実行
./build-src/piece-emu --trace test.elf

# 最大実行サイクル数を指定
./build-src/piece-emu --max-cycles 1000000 test.elf

# 外部メモリサイズを変更（Flash 改造 P/ECE 向け）
./build-src/piece-emu --flash-size 2097152 test.elf  # 2 MB Flash
```

### キー操作（piece-emu-system）/ Keyboard Controls

| キー | P/ECE ボタン / 機能 |
|---|---|
| `←` `→` `↑` `↓` | 十字キー / D-pad |
| `Z` | B ボタン / B button |
| `X` | A ボタン / A button |
| `Enter` | START |
| `Backspace` | SELECT |
| `F5` | ホットスタート / Hot reset（BCU・I/O ポートは保持） |
| `Shift` + `F5` | コールドスタート / Cold reset（全初期化） |
| `F12` | スクリーンショット保存（PNG）/ Save PNG screenshot |
| `Esc` | エミュレータ終了 / Quit |

スクリーンショットの保存先は `--snapshot-path DIR` で指定できます（デフォルト：カレントディレクトリ）。ファイル名は `piece_YYYYMMDD_HHMMSS_mmm.png`。PNG 書き出しは `src/third_party/stb/stb_image_write.h`（Sean Barrett, public domain / MIT dual license）を同梱利用しています。128×88 画素の 8-bit グレースケール PNG を速度優先（圧縮レベル 1）で書き出します。

Use `--snapshot-path DIR` to set the directory (default: current). Filenames follow `piece_YYYYMMDD_HHMMSS_mmm.png`. PNG encoding uses the bundled header-only library `src/third_party/stb/stb_image_write.h` (Sean Barrett, public domain / MIT dual license) at compression level 1 (speed over size — a 128×88 image compresses in well under a millisecond).

### ゲームパッド操作（piece-emu-system）/ Gamepad Controls

SDL3 Gamepad サブシステム経由でゲームパッドに対応しています。Windows では XInput 対応のコントローラ（Xbox 360 / Xbox One / 互換品など）をそのまま使用できます。Linux では evdev、macOS では GameController フレームワーク経由で SDL3 が自動認識します。コントローラはホットプラグ対応で、起動後の接続・切断も反映されます。

Gamepads are supported via SDL3's Gamepad subsystem. On Windows, any XInput-compatible controller (Xbox 360 / Xbox One / compatible pads) works out of the box. On Linux (evdev) and macOS (GameController framework) SDL3 auto-detects connected devices. Hot-plug is supported.

| ゲームパッド入力 | P/ECE ボタン |
|---|---|
| D-pad / 左スティック (L-stick) | 十字キー / D-pad |
| A ボタン (SDL `SOUTH`) | A |
| B ボタン (SDL `EAST`) | B |
| `Start` | START |
| `Back` (Select / Share) | SELECT |

デフォルトは **XInput 標準のラベル配列**（A ラベル＝P/ECE A、B ラベル＝P/ECE B）です。`--swap-ab` を指定すると **実機の物理配列**（右側のボタン＝A、左側のボタン＝B、任天堂配列）に切り替わります。SDL3 はフェイスボタンを位置で正規化するため、この切り替えは Xbox・PlayStation・Nintendo いずれのパッドでも同一に機能します。

The default layout follows **XInput face labels** (A label → P/ECE A, B label → P/ECE B). Pass `--swap-ab` to switch to the **physical layout of the real P/ECE hardware** (right-side face button = A, left-side = B — matching Nintendo's label scheme). SDL3 normalises face buttons by position, so this flag works consistently across Xbox / PlayStation / Nintendo pads.

**Windows で Steam を起動している場合の注意**: アプリ終了操作の `START` + `SELECT` 同時押しが、Steam Input によって Guide ボタン合成 → Xbox Game Bar・タスクビュー・オンスクリーンキーボードなどの OS 機能にフックされ、エミュレータにイベントが届かなくなります。**エミュレータ使用時は Steam を終了してください。** Steam 非起動状態であれば追加設定なしで正常に動作します。

**Note for Windows users with Steam running**: the app-exit combination `START` + `SELECT` is intercepted by Steam Input, which synthesises a Guide button press and triggers OS features (Xbox Game Bar, Task View, on-screen keyboard) — the event never reaches the emulator. **Quit Steam before running `piece-emu-system`.** With Steam not running, no additional configuration is needed.

### エミュレータメモリマップ / Emulator Memory Map

```
0x000000–0x001FFF   IRAM  (8 KB, 0-wait)
0x030000–0x07FFFF   I/O + semihosting (0x060000)
0x100000–0x13FFFF   SRAM  (デフォルト 256 KB、--sram-size で変更可)
0xC00000+           Flash (デフォルト 512 KB、--flash-size で変更可)
```

---

## ホストユーティリティ / Host Utilities

エミュレータ本体とは別に、P/ECE フラッシュイメージ（PFI）を操作するためのコマンドラインツールが `src/tools/` に含まれています。

In addition to the emulator, `src/tools/` provides command-line tools for managing P/ECE flash images (PFI).

### mkpfi — PFI フラッシュイメージ作成 / Create PFI Flash Image

P/ECE カーネルバイナリ（`all.bin`）から PFI フラッシュイメージを生成します。

Creates a PFI flash image from a raw P/ECE kernel binary (`all.bin`).

```sh
mkpfi [-512kb|-2mb] all.bin [piece.pfi]
```

| オプション | 説明 |
|---|---|
| `-512kb` | 512 KB フラッシュ用イメージを生成（デフォルト）/ 512 KB flash image (default) |
| `-2mb` | 2 MB フラッシュ用イメージを生成（改造 P/ECE 向け）/ 2 MB flash image (for modded P/ECE) |
| `all.bin` | P/ECE カーネルバイナリ（必須）/ P/ECE kernel binary (required) |
| `piece.pfi` | 出力ファイル名（省略時: `piece.pfi`）/ Output filename (default: `piece.pfi`) |

### pfar — PFFS アーカイバ / PFFS Archiver

PFI フラッシュイメージ内の PFFS ファイルシステムを操作します。
ゲームアプリ（`.pex`）などのファイルを追加・削除・抽出できます。

Manages files inside a PFI flash image's PFFS filesystem.
Add, delete, or extract game applications (`.pex`) and other files.

```sh
pfar piece.pfi [-a|-d|-e|-l|-v] [file [...]]
```

| オプション | 説明 |
|---|---|
| `-a` | ファイルを PFFS に追加 / Add file(s) to PFFS |
| `-d` | ファイルを PFFS から削除 / Delete file(s) from PFFS |
| `-e` | ファイルを PFFS からディスクに展開 / Extract file(s) to disk |
| `-l` | PFFS ディレクトリを一覧表示（デフォルト）/ List PFFS directory (default) |
| `-v` | PFI システム情報を表示 / Show PFI system info |

```sh
# PFFS 内のファイル一覧を表示
pfar piece.pfi -l

# ゲームを追加
pfar piece.pfi -a mygame.pex

# ゲームを展開
pfar piece.pfi -e mygame.pex

# システム情報（H/W バージョン・BIOS・クロック・メモリマップ）を表示
pfar piece.pfi -v
```

### ripper — 実機フラッシュ吸い出し / Real-Hardware Flash Ripper

USB 接続した P/ECE 実機からフラッシュ全域を読み出し、PFI ファイルとして保存します。
libusb-1.0 が必要です（Linux/macOS: システムパッケージ、Windows: vcpkg `ripper` フィーチャー）。

Reads the full flash contents from a USB-connected P/ECE and saves it as a PFI file.
Requires libusb-1.0 (Linux/macOS: system package; Windows: vcpkg `ripper` feature).

```sh
ripper [output.pfi]
# Default output: piece.pfi
```

P/ECE USB の VID/PID は `0x0e19` / `0x1000` です。Linux では root 権限またはデバイスアクセスを許可する udev ルールが必要です。

The P/ECE USB VID/PID is `0x0e19` / `0x1000`. On Linux, root access or a udev rule granting access to the device is required.

```sh
# udev ルール例 / Example udev rule (/etc/udev/rules.d/99-piece.rules)
SUBSYSTEM=="usb", ATTR{idVendor}=="0e19", ATTR{idProduct}=="1000", MODE="0666"
```

---

## テスト / Testing

### C++ ユニットテスト / C++ Unit Tests

```sh
ninja -C build-src test
```

161 テストが通過します（`piece_core_tests` 107 + `piece_soc_tests` 54）。CPU 命令・逆アセンブラ・PSR フラグ・BCU・INTC・T8・T16・PortCtrl・RTC・SIF3+HSDMA・PWM Sound を網羅。

161 tests pass (`piece_core_tests` 107 + `piece_soc_tests` 54). Covers CPU instructions, disassembler, PSR flags, BCU, INTC, T8, T16, PortCtrl, RTC, SIF3+HSDMA, and PWM Sound.

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

### ジェネレータ方式リグレッション試験 / Generator-based Regression Suite

Python ジェネレータが Python オラクルつきのインラインアセンブリ C テストを出力し、
命令セット全体を網羅する 1,659 ケースのリグレッションスイートを生成します。
Python generators emit inline-asm C tests with a Python oracle per case,
producing a 1,659-case regression suite covering the full CPU instruction set.

```sh
cd src/tests/bare_metal/gen && make && make run
```

網羅範囲 / Coverage: ALU (253)・memory (129)・SP-relative (72)・shift/rotate+misc (628)・
mul/div (69)・branch+delay slot (236)・pushn/popn (34)・mac (15)・bitops (164)・
special regs (37)・`call.d %rb` (6)・int/reti (16)。
失敗時はセミホスティング経由でケース ID が報告されます。
Failures report per-case IDs via semihosting.

本スイートの整備過程で、エミュレータおよび LLVM バックエンドで以下 3 件の
不具合が発見・修正されました（詳細は `git log`）。
Three bugs in the emulator / LLVM backend were uncovered and fixed during
development of this suite (see `git log` for details):

- **エミュレータ**: 符号付きステップ除算 (`div0s`/`div1`/`div2s`/`div3s`) が
  誤った商・剰余を生成していた。S1C33 の非復元型符号付き除算アルゴリズムに
  修正（legacy piemu から移植）。
  Emulator: signed step-division produced wrong quotient/remainder;
  now matches the S1C33 non-restoring signed-division algorithm.
- **エミュレータ**: `int imm2` 命令がトラップ番号 `imm2` へディスパッチしていたが、
  仕様は `12 + imm2`（SW 例外、ベクタは base+48..60）が正しい。修正済み。
  Emulator: `int imm2` was mapped to trap number `imm2` instead of `12 + imm2`.
- **LLVM バックエンド (S1C33)**: レジスタアロケータが `popn` を跨いで R0
  （callee-saved）を戻り値のステージングに用い、返り値を破壊するケースがあった。修正済み。
  LLVM backend: regalloc could stage the return value in R0 (callee-saved)
  across `popn`, clobbering it.

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
