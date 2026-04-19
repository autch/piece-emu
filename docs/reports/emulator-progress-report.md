# P/ECE エミュレータ実装進捗報告書

**対象設計仕様書**: `PIECE_EMULATOR_DESIGN.md`  
**報告日**: 2026-04-19  
**実装場所**: `src/`  
**ビルド方法**:
```bash
cmake -S src -B build-src -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=/home/autch/src/vcpkg/scripts/buildsystems/vcpkg.cmake
ninja -C build-src
```

---

## 概要

P0（CPU コア・メモリ・ELF ローダ・セミホスティング・GDB RSP）、P1（タイマ・割り込み・ポートコントローラ・クロック制御・BCU・WDT・RTC）、P2-1〜P2-4（PFI ローダ・SIF3・HSDMA・S6B0741・SDL3 フロントエンド・ボタン入力・PWM サウンド）が実装済みで、P/ECE カーネル (`piece.pfi`) を起動してゲームをウィンドウ表示・操作・サウンド再生できる状態にある。マルチスレッド構成 (main-SDL / CPU / GDB / audio) でフロントエンド応答性を確保。F12 PNG スクリーンショット、F5/Shift+F5 リセットも実装。

161 本のユニットテスト (piece_core_tests 107 + piece_soc_tests 54) が全て PASS し、9 本のベアメタル CPU テストも全て PASS することを確認した。

---

## P0 項目 — 実装状況

### ✅ CPUコア（命令デコード＋実行）

設計書 §7 の核心要件。

| 実装内容 | ファイル |
|----------|---------|
| 65536エントリ dispatch テーブル | `cpu_dispatch.cpp` |
| ext 蓄積→合成方式（方式2） | `cpu_core.cpp`, `cpu_impl.hpp` |
| PSR フラグ（N, Z, V, C, IE, DS, MO, IL） | `cpu.hpp` (`Psr` struct) |
| クラス別命令実装（Class 0〜6 全て） | `cpu_class0.cpp` 〜 `cpu_class6.cpp` |
| ディレイドブランチ（in_delay_slot 状態） | `cpu_class0.cpp`, `cpu_core.cpp` |
| 未定義命令ハンドラ（oops ダンプ、エミュレータ停止） | `cpu_core.cpp` (`h_undef`) |
| ext中 シフト命令の ext 無視 | `cpu_class4.cpp` |

**実装済み命令クラス**:
- Class 0a: NOP, SLP, HALT, PUSHN, POPN, BRK, RET/RET.D, RETD, INT, RETI, CALL, JP
- Class 0b: JR系条件分岐（JREQ/JRNE/JRLT/JRLE/JRGT/JRGE/JRULT/JRULE/JRUGT/JRUGE）, CALL simm8, JP simm8
- Class 1: SP相対・レジスタ間接 LD/ST（byte/halfword/word, sign/zero extend, post-increment）
- Class 2: SP相対ロード/ストア（imm6、スケール付き）
- Class 3: ADD/SUB/CMP/AND/OR/XOR/NOT（即値）、LD.W rd, simm6
- Class 4A: ADD/SUB SP, imm10（words単位スケーリング）
- Class 4B: SRL/SLL/SRA/SLA/RR/RL（imm4）
- Class 4C: SRL/SLL/SRA/SLA/RR/RL（レジスタ量）
- Class 4D: SCAN0/SCAN1/SWAP/MIRROR、DIV0S/DIV0U/DIV1/DIV2S/DIV3S
- Class 5A: LD.W %special/%rd（PSR/SP/ALR/AHR）、BTST/BCLR/BSET/BNOT [rb] imm3、ADC/SBC
- Class 5B: LD.B/LD.UB/LD.H/LD.UH %rd, %rs（レジスタ間 sign/zero extend）
- Class 5C: MLT.H/MLTU.H/MLT.W/MLTU.W（乗算）、MAC（積和演算）
- Class 6: ADD/SUB/CMP/AND/OR/XOR/NOT（レジスタ）、LD.W rd, rs

### ✅ メモリサブシステム

