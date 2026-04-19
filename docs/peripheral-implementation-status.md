# C33コアペリフェラル実装状況

**最終更新**: 2026-04-19  
**対象ディレクトリ**: `src/core/`, `src/soc/`, `src/board/`, `src/host/`, `src/debug/`, `src/system/`  
**ビルドコマンド**: `ninja -C build-src` / `ninja -C build-src test`

---

## 実装済み（P1フェーズ）

### Step 1: ITickable インターフェース + TTBR 動的化

**ファイル**: [src/core/tick.hpp](../src/core/tick.hpp)

```cpp
class ITickable {
public:
    virtual void tick(uint64_t cpu_cycles) = 0;
    virtual ~ITickable() = default;
};
```

サイクル駆動のペリフェラル（タイマ類）が共通で実装するインターフェース。`cpu_cycles` は単調増加するCPU総サイクル数。

**ファイル**: [src/core/cpu.hpp](../src/core/cpu.hpp), [src/core/cpu_core.cpp](../src/core/cpu_core.cpp)

`CpuState::ttbr` フィールドを追加し、`do_trap()` 内のトラップテーブルアドレスをハードコードから動的化した。

```cpp
// cpu.hpp に追加
uint32_t ttbr = 0x400;  // Trap Table Base Register; P/ECE kernel sets 0x400
```

- デフォルト `0x400`: 既存のベアメタルテストに影響なし
- BcuAreaCtrl が rTTBR 書き込み時に `cpu.state.ttbr` を更新する

---

### Step 2: 割り込みコントローラ (InterruptController)

**ファイル**: [src/soc/peripheral_intc.hpp](../src/soc/peripheral_intc.hpp), [src/soc/peripheral_intc.cpp](../src/soc/peripheral_intc.cpp)

**レジスタマップ**: 0x040260–0x04029F (64バイト)

デバイスからの割り込み要求を受け取り、IEN・優先度チェックを経てCPUへ配送するハブ。

```cpp
// デバイスからの呼び出し
intc.raise(InterruptController::IrqSource::T16_CRA0);
```

