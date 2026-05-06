# piece-emu — Aquaplus P/ECE Diagnostic Emulator

[アクアプラス P/ECE](https://aquaplus.jp/piece/) の diagnostic エミュレータです。実機では無視される無効な操作を積極的に検知して警告します。GDB RSP とセミホスティングにより、コンパイラ開発のデバッグ基盤として機能します。

未実装デバイスはあるものの（USB / 赤外線 / MMC / ADC / IDMA）、`piece.pfi` からカーネルブートしてゲームが起動・操作・発音し、PFFS への書き込み（セーブデータ等）はホスト PFI ファイルへ書き戻される状態に到達しています。

A diagnostic emulator for the [Aquaplus P/ECE](https://aquaplus.jp/piece/) handheld. Actively detects and warns about invalid operations that real hardware silently ignores. GDB RSP and semihosting make it a debugging foundation for compiler development.

Not every peripheral is emulated (USB, IrDA, MMC, ADC, IDMA), but games boot from `piece.pfi`, display on the LCD, accept input, produce audio, and persist PFFS writes (save data) back to the host PFI file.

---

## 概要 / Overview

| 項目 | 内容 |
|---|---|
| ターゲット CPU | EPSON S1C33000 (S1C33209) — 32-bit RISC, 16-bit fixed-width instructions |
| ターゲットデバイス | Aquaplus P/ECE |
| 言語 | C++20 |
| ビルドシステム | CMake + Ninja |
| ステータス | 全命令・主要周辺デバイス・GDB RSP（同期/非同期）・セミホスティング・HLT/SLEEP 高速スキップ・PFI ブート・SDL3 LCD 表示・ボタン入力・PWM サウンド出力・LLDB MCP 対応・4 スレッド構成（SDL/CPU/GDB/Audio）・O(1) タイマ解析（ホスト CPU ~50%）。実機 PFI からゲームを起動・操作・発音可能 |

---

## 設計方針 / Design Philosophy

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
| SST 39VF400A / 39VF160 | 0xC00000+ | CFI ステートマシン（Word-Program / Sector-Erase / Block-Erase / Chip-Erase / Software ID / CFI Query / Short Exit）、4KB セクタ dirty 追跡、ホスト PFI への書き戻し（POSIX `pwrite`+`fsync` / Win32 `WriteFile`+`FlushFileBuffers`、debounce、SIGINT/SIGTERM 対応）|

### 未実装 / 不完全 / Not Yet Implemented

- USB デバイス / USB device
- 赤外線通信（IrDA）/ IrDA
- MMC カードインタフェース / MMC card interface
- ADC / ADC
- IDMA（内部 DMA、HSDMA は実装済み）/ IDMA (HSDMA is implemented)
- Overlay PFI（base から copy-on-write で書き込みは別ファイルへ）/ Overlay PFI (copy-on-write writes to a separate file)

---

## PFI イメージを用意する / Preparing a PFI Image

エミュレータを動かすには、P/ECE のフラッシュメモリ内容を収めた PFI ファイル（`piece.pfi`）が必要です。
実機を持っていなくても、アクアプラス公式が配布しているシステムアップデータから **合法的に** 作成できます。
PFI 生成に使う `mkpfi` はバイナリ配布にも含まれています。

To run the emulator you need a PFI file (`piece.pfi`) — a flash-memory image of the P/ECE system area.
You can build one **legally** from the official Aquaplus system updater, even without real hardware.
The `mkpfi` tool used for this is included in the binary distribution.

1. [アクアプラス公式サイト — PieceSystem 1.20 ダウンロードページ](https://aquaplus.jp/piece/dl/update120.html) から
   [update120.exe](https://aquaplus.jp/piece/dl/update120.exe) を入手します。
   Get [update120.exe](https://aquaplus.jp/piece/dl/update120.exe) from the [official download page](https://aquaplus.jp/piece/dl/update120.html).

2. `update120.exe` は LHA 自己展開アーカイブです。Windows なら実行、Linux/macOS では `lha` で展開します。
   It's an LHA self-extractor. On Windows, run it; on Linux/macOS use `lha`.

   ```sh
   # Debian/Ubuntu: sudo apt install lhasa   (コマンド名は lha)
   # macOS (Homebrew): brew install lha
   lha x update120.exe
   ```

3. 展開すると `piece/update/all.bin`（512 KB フラッシュ、市販の実機と同等）または
   `piece/update/2mb/all.bin`（2 MB に改造された個体向け）が見つかります。
   You'll find `piece/update/all.bin` (512 KB flash — the factory configuration) or
   `piece/update/2mb/all.bin` (2 MB flash — for hand-modded units).

4. 同梱の `mkpfi` で `piece.pfi` を生成します。
   Run the bundled `mkpfi` to produce `piece.pfi`:

   ```sh
   mkpfi -512kb piece/update/all.bin piece.pfi
   # 2 MB 改造機なら: mkpfi -2mb piece/update/2mb/all.bin piece.pfi
   ```

5. 生成した `piece.pfi` をエミュレータに渡せば、ランチャーが起動します。まだゲーム（`.pex`）は
   入っていないので、同梱の `pfar` で追加してください（`pfar` の詳細は後述の
   [ホストユーティリティ](#ホストユーティリティ--host-utilities) 節を参照）。
   Pass the resulting `piece.pfi` to the emulator and the launcher will boot. No games (`.pex`) are
   installed yet — add them with the bundled `pfar` (see the
   [Host Utilities](#ホストユーティリティ--host-utilities) section below for details):

   ```sh
   pfar piece.pfi -a mygame.pex
   piece-emu-system piece.pfi
   ```

ゲームアプリは各作者のサイトで配布されているものを利用してください。
Game applications (`.pex`) should be obtained from their respective authors' distribution pages.

---

## システムエミュレータの使い方 / Running the System Emulator

SDL3 による LCD 表示・ボタン入力・オーディオを備えたフルシステムフロントエンド `piece-emu-system` の使い方です。PFI フラッシュイメージは positional 引数で指定します。

```sh
# フルシステムブート（LCD 表示・ボタン入力）
./build-src/piece-emu-system images/old/piece.pfi

# 表示スケール・補間モード指定
./build-src/piece-emu-system --scale 6 --scale-mode pixelart images/old/piece.pfi

# GDB RSP（LLDB MCP 対応、非同期モード）
./build-src/piece-emu-system images/old/piece.pfi --gdb-port 1234
lldb
(lldb) gdb-remote 1234

# オーディオ無効
./build-src/piece-emu-system --no-audio images/old/piece.pfi

# ゲームパッドの A/B を P/ECE 物理配置（右=A, 左=B）に合わせる
./build-src/piece-emu-system --swap-ab images/old/piece.pfi

# PFI 書き戻しを抑止（読み出し専用）/ Suppress host PFI writeback
./build-src/piece-emu-system --read-only images/old/piece.pfi

# 書き戻しまでの idle 時間を変更（既定 2000ms）/ Change writeback debounce
./build-src/piece-emu-system --writeback-debounce-ms 5000 images/old/piece.pfi
```

### Flash 書き戻し / Flash Writeback

`piece-emu-system` はカーネルが PFFS（セーブデータなど）に書き込んだ内容を、4KB セクタ単位でホストの PFI ファイルへ書き戻します。最後の書き込みから `--writeback-debounce-ms` ミリ秒（既定 2000ms）が経過したタイミングで `pwrite` + `fsync` を実行し、終了時には残った差分を強制 flush します。Ctrl-C / SIGTERM でも同じ shutdown 経路を踏むため、未保存のセーブデータが失われません。

`--read-only` を指定するとホストファイルは一切変更されません（書き込みは RAM 上では成功しますが、終了と同時に破棄されます）。

`piece-emu-system` writes back PFFS-level kernel writes (save data, etc.) to the host PFI file at 4 KB sector granularity. After `--writeback-debounce-ms` ms (default 2000 ms) of idle time following the most recent write, the dirty sectors are flushed via `pwrite` + `fsync`; any remaining dirty sectors are force-flushed at exit. Ctrl-C / SIGTERM walk the same shutdown path, so save data is not lost.

Pass `--read-only` to suppress host-file mutation entirely (writes still succeed in RAM but are discarded at exit).

ヘッドレスフロントエンド `piece-emu` ではテストの冪等性を確保するため、Flash 書き戻しは既定で **無効** です。`--enable-flash-writeback` で有効化できます（`--read-only` が常に優先）。

The headless `piece-emu` frontend keeps flash writeback **disabled by default** so test runs remain idempotent. Use `--enable-flash-writeback` to opt in (`--read-only` always wins).

### キー操作 / Keyboard Controls

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

### ゲームパッド操作 / Gamepad Controls

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

以下は開発者向けのセクションです（リポジトリ構成・ビルド手順・ヘッドレスモード・テスト・設計資料）。バイナリでエミュレータを動かすだけであれば、ここから先は読む必要はありません。

The sections below are for developers (repository layout, build instructions, headless mode, tests, and design documents). If you are only running the emulator from a prebuilt binary, you do not need to read beyond this point.

---

## リポジトリ構成 / Repository Layout

```
piece-emu/
├── src/
│   ├── core/                  libpiece_core.a — CPU コア・BCU・逆アセンブラ / CPU core, BCU, disassembler
│   ├── soc/                   libpiece_soc.a — S1C33209 オンチップ周辺デバイス / On-chip peripherals
│   ├── board/                 libpiece_board.a — 外付けデバイス（S6B0741 LCD 等）/ Board external devices
│   ├── debug/                 libpiece_debug.a — ELF/PFI ローダ・セミホスティング・GDB RSP
│   ├── host/                  libpiece_host.a — SDL3 フロントエンド（LCD・Audio・Screenshot）/ SDL3 frontend
│   ├── system/                piece-emu-system 内部モジュール（CPU runner・CLI 解析ほか）/ system binary internals
│   ├── tools/                 ホストユーティリティ（mkpfi, pfar, ripper）/ Host utilities
│   ├── tests/unit/            C++ ユニットテスト（GTest, core + soc）/ C++ unit tests
│   ├── tests/bare_metal/      S1C33 ベアメタルテスト（LLVM ツールチェーン使用）/ Bare-metal tests
│   └── tests/bare_metal/gen/  ジェネレータ方式リグレッション試験（1,659 ケース）/ Regression suite
├── docs/                      S1C33000 早見表・周辺実装状況・カーネル要点 / Reference docs (Japanese)
└── PIECE_EMULATOR_DESIGN.md   エミュレータ設計仕様書 / Emulator design specification
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

## Emulator Memory Map

```
0x000000–0x001FFF   IRAM  (8 KB, 0-wait)
0x030000–0x07FFFF   I/O + semihosting (0x060000)
0x100000–0x13FFFF   SRAM  (default 256 KB; overridable via --sram-size)
0xC00000+           Flash (default 512 KB; overridable via --flash-size)
```

---

## HLT/SLEEP 高速スキップ / Fast-forward on HLT

`slp`/`halt` 命令で CPU が停止すると、全周辺デバイスの `next_wake_cycle()` から
最早イベント時刻を計算し、サイクルカウンタをその時刻に飛ばしてから周辺デバイスを起こします。
カーネルの `ei; slp` パターンに対応し、実時間でのビジーウェイトを回避します。

When the CPU halts via `slp`/`halt`, the emulator fast-forwards to the earliest
`next_wake_cycle()` across all peripherals, fires the relevant interrupt, and resumes.
Supports the kernel's `ei; slp` pattern without busy-waiting.

---

## セミホスティング / Semihosting

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

## ヘッドレスモード / Headless Mode

ベアメタル ELF 実行、PFI によるフルシステムブート（表示なし）、自動テスト・CI・コンパイラ開発向けのフロントエンド `piece-emu` の使い方です。

```sh
# ベアメタル ELF を実行（終了コード 0=PASS）
./build-src/piece-emu test.elf

# PFI フラッシュイメージからフルシステムブート（ヘッドレス）
./build-src/piece-emu --pfi images/old/piece.pfi

# GDB RSP（同期モード）
./build-src/piece-emu --gdb test.elf &
lldb
(lldb) gdb-remote 1234

# デバッグオプション
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

### 同梱サードパーティコード / Bundled Third-Party Code

- [`src/third_party/stb/stb_image_write.h`](src/third_party/stb/stb_image_write.h)
  — Sean Barrett 氏による PNG 等のイメージ書き出しライブラリ（`stb_image_write` v1.16）。
  パブリックドメインで配布されており、入手元は
  [nothings/stb（GitHub）](https://github.com/nothings/stb) です。
  F12 スクリーンショット機能で使用しています。
  Sean Barrett's single-header image writer (`stb_image_write` v1.16), released into
  the public domain. Source: [nothings/stb on GitHub](https://github.com/nothings/stb).
  Used by the F12 screenshot feature.

---

## ライセンス / License

本プロジェクトのソースコードは [MIT ライセンス](LICENSE) の下で配布されています。
本プロジェクトのコードの大半は GitHub Copilot および Claude Code（Anthropic Claude を搭載した AI コーディングアシスタント）を用いて記述されています。

This project's source code is distributed under the [MIT License](LICENSE).
Most of the code in this project was written with the assistance of GitHub Copilot and Claude Code (an AI coding assistant powered by Anthropic Claude).