| 実装内容 | ファイル |
|----------|---------|
| 内蔵 IRAM 8 KB (0x000000–0x001FFF) | `bus.cpp` |
| SRAM (0x100000–0x13FFFF) | `bus.cpp` |
| Flash ROM (0xC00000〜) | `bus.cpp` |
| register_io() による I/O ハンドラ登録機構 | `bus.hpp`, `bus.cpp` |
| read8/write8/read16/write16/read32/write32 の全幅サポート | `bus.cpp` |
| SRAM ウォッチポイント + シャドウ SRAM（最終書き込みPC追跡） | `bus.hpp`, `bus.cpp` |

### ✅ ELFローダ（ベアメタル）

ELF プログラムヘッダの各 PT_LOAD セグメントを対応するバスアドレスに書き込み、エントリポイントから PC を開始する。`elf_loader.cpp`。

### ✅ PFI フラッシュイメージローダ

`pfi_loader.cpp`。`PFIHEADER` + `SYSTEMINFO` を解析し、フラッシュ領域 (0xC00000) に書き込む。`pfi_format.h` は C/C++ 共用の純粋 C ヘッダとして定義し、ホストツール (`mkpfi`) とも共有する。

### ✅ セミホスティングポート（最小 P0 セット）

設計書 §9 のうち最小限のポートを実装。`semihosting.cpp`。

| ポート | オフセット | 状態 |
|--------|-----------|------|
| CONSOLE_CHAR | +0x00 | ✅ 実装済み |
| CONSOLE_STR | +0x02 | ✅ 実装済み（低 16 bit アドレスのみ） |
| TEST_RESULT | +0x08 | ✅ 実装済み |
| CYCLE_COUNT_LO/HI | +0x0C/+0x10 | ❌ 未実装 |
| REG_SNAPSHOT 〜 MEM_CRC | +0x14〜+0x34 | ❌ 未実装 |

### ✅ 逆アセンブラ

設計書 §2.2 の蓄積→合成方式を実装。`cpu_disasm.cpp`。`--trace` フラグ時に各命令を逐次逆アセンブルして stderr 出力。

### ✅ GDB RSP デバッガ（拡張版）

`gdb_rsp.cpp/hpp`。GDB および LLDB（MCP）の両クライアントに対応。

| 機能 | 状態 |
|------|------|
| レジスタ読み出し / 書き込み (g/G/p/P) | ✅ |
| メモリ読み出し / 書き込み (m/M) | ✅ |
| 実行継続 / ステップ実行 (c/s/vCont) | ✅ |
| ソフトウェア・ハードウェアブレークポイント (Z0/Z1/z0/z1) | ✅ |
| ウォッチポイント (Z2/Z3/Z4 — write/read/rw) | ✅ |
| LLDB 拡張 (QStartNoAckMode, qHostInfo, qProcessInfo, qRegisterInfo) | ✅ |
| 非同期モード (serve_async / take_async_run_cmd / notify_async_stopped) | ✅ |
| 停止理由レポート (T05 + watch/rwatch/awatch アノテーション) | ✅ |

**同期モード** (`piece-emu --gdb`): RSP サーバが CPU ステップを直接制御するシングルスレッド動作。  
**非同期モード** (`piece-emu-system --gdb-port`): RSP サーバを別スレッドで起動し、SDL メインループが CPU ステップを担当。SDL ウィンドウを維持しながらデバッグ可能。

---

## P1 項目 — 実装状況

### ✅ 割り込みコントローラ (InterruptController)

`peripheral_intc.cpp`。39 割り込み源に対応。IEN・優先度チェック後に CPU トラップを発行。  
ISR クリア方式: RSTONLY=0 で直接書き込み (write-0-to-clear)、RSTONLY=1 で保護モード。

**ユニットテスト**: 8テスト PASS

### ✅ クロック制御 (ClockControl)

`peripheral_clkctl.cpp`。T8/T16 クロック周波数を CLKCTL/PWRCTL から算出。P07 書き込み時に 48MHz/24MHz を切り替え。

