# P/ECE カーネルソース参照ガイド

**ソース位置**: `sdk/sysdev/pcekn/` (読み取り専用)  
**バージョン**: PIECE KERNEL Ver 1.19 (Aquaplus / OeRSTED)  
**注意**: `sdk/` はほかのプロジェクトと共用のため、絶対に編集しないこと。

---

## ファイル一覧と役割

| ファイル | 役割 | エミュレータへの関連度 |
|---------|------|----------------------|
| `pcekn.c` | カーネルエントリ・サービスAPI | ★★★ ブートシーケンス |
| `hard.c` | ハードウェア初期化 (InitHard, GetSysClock) | ★★★ レジスタ設定値 |
| `timer.c` | タイマ割り込み・ClockTicks | ★★★ T16/WDT設定 |
| `pad.c` | パッド入力 (_pcePadGetDirect) | ★★ ボタンビット配置 |
| `lcd.c` | LCD ドライバ (S6B0741 via SIF3/HSDMA) | ★★ SIF3/HSDMA要件 |
| `snd.c` | サウンド (T16 Ch.1 PWM + HSDMA) | ★ Audio要件 |
| `rtc.c` | RTC (計時タイマ) | ★ RTC要件 |
| `powerman.c` | 電源管理 | ★ |
| `vector.h` | トラップベクタ定数 | ★★ |
| `pcekn.h` | カーネル内部マクロ (iop_c/iop_s/iop_w) | ★★★ |
| `work.h/c` | グローバル変数宣言 | ★★ |

---

## I/Oアクセスマクロ (pcekn.h)

```c
// iop_c/iop_s/iop_w は単純に I/O ベースポインタ
volatile unsigned char  *bp = iop_c;  // byte  access: bp[offset]
volatile unsigned short *sp = iop_s;  // short access: sp[offset/2]  ← sp[0x8180/2]
volatile unsigned long  *wp = iop_w;  // word  access: wp[offset/4]  ← wp[0x8134/4]
```

`iop_c` の実体は `(volatile uint8_t *)0x040000` (I/Oベース)。  
オフセットはそのまま I/O アドレスの下位24ビット。例: `bp[0x2c1]` = アドレス `0x0402C1`。

---

## ブートシーケンス (pcekn.c: BootEntry)

```
BootEntry00()              ← リセットベクタ (FRAM にコピー後ジャンプ)
  BootEntry()
    ├── WDT を WRWD で一時解除 → bp[0x171] = 0
    ├── IRAM をゼロクリア
    ├── FRAM2 コピー
    ├── InitHard(TPVECTORTOP=0x400)   ← TTBR設定・GPIO初期化・D ボタン待ち
    ├── InitVector()
    ├── GetSysClock()                 ← T16 Ch.0 で CPU クロック実測
    ├── InitHard2()                   ← USB クロック・PORT1 割り込み設定
    ├── InitTimer()                   ← T16 Ch.0 1ms + WDT NMI 設定
    ├── InitPad()
    ├── InitPower()
    ├── InitIR()
    ├── InitFont()
    ├── InitLCD()                     ← SIF3 初期化・LCD リセット・初期コマンド
    ├── InitRTC()
    ├── InitSound()
    ├── InitFile()
    ├── InitDraw()
    ├── InitUSB()
    ├── InitUSBCOM()
    ├── InitError()
    ├── InitHeapman()
    ├── InitFlashAcc()
    └── AppStart()                    ← アプリケーション開始
```

---

## InitHard (hard.c) — 重要レジスタ設定

```c
// 全割り込み禁止
wp[0x270/4] = 0;   // IEN1–IEN4 = 0
wp[0x274/4] = 0;   // IEN5–IEN8 = 0

// INTC rRESET (byte 63 of INTC block at 0x04029F)
bp[0x29f] = 0x06;  // RSTONLY=0, IDMAONLY=1, DENONLY=1
// → ISR は write-0-to-clear (直接書き込み) モード

// TTBR (トラップテーブルベースアドレス)
wp[0x8134/4] = (unsigned long)TPVECTORTOP;  // = 0x400

// BCU エリア設定
sp[0x8126/2] = 0x6012;  // Area 10-9 (Flash)
sp[0x8128/2] = 0x0024;  // Area 8-7  (LCD 8bit, USB 4wait)
sp[0x812a/2] = 0x1112;  // Area 6-4  (SRAM 2wait)
sp[0x812e/2] = 0x0008;  // BSL 設定

// CPU クロック初期値: x1/8 (= 3 MHz または 6 MHz)
bp[0x19e] = 0x96;               // PSCDT0 (wait)
bp[0x180] = 0x27 + (3 << 6);   // PWRCTL: CLKCHG bits = 3 → /8
```