- ISRフラグ: IEN・優先度に関わらず常にセット（GDBからのレジスタ確認に有用）
- IEN=0 または priority=0 の場合は assert_trap を呼ばない
- rRESET.RSTONLY=0 (default, kernel sets bp[0x29f]=0x06): direct write — write-0-to-clear (same as kernel's `&= ~mask` pattern); writing 1 force-sets the flag
- rRESET.RSTONLY=1: write-0-to-clear only — writing 1 has no effect (protected against software force-set)
- IrqSource ↔ (trap_no, pri_byte, pri_shift, ien_byte, ien_bit, isr_byte, isr_bit) 変換テーブルを保持

**実装済みソース**: 39種類

| グループ | IrqSource | Trap番号 |
|---------|-----------|---------|
| ポート入力 | PORT0–PORT3 | 16–19 |
| キー入力 | KEY0–KEY1 | 20–21 |
| HSDMA | HSDMA0–HSDMA3 | 22–25 |
| IDMA | IDMA | 26 |
| 16bitタイマ | T16_CRB0/CRA0〜T16_CRB5/CRA5 | 30–51 |
| 8bitタイマ | T8_UF0–T8_UF3 | 52–55 |
| シリアル | SIF0_ERR/RX/TX, SIF1_ERR/RX/TX | 56–62 |
| その他 | AD, CLK_TIMER | 64–65 |
| ポート入力2 | PORT4–PORT7 | 68–71 |

**ユニットテスト**: [src/tests/unit/test_peripheral_intc.cpp](../src/tests/unit/test_peripheral_intc.cpp) — 8テスト

---

### Step 3: クロック制御 (ClockControl)

**ファイル**: [src/soc/peripheral_clkctl.hpp](../src/soc/peripheral_clkctl.hpp), [src/soc/peripheral_clkctl.cpp](../src/soc/peripheral_clkctl.cpp)

**レジスタマップ**: 0x040146–0x04014E, 0x040180

タイマの入力クロック周波数を計算するコンポーネント。タイマが `cycles_per_count()` を求める際に参照する。

```
CLKCTL.TSx[2:0] → 分周比 2^(TSx+1)
CLKCTL.TONx     → クロック有効/無効
PWRCTL.CLKDT[1:0] (bits 7:6) → CPU分周 ×1 / ×1/2 / ×1/4 / ×1/8
PWRCTL.CLKCHG    (bit 2)     → 1=OSC3経路, 0=OSC1経路 (P/ECEでは常に1)
P07 (Port 0 bit 7)           → OSC3実周波数 1=24MHz, 0=48MHz
```

実効 CPU クロック:

```
OSC3 = (P07==1) ? 24 MHz : 48 MHz
CPU  = OSC3 >> CLKDT
```

- `cpu_clock_hz()`: P07 と PWRCTL.CLKDT から CPU クロックを返す
  （CLKCHG は無視 — P/ECE カーネルが常に 1 を維持する前提）
- `t8_clock_hz(ch)`: CLKSEL_T8 + CLKCTL_T8_01/23 から T8 チャンネルのクロックを返す
- `t16_clock_hz(ch, cksl)`: CLKCTL_T16_ch から T16 チャンネルのクロックを返す
- `on_clock_change` コールバック: CPU クロック変化時に通知（pace 再計算／タイマ wake point 更新）

### P07 と「48MHz 倍速モード」— 罠

**P07 の電源投入デフォルトは 1 (OSC3 = 24 MHz)**。P/ECE 回路図で P07/#SRDY1/#DMAEND3
ピンは 47kΩ (R114) で VDDE にプルアップされており、リセット直後は入力として
High にラッチされる。カーネルの `InitHard` がこのラッチ値を `a = bp[0x2d1];
bp[0x2d1] = a & 0x80;` で読み出し、同じ値を出力として書き戻すことで P07=1 が
そのまま保持される — **カーネルソースに「P07 に 1 を書く」明示的なコードは
存在しない**。

アプリは次の慣例で動く:

- 通常: OSC3 = 24 MHz のまま、`pceCPUSetSpeed()` で CLKDT を ×1/×1/2 に切り替える
- 「48MHz 倍速モード」: アプリが `bp[0x2d1] = 0x00` で P07=0 に落として
  OSC3 = 48 MHz に上げる。終了時に `bp[0x2d1] = 0x80` を書いて P07=1 に戻す。

エミュレータでの要件:

- `PortCtrl::pport_[1]` の初期値を `0x80` にしておく
  ([peripheral_portctrl.hpp](../src/soc/peripheral_portctrl.hpp))。これを 0 にすると
  カーネルが P07=0 をラッチしてしまい、アプリの「終了時 P07=1 書き込み」が
  OSC3 を 24 MHz に**落とす**動作になり、アプリ起動のたびに雪だるま式に遅くなる。
- `ClockControl::p07_slow_` の初期値も `true` にする。pport_[1] と整合する。

---

### Step 4: 8bitタイマ × 4 (Timer8bit)

**ファイル**: [src/soc/peripheral_t8.hpp](../src/soc/peripheral_t8.hpp), [src/soc/peripheral_t8.cpp](../src/soc/peripheral_t8.cpp)

**レジスタマップ**: 0x040160 + ch×4 (各4バイト、ch=0–3)

| オフセット | レジスタ | 用途 |
|-----------|---------|------|
| +0 (lo) | rT8CTL | PTRUN[0], PSET[1], PTOUT[2] |
| +1 (hi) | rRLD | リロード値 |
| +2 (lo) | rPTD | カウンタ（読み取り専用） |
| +3 (hi) | Dummy | — |

- カウントダウン式: PTD が 0 からデクリメント → アンダーフロー → PTD=RLD + IRQ
- PSET=1 書き込み: 即座に PTD=RLD（カウントは継続）
- PTRUN=0: カウント停止
- クロック停止 (TONA=0): カウント停止

**ユニットテスト**: [src/tests/unit/test_peripheral_t8.cpp](../src/tests/unit/test_peripheral_t8.cpp) — 7テスト

---

### Step 5: 16bitタイマ × 6 (Timer16bit)

**ファイル**: [src/soc/peripheral_t16.hpp](../src/soc/peripheral_t16.hpp), [src/soc/peripheral_t16.cpp](../src/soc/peripheral_t16.cpp)

**レジスタマップ**: 0x048180 + ch×8 (各8バイト、ch=0–5)

| オフセット | レジスタ | 用途 |
|-----------|---------|------|
| +0 | rCRA | 比較データA |
| +2 | rCRB | 比較データB |
| +4 | rTC | カウンタ（読み書き可） |
| +6 (lo) | rT16CTL | 制御レジスタ |

rT16CTL ビット: PRUN[0], PRESET[1], PTM[2], CKSL[3], OUTINV[4], SELCRB[5], SELFM[6]

- カウントアップ式: TC==CRB → raise_crb（TCはリセットされない）、TC==CRA → raise_cra + TC=0（SELFM=0時）
- PRESET=1 書き込み: TC=0 かつ next_tick_cycle_=0 にリセット（自己クリア）
- P/ECE カーネルはチャンネル0 CRA で約1msタイマを構成する

**ユニットテスト**: [src/tests/unit/test_peripheral_t16.cpp](../src/tests/unit/test_peripheral_t16.cpp) — 10テスト

---

### Step 6: ポートコントローラ (PortCtrl)

**ファイル**: [src/soc/peripheral_portctrl.hpp](../src/soc/peripheral_portctrl.hpp), [src/soc/peripheral_portctrl.cpp](../src/soc/peripheral_portctrl.cpp)

**レジスタマップ**:
- K port: 0x0402C0–0x0402C4 (rCFK5, rK5D, Dummy, rCFK6, rK6D)
- PINT: 0x0402C6–0x0402CF (ポート入力割り込み設定)
- P port: 0x0402D0–0x0402DF (P0–P3 汎用GPIO)

P/ECE ボタン配線:
```
K5[0]=UP, K5[1]=DOWN, K5[2]=LEFT, K5[3]=RIGHT, K5[4]=SELECT
K6[0]=A,  K6[1]=B,   K6[2]=START
```

- `set_k5(bits)` / `set_k6(bits)`: フロントエンド（SDL3）からの入力更新
- K5D・K6D は CPU 書き込みを無視（入力専用）
- KEY0/KEY1 割り込み: `(Kxd & ~SMPK) == (SCPK & ~SMPK)` 条件でトリガ（SPPK で K5/K6 を選択）

**ユニットテスト**: [src/tests/unit/test_peripheral_portctrl.cpp](../src/tests/unit/test_peripheral_portctrl.cpp) — 10テスト

---

### Step 7: BCUエリア設定 (BcuAreaCtrl)

**ファイル**: [src/soc/peripheral_bcu_area.hpp](../src/soc/peripheral_bcu_area.hpp), [src/soc/peripheral_bcu_area.cpp](../src/soc/peripheral_bcu_area.cpp)

**レジスタマップ**: 0x048120–0x04813B

- rA6_4 (0x04812A): A5WT[2:0] → `bus.sram_wait` を更新
- rA10_9 (0x048126): A10WT[2:0] → `bus.flash_wait` を更新
- rTTBR (0x048134 lo + 0x048136 hi): `cpu.state.ttbr` を更新
- コールドスタートデフォルト: 全AxWT=7（最大ウェイト）

---

### Step 8: ウォッチドッグタイマ スタブ (WatchdogTimer)

**ファイル**: [src/soc/peripheral_wdt.hpp](../src/soc/peripheral_wdt.hpp), [src/soc/peripheral_wdt.cpp](../src/soc/peripheral_wdt.cpp)

**レジスタマップ**: 0x040170–0x040171 (rWRWD + rEWD)

読み書きを吸収するだけのスタブ。P/ECE カーネルが WDT を有効化するが、エミュレータはリセット動作を行わない。

---

### Step 9: クロックタイマ (RTC)

**ファイル**: [src/soc/peripheral_rtc.hpp](../src/soc/peripheral_rtc.hpp), [src/soc/peripheral_rtc.cpp](../src/soc/peripheral_rtc.cpp)

**レジスタマップ**: 0x040151–0x04015B

1秒割り込み（ベクタ 65, 優先度 4）でアラーム機能を提供。カーネルが使用。

**ユニットテスト**: [src/tests/unit/test_peripheral_rtc.cpp](../src/tests/unit/test_peripheral_rtc.cpp)

---

### Step 10: CMakeLists.txt + main.cpp 更新

**ファイル**: [src/CMakeLists.txt](../src/CMakeLists.txt), [src/soc/CMakeLists.txt](../src/soc/CMakeLists.txt), [src/main.cpp](../src/main.cpp)

`piece_soc` に上記ペリフェラルのコンパイル単位を追加。  
`main.cpp` に以下を追加:
- 全ペリフェラルのインスタンス化・`attach()` 呼び出し
- CPU ステップ後に T8×4, T16×6 の `tick(total_cycles)` を呼ぶループ

---

## テスト結果

```
[==========] 161 tests from 13 test suites ran.
[  PASSED  ] 161 tests.
```

テストバイナリはライブラリ境界で2本に分割（Windows の gtest_discover_tests スタック
オーバーフロー回避のため）:

- `piece_core_tests` — CPU / BCU / EXT / PSR / shift / disasm (107 テスト)
- `piece_soc_tests` — オンチップ周辺 (INTC / T8 / T16 / PortCtrl / RTC / SIF3+HSDMA / Sound) (54 テスト)

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
| Sif3Fixture | soc | 9 | シリアルI/F3 (LCD) + HSDMA インライン DMA |
| SoundFixture | soc | 3 | PWM サウンド (HSDMA Ch1 → リングバッファ) |

---

## SDK実機資料から判明した重要事項

> ソース: `docs/piece-cd/PIECE ハードウエア割り込み.html`, `PIECE ポート解説.html`  
> (BIOS ver1.12 時点)

### P/ECE BIOS による割り込み使用状況

| ベクタ | 周辺機能 | BIOSでの用途 | 優先度 |
|--------|---------|------------|--------|
| 7 (NMI) | ウォッチドッグ | 1msタイマカウント専用 | NMI |
| 17 | PORT1 (K51=USB INT-N) | USB INT | 6 |
| 18 | PORT2 (赤外線受信) | 赤外線受信 | 7 |
| 20 | KEY0 | スタンバイ解除 | 4 |
| 22 | HSDMA Ch.0 | LCD転送 (割り込みは未使用、DMA動作のみ) | — |
| 23 | HSDMA Ch.1 | サウンド再生 (PWM値転送) | 7/5 |
| **30** | **T16 Ch.0 CRB** | **1msタイマ (contextsw + timer_sub)** | **6** |
| 31 | T16 Ch.0 CRA | kernel では未使用（GetSysClock 計測のみ）| — |
| 34, 35 | T16 Ch.1 CRB/CRA | サウンド再生 (PWM) | — |
| 46, 47 | T16 Ch.4 CRB/CRA | USB 6MHz クロック供給 | — |
| 50, 51 | T16 Ch.5 CRB/CRA | 赤外線送信 | 7 |
| 52 | T8 Ch.0 UF | スタンバイ解除 | — |
| 65 | 計時タイマ | アラーム | 4 |

**NMI (trap 7) の実際の発生源**:  
`sdk/sysdev/pcekn/timer.c` の `InitTimer()` を確認した結果:
- `pceVectorSetTrap(7, nmi_isr)` → NMI ハンドラは ClockTicks++ のみ行うシンプルな関数
- `pceVectorSetTrap(30, timer_isr)` → T16 CRB0 ハンドラ（ISR クリア → contextsw → timer_sub）
- WDT を有効化 (`bp[0x170]=0x80, bp[0x171]=0x02`) → WDT が NMI を発生させる  

つまり **T16 CRB0 は trap 30 経由** で timer_isr を呼ぶ。**NMI (trap 7) は WDT から独立して発生**する。  
両者は完全に別のパス。T16 CRB0 と NMI の混同は誤りだった。

**ISR クリア方式の確認**:  
`timer_isr` が `*(unsigned char *)0x40282 &= ~4` でビット2をクリアする。  
bp[0x29f]=0x06 で RSTONLY=0（直接書き込み）。`&= ~4` はビット2を0にする read-modify-write → write-0-to-clear。これは RSTONLY=0 の「直接書き込み」モードと一致する。

### K ポート実際の配線 (ポート解説より)

K5 ポート (K5D レジスタ = 0x0402C1):

| ビット | 信号 | 種別 |
|-------|------|------|
| K50 | USB SUSPEND | 入力（非ボタン） |
| K51 | USB INT-N | 入力（非ボタン） |
| K52 | 赤外線受信データ | 入力 |
| **K53** | **ボタン SELECT** | 入力 (0=押下) |
| **K54** | **ボタン START** | 入力 (0=押下) |

K6 ポート (K6D レジスタ = 0x0402C4):

| ビット | 信号 | 種別 |
|-------|------|------|
| **K60** | **ボタン右** | 入力 (0=押下) |
| **K61** | **ボタン左** | 入力 |
| **K62** | **ボタン下** | 入力 |
| **K63** | **ボタン上** | 入力 |
| **K64** | **ボタン B** | 入力 |
| **K65** | **ボタン A** | 入力 |
| K66 | 電池電圧 ADC | アナログ入力 |
| K67 | Di電圧 ADC | アナログ入力 |

> **注意**: peripheral_portctrl.hpp の旧コメントは誤りだったため修正済み。  
> SDL3 フロントエンド実装時は上記配線に従うこと。ボタンはすべて **active-low** (0=押下)。

### P ポート主要信号

| ポート | 信号 | 用途 |
|--------|------|------|
| P07 | OSC3クロック制御 | 0=48MHz, 1=24MHz → ClockControl 連携要。**リセットデフォルト=1** (47kΩ で VDDE にプルアップ)。詳細は上記「P07 と 48MHz 倍速モード」節を参照 |
| P15 | LCD SLCK | SIF3 SCLK3 |
| P16 | LCD SID | SIF3 SOUT3 |
| P20 | LCD CSB (OUT) | SRDY3 制御 (P32と直結) |
| P21 | LCD RS | コマンド/データ選択 (0=データ, 1=コマンド) |
| P23 | オーディオ出力 | T16 Ch.1 PWM出力 |
| P26 | USB クロック 6MHz | T16 Ch.4 出力 |
| P33 | LCD RESET | 0=リセット, 1=動作 |

---

## 実装済み（P2フェーズ）

### Step 11: シリアルI/F3 (Sif3)

**ファイル**: [src/soc/peripheral_sif3.hpp](../src/soc/peripheral_sif3.hpp), [src/soc/peripheral_sif3.cpp](../src/soc/peripheral_sif3.cpp)

**レジスタマップ**: 0x0401F4–0x0401F9

| アドレス | レジスタ | 用途 |
|---------|---------|------|
| 0x0401F4 | rSIF2_IRDA (lo) | SIF2 IrDA — ハンドラ共有 |
| 0x0401F5 | rTXD | 送信データ — 書き込みで LCD 転送トリガ |
| 0x0401F6 | rRXD | 受信データ — 0x00 返却 (スタブ) |
| 0x0401F7 | rSTATUS | ステータス — bit1=TDBE 常に 1 |
| 0x0401F8 | rCTL | 制御 (R/W) |
| 0x0401F9 | rIRDA | IrDA (R/W) |

TXD 書き込み時:
1. `txd_callback(byte)` を呼ぶ（S6B0741 に接続）
2. HSDMA Ch0 が有効なら `hsdma_.do_ch0_inline(bus, txd_callback)` を呼びインライン DMA を実行

**ユニットテスト**: [src/tests/unit/test_peripheral_sif3.cpp](../src/tests/unit/test_peripheral_sif3.cpp) — 4テスト

---

### Step 12: 高速DMA (Hsdma)

**ファイル**: [src/soc/peripheral_hsdma.hpp](../src/soc/peripheral_hsdma.hpp), [src/soc/peripheral_hsdma.cpp](../src/soc/peripheral_hsdma.cpp)

**レジスタマップ**: 0x048220 + ch×0x10 (ch=0–3), 各チャンネル 16バイト

| オフセット | レジスタ | 用途 |
|-----------|---------|------|
| +0x0 | rCNT | 転送カウンタ (32bit) |
| +0x4 | rSADR | ソースアドレス (32bit) |
| +0x8 | rDADR | デスティネーションアドレス (32bit) |
| +0xC | rHSEN | 有効フラグ bit[0]=HS_EN (16bit) |
| +0xE | rTF | トリガフラグ bit[0]=HS_TF (16bit) |

- **Ch0 (LCD)**: SIF3 TXD 書き込みからインライン DMA を実行。`do_ch0_inline()` が `ch0_sadr` から `ch0_cnt` バイトを読み `txd_callback` に渡す
- **Ch1 (サウンド)**: EN に 0→1 遷移で `on_ch1_start(bus, sadr, cnt)` を呼ぶ（P2-4 サウンド用）
- **Ch2/3**: レジスタ R/W のみ（副作用なし）

**ユニットテスト**: [src/tests/unit/test_peripheral_hsdma.cpp](../src/tests/unit/test_peripheral_hsdma.cpp) — 5テスト

---

### Step 13: LCDコントローラ (S6b0741)

**ファイル**: [src/board/s6b0741.hpp](../src/board/s6b0741.hpp), [src/board/s6b0741.cpp](../src/board/s6b0741.cpp)

P/ECE のハードウェア配線によりデータバスのビット順序が逆になっているため、受信バイトをビット反転してから処理する。

RS 信号 (コマンド/データ選択) は P21 (PortCtrl port 2 bit 1) で判定:
- P21=0 → コマンドバイト
- P21=1 → データバイト

コマンド解析（ビット反転後）:
```
0x00–0x0F  列アドレス下位: col2 = (col2 & 0xE0) | (cmd & 0x0F) << 1
0x10–0x17  列アドレス上位: col2 = (col2 & 0x1E) | (cmd & 0x07) << 5
0xB0–0xBF  ページアドレス: page = cmd & 0x0F
```

VRAM: `uint8_t vram_[16][256]` — 16ページ × 256カラム。  
`to_pixels(out[88][128])`: 2bit/pixel → 4階調グレースケール変換（piemu の `lcdc_conv()` と同等）。

---

### Step 14: SDL3 LCDレンダラ (LcdRenderer)

**ファイル**: [src/host/lcd_renderer.hpp](../src/host/lcd_renderer.hpp), [src/host/lcd_renderer.cpp](../src/host/lcd_renderer.cpp)

SDL3 ウィンドウ (デフォルト 4倍 → 512×352) に S6B0741 VRAM を表示する。

- `init(scale)`: SDL3 ウィンドウ / レンダラ / SDL_Texture を生成
- `render(pixels[88][128])`: 2bpp グレースケールを ARGB テクスチャに変換し拡大描画
- `poll_events(key_cb)`: SDL3 イベントポーリング。ウィンドウクローズ時 false を返す
- SDL3 が見つからない場合はスタブ INTERFACE ターゲットを提供（CI ビルド対応）

---

### Step 15: piece-emu-system フルシステムフロントエンド

**ファイル**: [src/system_main.cpp](../src/system_main.cpp)

SDL3 ウィンドウ + GDB RSP 非同期モードを統合したフルシステムフロントエンド。

- `--pfi <file>`: PFI フラッシュイメージを読み込んで起動
- `--gdb-port <port>` / `--gdb-debug`: GDB RSP サーバを起動 (デフォルトポート 1234)
- GDB クライアント接続時は RSP 非同期モードで CPU ステップを制御
- 非接続時は 60fps フレームレートでフリーラン
- SDL3 キーイベント → K5D/K6D ビットマッピング（active-low ボタン入力）

GDB RSP 非同期モードの仕組み:
```
RSP スレッド: serve() が async_run_cmd_ を set → async_stopped_cv_ でブロック
メインループ: take_async_run_cmd() でコマンド取得 → CPU ステップ → notify_async_stopped()
```

---

### Step 17: PWM サウンド (Sound + AudioOutput)

**ファイル**:
- [src/soc/peripheral_sound.hpp](../src/soc/peripheral_sound.hpp),
  [src/soc/peripheral_sound.cpp](../src/soc/peripheral_sound.cpp) — SoC 側ドライバ
- [src/host/audio_output.hpp](../src/host/audio_output.hpp),
  [src/host/audio_output.cpp](../src/host/audio_output.cpp) — SDL3 オーディオシンク
- [src/host/audio_log.hpp](../src/host/audio_log.hpp),
  [src/host/audio_log.cpp](../src/host/audio_log.cpp) — `--audio-trace` 診断ログ

HSDMA Ch1 EN 0→1 のタイミングで `Sound::handle_ch1_start()` が `cnt` 本の PWM
サンプルを SADR から一括読み出し、SPSC リングバッファ (4096 サンプル、128ms @ 32kHz)
に格納する。HSDMA Ch1 完了割り込み (vec 23) は `cnt*(cpu_hz/32000)` サイクル後に
スケジュールされ、`tick()` で遅延配信する (IL=1 で配信して再入回避)。

SDL3 側は `AudioOutput::open()` が `SDL_AudioStream` コールバックを登録し、
コールバックスレッドが `Sound::pop()` でリングから int16 サンプルを取り出す。
ホストデバイスレートへのリサンプリングは SDL3 の `SDL_AudioStream` が担当。

ネイティブ出力レート: 32 kHz / モノラル / int16。  
設計上の注意: T16 Ch.1 は SELFM=1 PWM モードで動くが T16 エミュレーションは
PWM コンペアを正確にはモデル化していない。EN 同期で全サンプル先読みすることで
T16 依存を回避し、カーネルの 4ms 完了サイクルを担保している (ダブルバッファ
方式により `make_xwave()` と DMA の重なりは実機と挙動同じ)。

**ユニットテスト**: [src/tests/unit/test_peripheral_sound.cpp](../src/tests/unit/test_peripheral_sound.cpp) — 3テスト

---

### Step 18: フロントエンド補助機能

**ファイル**:
- [src/host/screenshot.hpp](../src/host/screenshot.hpp),
  [src/host/screenshot.cpp](../src/host/screenshot.cpp) — F12 で PNG 保存 (stb_image_write)
- [src/system/lcd_framebuf.hpp](../src/system/lcd_framebuf.hpp) — CPU→メイン
  スレッド間の共有ピクセルバッファ (mutex 保護、最新フレーム勝ち)
- [src/system/cpu_runner.hpp](../src/system/cpu_runner.hpp),
  [src/system/cpu_runner.cpp](../src/system/cpu_runner.cpp) — CPU 実行スレッド
- [src/system/button_input.hpp](../src/system/button_input.hpp),
  [src/system/button_input.cpp](../src/system/button_input.cpp) — SDL3 キー→K5/K6 マップ
- [src/system/cli_config.hpp](../src/system/cli_config.hpp),
  [src/system/cli_config.cpp](../src/system/cli_config.cpp) — CLI11 ベースのオプション解析
- [src/system/piece_peripherals.hpp](../src/system/piece_peripherals.hpp),
  [src/system/piece_peripherals.cpp](../src/system/piece_peripherals.cpp) — 全ペリフェラル集約 + `attach()` / `reset()`

キーバインド:
- F5: ホットスタート (CPU + オンチップ周辺をリセット)
- Shift+F5: コールドスタート (BCU エリア / PortCtrl / LCD も含めてリセット)
- F12: PNG スクリーンショット保存 (`piece_YYYYMMDD_HHMMSS_mmm.png`)
- Escape: 終了

スレッドモデル (詳細は `CLAUDE.md` 参照):
- `piece-sdl` (メイン): SDL3 イベントポーリング + `LcdRenderer::render()`
- `piece-cpu` (`std::thread`): `CpuRunner::run()` = CPU ステップ + ペリフェラル tick
- `piece-gdb` (オプション): GDB RSP 非同期サーバ
- SDL オーディオスレッド: `AudioOutput::audio_cb()` が `Sound::pop()` を pull

共有状態: `LcdFrameBuf` (mutex)、`std::atomic<bool> quit_flag`、
`std::atomic<uint16_t> shared_buttons`、Sound の SPSC リングバッファ。

---

### Step 16: PortCtrl バイト書き込み対応 + P21D メソッド

**ファイル**: [src/soc/peripheral_portctrl.cpp](../src/soc/peripheral_portctrl.cpp)

P/ECE カーネルはポートデータレジスタを奇数アドレスへのバイト書き込みで更新する（例: `bp[0x2d9] |= 0x02`）。Bus が奇数アドレス書き込みを 8bit ストアとして IO ハンドラに渡すため、ハンドラが `addr & 1` を確認してバイト位置を識別する必要がある。

すべての K ポート / P ポートのハンドラが `addr & 1` チェックで hi/lo バイトを個別に更新するよう修正済み。

追加メソッド:
```cpp
// P2D レジスタ bit1 を返す（S6B0741 の RS 信号 = LCD コマンド/データ選択）
uint8_t p21d() const { return (pd(2) >> 1) & 1; }
```

---

## 未実装（今後のフェーズ）

### P2: IDMA

**アドレス**: 0x048200–0x048207

複雑な汎用 DMA。P/ECE BIOS では未使用。将来のシステムモードブートで必要になる可能性あり。

---

### P2: A/Dコンバータ スタブ

**アドレス**: 0x040240–0x040245

K66（電池電圧）・K67（Di電圧）の ADC 測定。カーネルが初期化時に参照する可能性あり。  
P/ECE BIOS がシステムで全 ADC チャンネルを占有（ユーザ利用不可）。

---

### P2-5: Flash 書き込み (SST39VF400A)

**必要条件**: セーブデータ書き込み対応

3段階アンロックコマンド (0xaa→0x55→cmd) のステートマシン実装と PFI ファイルへの書き戻し。  
実装ファイル（未作成）: `src/board/flash_sst39vf.{hpp,cpp}`

---

## 次のステップ

1. **P2-5 Flash 書き込み**: SST39VF400A コマンドステートマシン + PFI 書き戻し
2. **ADC スタブ**: カーネルが `GetVbatt()` を呼ぶ場合に追加（固定値返却）
3. **IDMA**: 将来的にシステムモードブートで必要になる可能性

---

## 既知の設計上の注意点

### `bus.write16` のバイト順序

INTC などのレジスタを `bus.write16(addr, val)` で設定するとき、`val` の **lo バイト** が `addr` に、**hi バイト** が `addr+1` に書かれる（リトルエンディアン）。

```cpp
// 例: INTC_BASE+8 の halfword → lo=byte8, hi=byte9
bus.write16(INTC_BASE + 8, 0x0300); // byte8=0x00, byte9=0x03  ← 意図通り
bus.write16(INTC_BASE + 6, 0x0300); // byte6=0x00, byte7=0x03  ← byte6 に書きたい場合はこれは間違い
bus.write16(INTC_BASE + 6, 0x0003); // byte6=0x03, byte7=0x00  ← 正しい
```

テスト作成時に混同しやすい点。ペリフェラルの「どのバイトに何を書くか」を常に意識すること。

### T8 の PSET タイミング

`bus.write16(T8_BASE, ctl_rld_word)` を1回で書くと、`on_ctl_write(ctl)` が呼ばれた時点では `rld_` がまだ更新されていない（lo バイトが先に処理されるため）。テストで PSET を使う場合は **先に RLD を書いてから CTL を書く**こと。

```cpp
bus.write16(T8_BASE, static_cast<uint16_t>(rld) << 8); // RLD=rld, CTL=0
bus.write16(T8_BASE, ctl | (static_cast<uint16_t>(rld) << 8)); // CTL=ctl (PSET有効)
```

### T16 の CRA/CRB 共有優先度

T16_CRA0 と T16_CRB0 は `pri_byte=6, pri_shift=0` を共有する（同じ3bitフィールドで優先度を設定）。IEN3 で個別に有効化できる（bit3=CRA0, bit2=CRB0）が、両方同時に有効化するには1回の `write16` で両ビットをセットすること（順番に書くと後の書き込みが前を上書きする）。

```cpp
bus.write16(INTC_BASE + 18, 0x000C); // IEN3[3|2] = CRA0|CRB0 を同時有効化
```