### ✅ 8bitタイマ × 4 (Timer8bit)

`peripheral_t8.cpp`。カウントダウン式、アンダーフロー割り込み、PSET 即時リロード。

**ユニットテスト**: 7テスト PASS

### ✅ 16bitタイマ × 6 (Timer16bit)

`peripheral_t16.cpp`。カウントアップ式、CRA/CRB 二段比較、PRESET 自己クリア。P/ECE カーネルは Ch0 CRB を 1ms タイマとして使用。

**ユニットテスト**: 10テスト PASS

### ✅ ポートコントローラ (PortCtrl)

`peripheral_portctrl.cpp`。K ポート（ボタン入力）・PINT（ポート入力割り込み）・P ポート（汎用 GPIO）を実装。

- K/P 全ハンドラが `addr & 1` を確認してバイトストアの hi/lo を個別更新（カーネルのバイト書き込み方式に対応）
- KEY0/KEY1 割り込み条件判定（SPPK/SCPK/SMPK レジスタ）
- `p21d()`: P2D bit1（S6B0741 RS 信号）を外部公開
- `set_k5()` / `set_k6()`: SDL3 フロントエンドからのボタン入力更新 API

**ユニットテスト**: 10テスト PASS

### ✅ BCUエリア設定 (BcuAreaCtrl)

`peripheral_bcu_area.cpp`。rA6_4 / rA10_9 のウェイト設定と rTTBR の CPU トラップテーブルアドレス更新。

### ✅ ウォッチドッグタイマ スタブ (WatchdogTimer)

`peripheral_wdt.cpp`。レジスタ R/W を吸収するスタブ（リセット動作なし）。

### ✅ クロックタイマ (RTC)

`peripheral_rtc.cpp`。1 秒割り込み（ベクタ 65、優先度 4）。カーネルの GetSysClock で使用。

**ユニットテスト**: 7テスト PASS

---

## P2 項目 — 実装状況

### ✅ シリアルI/F3 / LCD 転送 (Sif3)

`peripheral_sif3.cpp`。TXD 書き込みで `txd_callback` を呼び出した後、HSDMA Ch0 が有効ならインライン DMA ループを実行して残りバイトをまとめて転送（piemu 互換方式）。

**ユニットテスト**: 4テスト PASS

### ✅ 高速DMA (Hsdma)

`peripheral_hsdma.cpp`。4チャンネル分のレジスタ管理。

- **Ch0 (LCD)**: `do_ch0_inline()` で SIF3 TXD から呼び出し、バス読み出し→コールバックのループ
- **Ch1 (サウンド)**: EN 0→1 遷移で `on_ch1_start` を呼ぶ（P2-4 サウンド用フック）
- **Ch2/3**: レジスタ R/W のみ（副作用なし）

**ユニットテスト**: 5テスト PASS

### ✅ LCDコントローラ (S6b0741)

`src/board/s6b0741.cpp`。P/ECE ハードウェアのビット逆配線に対応してバイト反転。P21D 参照でコマンド/データを判定。VRAM 16ページ × 256カラム、`to_pixels()` で 2bit/pixel グレースケール変換。

### ✅ SDL3 フロントエンド (LcdRenderer + piece-emu-system)

`src/host/lcd_renderer.cpp` / `src/system_main.cpp`。

- SDL3 ウィンドウ（デフォルト 4倍 → 512×352）に 60fps で LCD 描画
- SDL3 キーイベント → K5D/K6D ビットマッピング（active-low）
- `piece-emu-system --pfi piece.pfi` でカーネルブートからゲーム画面を表示
- `--gdb-port N` で GDB RSP 非同期モードを起動し、SDL ウィンドウを維持しながらデバッグ可能
- F5 / Shift+F5 でホット / コールドスタート、F12 で PNG スクリーンショット保存

### ✅ マルチスレッド構成 (piece-emu-system)

`src/system/cpu_runner.cpp`、`src/system/piece_peripherals.cpp`、`src/system/lcd_framebuf.hpp`。