**D ボタン待ち**:
```c
while (1) {
    if (bp[0x2c1] & 0x08) break;  // K5D bit 3 = SELECT ボタン (1=離)
    // LED 点滅 (p03, p05)
}
```
→ エミュレータではK53(SELECT)を常時1(非押下)にしておくと即通過できる。

---

## GetSysClock (hard.c) — クロック測定

RTC (bp[0x153] bit3 = 1秒クロック) を使って T16 Ch.0 TC の変化量を計測。

```c
// 測定用 T16 Ch.0 設定
bp[0x147] = 8+4;           // CLKCTL_T16_0: TON=1, TS=4 → CPU/32
sp[0x8180/2] = 32768-1;    // CRA = 32767
sp[0x8182/2] = 65536-1;    // CRB = 65535
bp[0x8186] = 3;            // CTL = 3 (PRUN=1, PRESET→TC=0)
```

計測後に `clkp1` を設定:
- 48 MHz 相当: `clkp1 = 8`, CPU速度 x1/2 に設定 (実効24 MHz)
- 24 MHz 相当: `clkp1 = 4`, CPU速度 x1   に設定 (実効24 MHz)

エミュレータでの影響: `GetSysClock` が T16 TC を読む。エミュレータの T16 が正常に動いていれば clkp1 が正しく測定される。ただしRTC (bp[0x153]) が未実装だとこの関数でハングする。

---

## InitTimer (timer.c) — 1ms タイマ設定

```c
pceVectorSetTrap(7,  nmi_isr);   // NMI = ClockTicks++
pceVectorSetTrap(30, timer_isr); // T16 CRB0 = contextsw + timer_sub

// T16 Ch.0 設定
bp[0x147] = 8+2;                          // CLKCTL_T16_0: TON=1, TS=2 → CPU/8 (=6MHz at 48MHz)
sp[0x8180/2] = 100 - 1;                   // CRA = 99  (未使用; 100 ticks/精度タイマ用)
sp[0x8182/2] = (6000 * clkp1 / 4) - 1;  // CRB = 5999 at 48MHz → 1ms
bp[0x8186] = 3;                           // CTL = PRUN + PRESET

// INTC: T16 Ch.0 CRB のみ有効 (CRA は無効)
bp[0x266] = 6;   // T16 Ch.0 優先度 = 6  (offset 6 = rP16T01, bits[2:0])
bp[0x272] = 4;   // IEN3 bit 2 = E16TU0 (T16_CRB0 enable)
// ※ bp[0x266] はINTCオフセット6の下位バイト = lo byte of halfword at 0x040266

// WDT 設定 (NMI源)
bp[0x170] = 0x80;  // rWRWD: WRWD=1 (書き込み保護を有効化してrEWDに書けるようにする)
bp[0x171] = 0x02;  // rEWD:  EWD=1  (WDT 有効 → NMI を定期発生)
```

### timer_isr の ISR クリア

```c
void timer_isr(void) {
    INT_BEGIN2;
    *(unsigned char *)0x40282 &= ~4;   // ISR3 (INTC byte 34) bit 2 = T16_CRB0 クリア
    // ...
}
```

アドレス `0x40282` = INTC ベース(0x040260) + 34 = ISR byte 34。  
`&= ~4` = bit 2 を 0 に → write-0-to-clear (RSTONLY=0 モード)。

---

## pad.c — _pcePadGetDirect

```c
(((bp[0x2c1] << 3) & 0xC0) | (bp[0x2c4] & 0x3F)) ^ 0xFF
// bp[0x2c1] = K5D (0x0402C1): bit3=SELECT(K53), bit4=START(K54), active-low
// bp[0x2c4] = K6D (0x0402C4): bit0=RI, bit1=LF, bit2=DN, bit3=UP, bit4=B, bit5=A
// 結果: 0=非押下, 1=押下 (XOR で論理反転)
// bit7=SELECT, bit6=START, bit5=A, bit4=B, bit3=UP, bit2=DN, bit1=LF, bit0=RI
```

SDL フロントエンド実装時の `set_k5() / set_k6()` 引数ビット割り当て:

| K5D ビット | K53 (SELECT) | K54 (START) |
|-----------|-------------|------------|
| bit 3 | SELECT 押下=0 | — |
| bit 4 | — | START 押下=0 |

| K6D ビット | K60 (右) | K61 (左) | K62 (下) | K63 (上) | K64 (B) | K65 (A) |
|-----------|---------|---------|---------|---------|--------|--------|
| bit 0–5   | 右=0 | 左=0 | 下=0 | 上=0 | B=0 | A=0 |

---