- `piece-sdl` (メインスレッド): SDL3 イベントポーリング + `LcdRenderer::render()`
- `piece-cpu` (`std::thread`): CPU ステップ + ペリフェラル tick + HSDMA Ch0 完了時に `LcdFrameBuf::push()`
- `piece-gdb` (オプション): GDB RSP 非同期サーバ
- SDL オーディオスレッド: `AudioOutput::audio_cb()` が `Sound::pop()` で SPSC リングから取得
- 共有状態: `LcdFrameBuf` (mutex)、`std::atomic<bool> quit_flag`、`std::atomic<uint16_t> shared_buttons`、Sound のリングバッファ

Windows ではメインスレッドスタックサイズを `/STACK:8388608` (8 MB) に設定（D3D11/D3D12 作成時のスタック消費対策）。

### ✅ PWM サウンド (Sound + AudioOutput)

`src/soc/peripheral_sound.cpp`、`src/host/audio_output.cpp`、`src/host/audio_log.cpp`。

- HSDMA Ch1 EN 0→1 で PWM サンプルを SADR から一括読み出し、SPSC リングバッファに格納 (4096 サンプル、128ms @ 32kHz)
- Ch1 完了 IRQ (vec 23) を `cnt*(cpu_hz/32000)` サイクル後にスケジュール配信。再入回避のため IL=1 で配信し INTC の level_override で IL 降下時に発火
- SDL3 `SDL_AudioStream` コールバックがリングから int16 を pull、ホストレートへのリサンプリングは SDL3 が担当
- `--audio-trace` で PULL / PUSH イベントを stderr にログ出力

**ユニットテスト**: 3 テスト PASS (リングバッファ挙動、サンプル生成、DMA 同期)

---

## コマンドラインインターフェース

```bash
# ベアメタル ELF 実行（TEST_RESULT で終了）
./build-src/piece-emu test.elf

# 全命令逆アセンブルトレース
./build-src/piece-emu --trace test.elf

# GDB RSP 接続待ち（同期モード）
./build-src/piece-emu --gdb 1234 test.elf

# サイクル数制限
./build-src/piece-emu --max-cycles 100000 test.elf

# SRAM 書き込みウォッチポイント（サイズ省略時 = 1バイト）
./build-src/piece-emu --wp-write 0x100000:4 test.elf

# 読み込み / 読み書き両用ウォッチポイント
./build-src/piece-emu --wp-read 0xADDR test.elf
./build-src/piece-emu --wp-rw 0xADDR test.elf

# 指定 PC 到達時にレジスタダンプ
./build-src/piece-emu --break-at 0xC01234 test.elf

# PFI フルシステム実行（SDL3 ウィンドウ）
./build-src/piece-emu-system --pfi piece.pfi

# PFI + GDB RSP 非同期モード（LLDB MCP 対応）
./build-src/piece-emu-system --pfi piece.pfi --gdb-port 1234

# RSP パケットトレース付き
./build-src/piece-emu-system --pfi piece.pfi --gdb-port 1234 --gdb-debug
```

終了コード: 0 = PASS（TEST_RESULT=0 またはクリーン停止）、1 = FAIL。

---

## テスト結果

### ユニットテスト（161テスト）

バイナリはライブラリ境界で 2 本に分割（Windows の gtest_discover_tests スタック
オーバーフロー回避のため）。両バイナリ合計 161 テスト / 13 スイートが PASS。

```
piece_core_tests : 107 tests / 6 suites
piece_soc_tests  :  54 tests / 7 suites
                 --- 161 total ---
```

| テストスイート | バイナリ | テスト数 | 内容 |
|-------------|---------|--------|------|
| ExtImmFixture | core | 16 | EXT即値拡張 |
| PsrFlags | core | 18 | PSRフラグ演算 |
| ShiftDecode | core | 9 | シフトデコード |
| DisasmFixture | core | 15 | 逆アセンブラ |
| BcuFixture | core | 21 | BCUメモリマップ |
| CpuInsnFixture | core | 28 | CPU命令実行 |
| IntcFixture | soc | 8 | 割り込みコントローラ |
| T8Fixture | soc | 7 | 8bitタイマ |
| T16Fixture | soc | 10 | 16bitタイマ |
| PortCtrlFixture | soc | 10 | ポートコントローラ |
| RtcFixture | soc | 7 | クロックタイマ (RTC) |
| Sif3Fixture | soc | 9 | シリアルI/F3 + HSDMA インライン DMA |
| SoundFixture | soc | 3 | PWM サウンド (HSDMA Ch1 → リングバッファ) |

### ベアメタル CPU テスト（9テスト）

`src/tests/bare_metal/` — LLVM S1C33 ツールチェーンでコンパイルしてエミュレータ上で実行。

| テスト | 内容 | 状態 |
|--------|------|------|
| test_basic.c | 基本 ALU（ADD/SUB/AND/OR/XOR/NOT/CMP） | ✅ PASS |
| test_ext_imm.c | ext 命令の即値拡張と符号規則 | ✅ PASS |
| test_alu_flags.c | PSR N/Z/V/C フラグ更新 | ✅ PASS |
| test_load_store.c | LD/ST 全幅、sign/zero extend、SP 相対 | ✅ PASS |
| test_shifts.c | SRL/SLL/SRA/SLA/RR/RL（即値・レジスタ量） | ✅ PASS |
| test_branches.c | 全条件分岐（10 種類） | ✅ PASS |
| test_multiply.c | MLT.H/MLTU.H/MLT.W/MLTU.W、MAC | ✅ PASS |
| test_div.c | DIV0U + 32×DIV1 ステップ除算 | ✅ PASS |
| test_misc.c | SCAN0/SCAN1/SWAP/MIRROR、ADC/SBC、BTST/BCLR/BSET/BNOT、PUSHN/POPN | ✅ PASS |

---

## 未実装（P2 残り / P3）

| 優先度 | コンポーネント | 状況 |
|--------|-------------|------|
| **P2-5** | Flash 書き込み（SST39VF400A コマンドシーケンス）| 未実装（読み取り専用） |
| **P2** | A/Dコンバータ スタブ (0x040240–0x040245) | 未実装（カーネルが参照する場合に追加） |
| **P2** | IDMA (0x048200–0x048207) | 未実装（P/ECE BIOS 未使用） |
| **P3** | USB (PDIUSBD12) | 未実装 |
| **P3** | 赤外線通信 | 未実装 |
| **P3** | MMC/SD | 未実装 |

セミホスティングポートも CYCLE_COUNT、REG_SNAPSHOT、TRACE_CTL（ポート経由）、BKPT_SET/CLR、HOST_TIME、MEM_CRC が未実装。

---

## 設計方針の遵守状況

| 設計方針 | 状況 |
|----------|------|
| **§2.1 シングルスレッド** | ⚠️ `piece-emu` (headless) はシングルスレッド。`piece-emu-system` は SDL/CPU/GDB/Audio の 4 スレッド構成（SDL レンダリングをメインスレッドに固定するため）。共有状態はすべて mutex / atomic / SPSC リング経由 |
| **§2.2 逆アセンブラ蓄積→合成方式** | ✅ |
| **§2.3 即値符号規則（ext は signedness を変更しない）** | ✅ ext_imm/ext_simm で分離実装 |
| **§2.4 ディレイドブランチ** | ✅ in_delay_slot フラグで実装 |
| **§2.5 GDB RSP** | ✅ ブレークポイント・ウォッチポイント・LLDB 拡張・非同期モード全実装 |
| **§2.6 未定義命令はエミュレータフォルト** | ✅ h_undef で oops ダンプ |
| **§2.7 65536エントリデコードテーブル** | ✅ cpu_dispatch.cpp |
| **§3 信頼性確立の順序** | ✅ P0→P1→P2 順で実装、各フェーズでテスト確認 |
| **エージェント統合（CLI完結, 終了コード, --trace）** | ✅ |
| **コード・コメントは英語** | ✅ src/ 配下は英語のみ |