## lcd.c — LCD 初期化の概要

SIF3 経由でS6B0741へSPIコマンドを送信。GPIO の役割:

| ポート | 信号 | 制御内容 |
|--------|------|---------|
| P20 (bp[0x2d9] bit0) | LCD CSB (chip select, active-low) | 0=選択 |
| P21 (bp[0x2d9] bit1) | LCD RS | 0=データ, 1=コマンド |
| P33 (bp[0x2dd] bit3) | LCD RESET | 0=リセット中 |
| bp[0x1f5] | SIF3 送信データ | S6B0741コマンド/データ |
| bp[0x1f8] | SIF3 制御 | bit7=TEN |

HSDMA Ch.0 で連続データ転送 (DMA 使用):
```c
wp[0x8220/4] = (len-1) | 0x80000000;  // HSDMA Ch0 転送カウント
wp[0x8224/4] = ((int)p+1) | 0x30000000; // 転送元アドレス
wp[0x8228/4] = 0x401f5;               // 転送先 = SIF3 送信バッファ
sp[0x822e/2] = 1;   // HSDMA Ch0 トリガ
sp[0x822c/2] = 1;   // HSDMA Ch0 イネーブル
```

---

## vector.h — トラップ番号定数

```c
#define TPVECTORTOP 0x400   // トラップテーブルベース (IRAM)
#define KSVECTORTOP 0x20    // カーネルサービステーブルベース
#define TPMAX       72      // トラップ最大数
```

---

## エミュレータ実装への影響まとめ

### GetSysClock がハングしうる条件

`while (bp[0x153] & 0x08)` で RTC の 1 秒クロック(bit 3)の立ち上がりエッジを待つ。  
RTC (0x040151–0x04015B) が未実装で `bp[0x153]` が常に 0 だと永遠に待ち続ける。

**対策**: RTC スタブで bp[0x153] bit 3 を定期的にトグルするか、ブートトレース時に --max-cycles で打ち切って確認する。

### InitHard の D ボタン待ち

`bp[0x2c1] & 0x08` = K5D bit 3 (SELECT = K53)。  
エミュレータ起動時は K5D = 0xFF (全ビット 1 = 全ボタン非押下) に初期化しておくと即通過する。

→ `peripheral_portctrl.cpp` で `k5d_` のデフォルトを `0xFF` にすること（現状要確認）。

### WDT NMI の周期

カーネルは WDT NMI で ClockTicks を 1 ずつ加算し、精度タイマの上位 16 bit に使用。  
エミュレータでは `peripheral_wdt.cpp` が cpu_clock/1000 周期 (≈1 ms) で NMI(trap7) を発生させる。  
実機の WDT クロックは cpu_clock/2^14 程度だが、精度タイマの動作に支障はない。

### INTC バイト順序の罠

`bus.write16(INTC_BASE + off, val)` のとき:
- `val` の **lo byte** → `regs_[off]`   (アドレスの偶数バイト)
- `val` の **hi byte** → `regs_[off+1]` (アドレスの奇数バイト)

例: T16 Ch.0 優先度 (rP16T01, INTC オフセット 6 の lo byte) を 6 に設定するには:
```cpp
bus.write16(INTC_BASE + 6, 0x0006);  // lo byte = 6 → offset 6
```
ユニットテストの `write_byte()` ヘルパー関数が read-modify-write を提供している。

### T16 CTL = 3 の挙動

```
CTL = 3 = 0b00000011
  bit 0 = PRUN  = 1 (タイマ動作)
  bit 1 = PRESET = 1 (TC をゼロにリセット → 自己クリア)
```

`bus.write16(base+6, 0x0003)` を 1 回書くと PRUN=1 かつ TC=0 でタイマ開始。  
PRESET は `peripheral_t16.cpp` の write ハンドラで自己クリア済み。

---

## 今後の実装で読むべきソース

| 必要な機能 | 読むファイル | 重要な関数/変数 |
|-----------|------------|--------------|
| RTC スタブ実装 | `rtc.c` | `InitRTC()`, `bp[0x151..15B]` |
| SIF3 スタブ実装 | `lcd.c` | `TxLCDAsync()`, `bp[0x1f5..1f8]` |
| HSDMA スタブ | `lcd.c`, `snd.c` | `wp[0x8220..822F]` |
| LCD フロントエンド | `lcd.c` | `S6B0741` コマンド列, `lcdc.c` (piemu) |
| サウンド | `snd.c` | `InitSound()`, HSDMA Ch.1, T16 Ch.1 |
| USB | `d12ci.c`, `usbcom.c` | PDIUSBD12 コントローラ |
| ファイルシステム | `file.c`, `fmacc*.c` | Flash アクセス |
