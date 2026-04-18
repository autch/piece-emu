# P/ECE エミュレータ設計仕様書

## 0. 本文書の目的

本文書は、アクアプラス P/ECE（PIECE）ゲーム機のエミュレータを新規開発するにあたり、設計議論で確定した要件・方針・技術仕様をまとめたものである。

### 動機と目的

本エミュレータの主目的は、P/ECE 向け LLVM/Clang バックエンドの開発を支援するデバッグ環境の提供である。実機は量産機でありデバッグ機能を持たず、コンパイラのコード生成バグの調査にはエミュレータ上での逆アセンブル・ステップ実行・ブレークポイントが不可欠である。

最終的には P/EMU のようにゲームを遊べるエミュレータに発展させたいが、当面はデバッグ支援に必要な機能から実装していく。

### 設計原則

**「正しい」動作に倒す。** 設計や実装で迷った場合は、実機と同じ挙動よりもCPUマニュアルの仕様に忠実な動作を選択する。エミュレータ自体にバグがあると、コンパイラとエミュレータのどちらがバグっているのか切り分けられなくなる。これが最も避けるべき事態である。

**実機より厳しく（diagnostic emulator）。** 実機では黙って無視される不正な操作に対しても、エミュレータは積極的に警告・エラーを報告する。これはコンパイラやアセンブラのバグを早期に検出するためであり、このエミュレータの最大の付加価値である。実機で動かすのと変わらない動作しかしないのであれば、エミュレータでテストする意味が薄れる。

具体例：
- 未定義オペコード → エラー停止（実機では動作未定義）
- ext + シフト命令（ext非対応）→ 警告（実機ではextが黙って無視される。コンパイラ/アセンブラのバグに起因）
- ディレイスロットに許可リスト外の命令 → 警告（実機では動く場合がある）
- ハーフワード接続デバイスへのバイトI/O → 警告（実機ではバス上で無視される）
- 非アラインドアドレスへのワード/ハーフワードアクセス → エラー（実機ではアドレス不整例外）
- `jp.d %rb` の使用 → エラー（実機のハードウェアバグにより誤動作する）

### 実装環境

| 項目 | 選択 |
|------|------|
| 言語 | C++ |
| メタビルドツール | CMake + Ninja |
| 当初の対象プラットフォーム | Linux |

### 動作モード

QEMUのユーザモード/システムモードに類似した2つの動作モードを持つ。CPUコア・BCU・メモリサブシステム・逆アセンブラ・GDB RSP・セミホスティングは共通基盤として両モードで共有する。

#### ベアメタルモード（当面の主力）

コンパイラのテスト・デバッグ用。カーネル不要。

- **コマンドラインアプリケーション** — GUIなし
- ELFを直接ロードしてエントリポイントから実行
- カーネルAPIテーブル（0x0〜）は空。カーネルAPI呼び出しは未定義動作
- セミホスティングポート（§4）で結果出力・テスト判定
- GDB RSP でデバッガ接続可能
- CI統合: 終了コード 0=PASS, 非0=FAIL

```
# ユニットテスト実行
$ piece-emu test_add.elf
PASS

# 実行トレース
$ piece-emu --trace test_add.elf

# GDB RSPデバッガ接続
$ piece-emu --gdb 1234 my_app.elf &
$ lldb
(lldb) gdb-remote 1234
(lldb) b main
(lldb) c
```

#### システムモード（P2-2 実装済み）

ゲーム実行用。カーネル込みのフルシステムエミュレーション。

- **P2-1 実装済み**: PFI イメージからフラッシュ全体をロードし、リセットベクタからブート。カーネルが AppStart まで到達することを確認済み。
- **P2-2 実装済み**: SDL3 フルシステムフロントエンド（`piece-emu-system`）。LCD 表示（S6B0741）、ボタン入力（K5/K6）、非同期 GDB RSP 対応。
- **P2-3 以降未実装**: サウンド出力（PWM + SDL3 Audio）、Flash 書き込み

```
# SDL3 フルシステムフロントエンド
$ piece-emu-system --pfi images/old/piece.pfi

# 非同期 GDB RSP 付き（LLDB MCP 対応）
$ piece-emu-system --pfi images/old/piece.pfi --gdb-port 1234
```

#### 共通基盤の構造

共通基盤をライブラリとして構築し、2つの実行バイナリがこれをリンクする。同一バイナリである必要はなく、依存関係を最小化するために分離する。

```
libpiece_core.a  (src/core/)
├── CPUコア / BCU / メモリ
└── 逆アセンブラ

libpiece_soc.a   (src/soc/) → piece_core
├── INTC / ClkCtl
├── T8×4 / T16×6
├── PortCtrl / BcuArea / WDT / RTC
└── SIF3 / HSDMA

libpiece_board.a (src/board/) → piece_soc
├── S6B0741 LCD コントローラ
└── (将来: NAND Flash, PDIUSBD12 USB)

libpiece_host.a  (src/host/) → piece_board, SDL3
└── LcdRenderer — S6B0741 VRAM → SDL3 テクスチャ

libpiece_debug.a (src/debug/) → piece_core
├── ELFローダ
├── PFIローダ
├── セミホスティング
└── GDB RSP（sync / async 両モード）

piece-emu          (ベアメタルモード実行バイナリ)
├── piece_debug + piece_board + piece_soc + piece_core
├── CLI フロントエンド（--wp-write/--wp-read/--wp-rw/--break-at）
└── 外部依存: なし（POSIXのみ）

piece-emu-system   (システムモード実行バイナリ)
├── piece_host + piece_debug + piece_board + piece_soc + piece_core
├── SDL3 GUI フロントエンド（LCD 表示・ボタン入力・60fps）
├── 非同期 GDB RSP（--gdb-port）— SDL main loop と2スレッド協調
└── 外部依存: SDL3
```

ベアメタルモードの `piece-emu` はPOSIX環境さえあれば動作し、CI環境やヘッドレスサーバにSDL等をインストールする必要がない。

**ELFロードの動作:**
- ELFのプログラムヘッダに従い、セグメントをメモリにロード（通常 0x100000〜）
- エントリポイントにPCを設定して実行開始
- カーネルAPIテーブル（0x0〜）は空なので、カーネルAPI呼び出しは未定義動作になる
- カーネルAPIを使わないベアメタルテストプログラム専用
- セミホスティングポートで結果を出力・判定

**PFIロードの動作:**
- PFIファイルからフラッシュイメージを 0xC00000 にロード
- 0xC00000 のリセットベクタからブート（カーネル起動→アプリ展開→実行）
- 全周辺デバイスのエミュレーションが必要

### エージェント統合

最終的な理想像は、LLVM側の実装を担当するAIエージェント（Copilot/Codex/Claude Code等）が、このエミュレータを自力で使って実装からデバッグまで完結できることである。これは以下の要件を課す：

**人間の介在を一切必要としないインタフェース:**
- 全操作がコマンドラインで完結すること（GUIやLCD目視確認に依存しない）
- 出力は機械可読（JSON等）であること
- 終了コードでPASS/FAILが判定できること

**エージェントの典型的なワークフロー:**
1. LLVM バックエンドのコードを修正
2. テストプログラムをコンパイル: `clang --target=s1c33-none-elf test.c -o test.elf`
3. エミュレータで実行: `piece-emu --elf test.elf --headless --timeout 5s`
4. PASS → 次のタスクへ。FAIL → 原因調査
5. トレース取得: `piece-emu --elf test.elf --headless --trace trace.log`
6. ブレークポイント付き実行: `piece-emu --elf test.elf --headless --break 0x100040 --dump-on-stop json`
7. JSON出力からレジスタ・メモリ状態を読み取り、コンパイラのバグ箇所を特定
8. 修正して 2 に戻る

**`--headless` モードの出力仕様（案）:**
```json
{
  "status": "FAIL",
  "exit_code": 3,
  "cycles": 48210,
  "pc": "0x001000a4",
  "stop_reason": "test_result_port",
  "registers": {
    "r0": "0x00000000", "r1": "0x00001234", ...
    "sp": "0x00101ff0", "psr": "0x00000002"
  }
}
```

**GDB RSP との使い分け:**
- 単純なPASS/FAIL判定 → `--headless` が効率的
- 対話的な調査が必要な場合 → GDB RSP（人間のデバッグ、またはエージェントがGDBプロトコルを使う場合）

### エミュレータ自身の正しさの担保

エミュレータにバグがあると、コンパイラのデバッグツールとして機能しない。エミュレータの正しさはシステマティックな自動テストで担保する。

**テストの構造:**

```
[CPUマニュアル命令仕様] ──→ [命令単位テストケース（手書き）]
                                    ↓
[LLVM TableGen定義] ──→ [テストベクタ自動生成] ──→ [エミュレータ実行]
                                    ↓                       ↓
                          [llvm-objdump期待出力]    [実行結果+逆アセンブル出力]
                                    ↓                       ↓
                                  [自動比較 → PASS/FAIL]
```

- **命令テストスイート**: 全命令×ext 0/1/2個×フラグ状態の組み合わせを網羅
- **符号境界テスト**: §2.3 の即値符号規則に基づく重点テスト
- **逆アセンブラテスト**: LLVM llvm-objdump、MAME c33dasm との3者突合
- **リグレッションテスト**: エミュレータの全ビルドで自動実行
- **実機突合**: 可能なケースでは実機の実行結果と比較（LCD表示やFlash書き出しで結果を取得）

テストスイート自体がエミュレータの「仕様書」として機能し、将来の変更で挙動が壊れた場合に即座に検出される。

### 先行実装

以下の既存実装を参照し、流用可能な部分を特定すること。

| 実装 | リポジトリ | 概要 | ライセンス |
|------|-----------|------|-----------|
| P/EMU/SDL | `https://github.com/autch/piemu` (ブランチ: `revert-to-single-thread` を優先、`modernize` も参照) | Naoyuki Sawa 氏の P/EMU を Autch 氏が SDL 移植。C言語。CPUコア＋最低限の周辺デバイス | 未明記（要確認） |
| MAME C33 | `https://github.com/mamedev/mame` の `src/devices/cpu/c33/` | Vas Crabb 氏によるスケルトン。逆アセンブラ `c33dasm.cpp/h` は完成、CPUコア実行部は未実装 | GPL-2.0（コード流用不可、仕様参照のみ） |

### 一次資料

プロジェクトナレッジに以下の資料が格納されている：

| 資料 | 内容 |
|------|------|
| `S1C33000_コアCPUマニュアル_2001-03.pdf` | CPUアーキテクチャ、命令セット、エンコーディング、割り込み |
| `S1C33209_201_222テクニカルマニュアル_PRODUCT_FUNCTION.pdf` | メモリマップ、BCU、周辺回路（タイマ、DMA、シリアル等） |
| `hardspec1.html` | P/ECE ハードウェア概要（CPU、LCD、サウンド、メモリ、I/O） |
| `PIECE_ハードウエア割り込み.html` | P/ECE 割り込み体系 |
| `PIECE_ポート解説.html` | P/ECE I/Oポート割り当て |
| `s6b0741.pdf` | Samsung S6B0741 LCDコントローラ仕様 |
| `PDIUSBD12.pdf` | Philips PDIUSBD12 USBコントローラ仕様 |
| `SST39VF400A.pdf` | SST39VF400A Flashメモリ仕様 |
| `errata.md` | CPUハードウェアバグ、コンパイラバグ、ライブラリバグのエラッタ集 |
| `DESIGN_SPEC.md`（アップロード済み） | LLVM バックエンド設計仕様（ABI、即値符号規則、メモリマップ等） |
| `piece-lab-*.txt` | piece-lab 記事群（2001〜2021年、開発者コミュニティの技術情報） |

---

## 1. ターゲットハードウェア仕様

### 1.1 全体構成

| コンポーネント | 型番/仕様 | 備考 |
|---------------|----------|------|
| CPU | EPSON S1C33209 (S1C33000 STD Core) | 32bit RISC, 24MHz, 3段パイプライン |
| メインRAM | SRAM 256KB | 外付け、0x100000〜0x13FFFF |
| 内蔵高速RAM | 8KB | 0x000000〜0x001FFF、1サイクルアクセス |
| Flashメモリ | SST39VF400A 512KB (または 2MB版) | 外付け、0xC00000〜 |
| LCD | 128×88ドット、白黒4階調 FSTN | Samsung S6B0741 コントローラ、SPI接続 |
| サウンド | CPU内蔵PWM | ソフトウェア音源 |
| 入力 | 4方向パッド + START/SELECT/A/B | 6ボタン |
| USB | Philips PDIUSBD12 | Full-Speed 12Mbps |
| 赤外線 | IrDA | P/ECE間通信用 |
| 電源 | 単三電池1本 or USB給電 | |

### 1.2 CPU: S1C33209 (S1C33000 STD Core)

- **命令長**: 16ビット固定
- **アドレス空間**: 28ビット（256MB）、上位4ビットは無視
- **エンディアン**: リトルエンディアン
- **パイプライン**: 3段（Fetch → Decode → Execute）
- **ディレイドブランチ**: あり

**レジスタセット:**

| レジスタ | 用途 |
|---------|------|
| R0〜R15 | 汎用32ビットレジスタ |
| PC | プログラムカウンタ（28ビット有効） |
| SP | スタックポインタ |
| PSR | プロセッサステータスレジスタ（N, Z, V, C, IE, DS, MO, IL[3:0]） |
| ALR | 算術演算ローレジスタ（乗除算結果の下位32ビット） |
| AHR | 算術演算ハイレジスタ（乗除算結果の上位32ビット / 余り） |

**PSRビットフィールド（S1C33209で使用するもの）:**

| ビット | 名前 | 機能 |
|--------|------|------|
| 0 | N | 負フラグ |
| 1 | Z | ゼロフラグ |
| 2 | V | オーバーフローフラグ |
| 3 | C | キャリーフラグ |
| 4 | IE | 割り込み許可 |
| 6 | DS | ディレイスロット中 |
| 7 | MO | MAC演算オーバーフロー |
| 8〜11 | IL[3:0] | 割り込みレベル |

**フラグ更新の注意**: ほぼ全てのALU命令（add, sub, cmp, and, or, xor, not, srl, sll, sra, rr, rl、ld即値含む）がN,Z,V,Cを暗黙に更新する。これはgcc33のバグ（0比較cmp欠落）の根本原因でもあった。テストスイートでフラグ更新を網羅的に検証すべき。

### 1.3 メモリマップ

```
0x000000〜0x001FFF  内蔵RAM 8KB（1サイクルアクセス）
  0x000000〜0x000FFF  割込みベクタ、カーネルAPIテーブル（システム使用）
  0x001000〜0x001FFF  ユーザ使用可能領域
  （0x002000 = スタック底）

0x040000〜0x04FFFF  I/Oレジスタ領域（周辺デバイス制御レジスタ）

0x100000〜0x13FFFF  外付けSRAM 256KB
  0x100000           アプリケーションヘッダ (pceAPPHEAD)
  0x100020           アプリコード開始
  0x13D000〜         システムワーク領域

0xC00000〜          外付けFlash（512KB〜2MB）
  0xC00000           リセットベクタ（緊急カーネルエントリ）
  0xC02000           通常カーネル開始
  0xC0C000           フォントデータ
  0xC28000〜         ファイルシステム (PFFS)
```

### 1.4 BCU（バスコントロールユニット）

BCUはCPUと外部デバイスのインタフェースを提供し、メモリマップのエリア別にデバイスタイプ・デバイスサイズ・ウェイトサイクル・出力ディセーブル遅延時間を設定する。エミュレータではBCUがアドレスデコードの中心となり、CPUからのメモリアクセスを適切なデバイスハンドラに振り分け、ウェイトサイクルに基づいて消費サイクル数を計算する。

#### エリア構成とP/ECEでの使用

| エリア | アドレス範囲 | P/ECEでの接続先 | #CE信号 | デバイスタイプ |
|--------|-------------|----------------|---------|-------------|
| 内蔵RAM | 0x000000〜0x001FFF | 8KB高速RAM | — | 内蔵（1サイクル） |
| 内蔵I/O | 0x040000〜0x05FFFF | 周辺デバイスレジスタ | — | 内蔵 |
| エリア4 | 0x100000〜0x1FFFFF | SRAM 256KB | #CE4 | SRAMタイプ, 16bit |
| エリア5 | 0x200000〜0x2FFFFF | （未使用） | #CE5 | — |
| エリア6 | 0x300000〜0x3FFFFF | I/Oデバイス（USB等） | #CE6 | SRAMタイプ, 8/16bit |
| エリア10 | 0xC00000〜0xFFFFFFF | Flash 512KB〜2MB | #CE10 | SRAMタイプ or バーストROM, 16bit |

P/ECEではエリア4にSRAM、エリア10にFlashが接続されている。エリア6にUSBコントローラ(PDIUSBD12)が接続されている。他のエリア(5,7,8,9)は未接続。

#### BCU制御レジスタ（エミュレータで実装すべきもの）

| アドレス | 名称 | 主なフィールド |
|---------|------|-------------|
| 0x048120 | エリア18〜15設定 | A18WT[2:0], A18DF[1:0], A18SZ, A16WT[2:0], A16DF[1:0], A16SZ |
| 0x048122 | エリア14〜13設定 | A14WT[2:0], A14DF[1:0], A14SZ, A14DRA, A13DRA |
| 0x048124 | エリア12〜11設定 | A12WT[2:0], A12DF[1:0], A12SZ |
| 0x048126 | エリア10〜9設定 | A10WT[2:0], A10DF[1:0], A10SZ, A10BW[1:0], A10IR[2:0] |
| 0x048128 | エリア8〜7設定 | A8WT[2:0], A8DF[1:0], A8SZ, A8DRA, A7DRA |
| 0x04812A | エリア6〜4設定 | A6WT[2:0], A6DF[1:0], A5WT[2:0], A5DF[1:0] |
| 0x04812E | バスコントロール | SBUSST（バスストローブ方式）, SWAIT（外部ウェイト許可） |
| 0x048130 | DRAMタイミング設定 | CEFUNC[1:0]（#CE信号割り当て切替） |

#### ウェイトサイクルの仕様

各エリアのウェイト制御ビット AxWT[2:0] で 0〜7 サイクルのウェイトを設定できる。

- **リードサイクル**: 基本1サイクル + ウェイト数 = 合計 [ウェイト数+1] サイクル
- **ライトサイクル**: 基本2サイクル（最小）。ウェイト数が2以上のとき [ウェイト数+1] サイクル
- **コールドスタート時のデフォルト**: ウェイト7サイクル、出力ディセーブル遅延3.5サイクル（最も安全な設定）
- カーネルのブートコードが各エリアのウェイトを適切な値に再設定する

#### 出力ディセーブル遅延

異なるエリア間のリード→次のアクセスの間に挿入される遅延サイクル（0.5〜3.5サイクル）。同一エリア内の連続リードでは挿入されない。

#### エミュレータ実装方針

- CPUからのメモリアクセスごとに、アドレスからエリアを判定し、BCUレジスタの設定に基づいてウェイトサイクル分の消費サイクルを加算する
- 内蔵RAM (0x000000〜0x001FFF) と内蔵I/O (0x040000〜) はBCUを経由せず1サイクルアクセス
- バーストROMモード（エリア10のFlash用）: 連続フェッチ時にバーストリードサイクルでウェイトを削減。命令フェッチの高速化に寄与
- DRAMインタフェースはP/ECEでは未使用のため、初期実装では省略可
- デバッグポート（§4、0x060000）はBCUのアドレスデコードより優先してマッチさせる

#### 内蔵ROMサイズ設定 (A10IR[2:0])

エリア10設定レジスタの A10IR[2:0] でブート用ROM/エミュレーションメモリのサイズを指定する（16KB〜2MB）。P/ECEではOTP/内蔵ROMエミュレーションモード（EA10MD="01"）を使用し、IDMAチャネル0でFlashからSRAMにブートコードを転送する。

### 1.5 P/ECE 実行モデル

- CPU起動時: 0xC00000 のリセットベクタ（ワード値）を読み、そのアドレスにジャンプ
- カーネルが起動後、ファイルシステムからアプリ (.pex) を SRAM 0x100000 に展開
- 0x100000 の pceAPPHEAD（シグネチャ 'pCeA' = 0x41654370 LE）を読み、各コールバックを呼び出す
- R8 = 0x0 をセットしてアプリを起動（0x0 からのワード配列がカーネルジャンプテーブル）

### 1.6 クロック体系とタイミングモデル

#### 実機のクロック構成

P/ECEは48MHz水晶発振子（OSC3）を搭載し、CPU内蔵プリスケーラでクロックを分周する。

| モード | CPUクロック | 設定方法 | 備考 |
|--------|-----------|---------|------|
| 通常モード | 24MHz | P07=1（デフォルト） | 標準動作。全周辺回路の動作保証あり |
| 高速モード | 48MHz | P07=0 | 高速動作を要求するゲームが存在。要実装 |
| 1/2モード | 12MHz | システムメニューから設定 | 周辺回路の動作保証なし |
| 1/4モード | 6MHz | システムメニューから設定 | 周辺回路の動作保証なし |

P07（ポート0ビット7）はOSC3クロック制御ポートで、0で48MHz（×4）、1で24MHz（×2）を選択する。`#X2SPD` 端子はCPUクロックとバスクロックの比を設定する（1:CPUクロック=バスクロック、0:CPUクロック=バスクロック×2）。

#### エミュレータのタイミングモデル

**2層構造**で実装する：

**CPUコア層（サイクル精度）:**
- 各命令のサイクル数を正確にカウント（命令ごとに1〜10+サイクル）
- BCUのウェイトサイクルを加算（メモリアクセス命令時）
- 累計サイクルカウンタを保持
- セミホスティングのサイクルカウンタ（§4）はこの値を返す

**周辺デバイス層（イベント駆動）:**
- タイマ: プリスケーラ設定とCPUクロック分周比から次の発火サイクルを計算し、CPUの累計サイクルが到達したら割り込み発生
- DMA: 転送開始時に完了サイクルを予約。完了までのサイクル数はブロック単位で計算
- LCD: DMA転送完了イベントで画面更新
- サウンド: タイマ割り込み駆動でPWMバッファを生成

バスサイクル単位のデバイス間競合（CPUとDMAのバス調停など）は厳密には再現しない。これを完全に再現するには全デバイスをバスクロック単位で同期させる必要があり、実装コストに見合わない。

**クロック切替の検出:**
- P07への書き込みをI/Oハンドラで検出
- エミュレータ内部のクロック分周比を即座に切り替え
- 1サイクルあたりのホスト時間が変わるため、フレームレート制御とタイマの発火間隔に影響
- 命令のサイクル数自体は変わらない（CPUパイプラインは同一）

**フレームレート制御:**
- P/ECEのカーネルは1ms割り込み（16bitタイマ駆動）でフレーム管理
- pceAppProc() は通常この1ms割り込みから呼ばれる（cnt引数で経過フレーム数を通知）
- エミュレータはCPUサイクル消費量とクロック周波数からホスト時間を計算し、実時間と同期

---

## 2. 設計方針

### 2.1 スレッドモデル: CPU/SDL 2スレッド分離

**決定**: `piece-emu-system` は CPU スレッドと SDL スレッドを分離する。

**スレッド構成**:
- **`piece-cpu`** (`std::thread`): `CpuRunner::run()` — CPU ステップループ・周辺デバイス・タイマ
- **`piece-sdl`** (main thread): SDL3 イベントポーリング + `LcdRenderer::render()` — SDL 描画はメインスレッド必須
- **`piece-gdb`** (optional): GDB RSP 非同期サーバ (`--gdb-port` 指定時のみ)

**スレッド間共有状態**:
- `LcdFrameBuf` — mutex 保護のピクセルスナップショット。CPU スレッドが HSDMA Ch0 完了時に `push()`、メインスレッドが ~60 Hz で `take()`。複数フレームが到着した場合は最新を優先（フレームドロップ）。
- `std::atomic<bool> quit_flag` — 停止シグナル
- `std::atomic<uint16_t> shared_buttons` — ボタン状態

**根拠**:
- SDL3 の描画 API（`SDL_RenderPresent`）はメインスレッドからのみ呼び出し可能
- CPU スレッドを分離することでホスト CPU のコアを有効活用し、ゲスト CPU 処理と SDL イベント処理が互いをブロックしない
- 共有状態を最小化（atomic + 1 mutex）してレースコンディションを回避

**HLT/SLEEP からの起床（タイムジャンプ方式）**:

CPU が `halt` / `slp` 命令を実行して `in_halt = true` になると、CPU スレッドは以下を行う：

1. `periph.next_wake_cycle()`（HLT 中）または `periph.sleep_wake_cycle()`（SLP 中）から次の周辺イベント予定サイクルを取得
2. `total_cycles` を `min(wake, next_render, next_event_poll)` まで前進（シミュ時刻のジャンプ）
3. `do_tick()` で全周辺を新時刻まで進める（IRQ 発火 → `assert_trap()` が `in_halt` を解除）
4. `total_cycles >= next_event_poll` なら `poll_buttons()` を呼ぶ

実時間との同期は 60 Hz の `sync_realtime()` 境界で `SDL_DelayNS`（スレッド安全な `nanosleep` ラッパで SDL イベントキューには触れない）により行う。`SDL_WaitEventTimeout` / `std::condition_variable::wait_until` は**使わない** — 周辺割り込みによる wake 契機（タイマ・RTC 等、SDL とは無関係）とボタン入力とで二重の signal 機構が不要になり、シミュ時刻ベースで一元化できるため。

**不変条件**: **CPU スレッドは `in_halt` 中であっても `shared_buttons` を最長 `EVENT_INTERVAL`（10,000 サイクル ≒ 0.4ms @ 24MHz）以内にポーリングする。** これは HLT ジャンプ target を `next_event_poll` でクランプし、ジャンプ後にも `poll_buttons()` を走らせることで保証する。この不変条件が破れると、実機では「ボタン長押しで SLEEP 復帰」の契機が最長 1 秒（RTC 周期）遅延する。

**HaltMode 区別（`CpuState::halt_mode`）**:

- `HaltMode::Hlt` (`halt` 命令) — 全クロック稼働継続。任意の周辺割り込み（T8/T16/WDT/RTC/KEY）で起床。`next_wake_cycle()` を使用。
- `HaltMode::Slp` (`slp` 命令) — 実機では OSC3 停止。wake 源は OSC1 駆動の RTC（1Hz）とボタン入力（KEY0 via `portctrl.check_key_irq()`）に限定される。`sleep_wake_cycle()` が RTC のみ返す。OSC3 駆動のタイマ（T8/T16/PWM/HSDMA）は wake 源から除外する。
- `assert_trap()` および GDB RSP 中断パスで `halt_mode = HaltMode::None` にリセット。

**未実装の側面（2026-04 時点）**: SLP 中に OSC3 駆動タイマを「実際に止める」仕様はまだ実装していない。SLP 中もタイマは進み続けるが、wake 源から除外されているため動作上は問題にならない。ただし SLP 中のタイマカウンタ値は実機と乖離するため、SLP 後にタイマ値を観察するプログラムの挙動には差異が出る可能性がある。

### 2.2 逆アセンブラ: 蓄積→合成方式 + 2段表示

**ext命令の処理**:
- ext を独立した命令としてデコードし、状態変数に蓄積
- 後続命令のデコード時に蓄積した ext 値と合成
- LLVM側の `applyPendingExt` と同一方式（方式2）

**表示形式**:
```
0xC02004:  ext  0x0048        ; ← 生の16bit命令
0xC02006:  ext  0x0000        ;
0xC02008:  ld.w %r0, 0x05     ; → ld.w %r0, 0x00120005 (合成値)
```

左側に生命令、右側にコメントとして合成後の実効オペランドを表示する。LLVM の llvm-objdump 出力と文字列レベルで比較検証可能な形式とする。

### 2.3 即値の符号規則（確定版）

**extは拡張対象命令の即値のsignednessを変更しない。** この表はCPUコアの `resolve_immediate()` の実装仕様である。

**signed（符号拡張）:**

| 命令 | 即値フィールド | ext×1 合成 | ext×2 合成 |
|------|--------------|-----------|-----------|
| `ld.w %rd, sign6` | sign6 (6bit) | sign_extend_19 | sign_extend_32 |
| `and/or/xor/not %rd, sign6` | sign6 (6bit) | sign_extend_19 | sign_extend_32 |
| `cmp %rd, sign6` | sign6 (6bit) | sign_extend_19 | sign_extend_32 |
| `jr**/call/jp sign8` (全分岐) | sign8 (8bit) | sign_extend_21 | sign_extend_34 |

**unsigned（ゼロ拡張）:**

| 命令 | 即値フィールド | ext×1 合成 | ext×2 合成 |
|------|--------------|-----------|-----------|
| `add/sub %rd, imm6` | imm6 (6bit) | zero_extend_19 | zero_extend_32 (=そのまま) |
| `ld.* [%sp+imm6]` (SP相対メモリ) | imm6 (6bit) | zero_extend_19 | zero_extend_32 |
| `add/sub %sp, imm10` | imm10 (10bit) | — (ext不可) | — |
| `ext + ld.* [%rb]` (ディスプレースメント) | — | zero_extend_13 | zero_extend_26 |
| `ext + op %rd, %rs` (3オペランド化) | — | zero_extend_13 | zero_extend_26 |

**合成式（統一関数）:**

```
uint32_t resolve_immediate(
    int ext_count,       // 0, 1, 2
    uint32_t ext0,       // 1個目のext値 (imm13)
    uint32_t ext1,       // 2個目のext値 (imm13)
    uint32_t raw_field,  // ターゲット命令の即値フィールド
    int width,           // 即値フィールド幅 (6, 8, 10)
    bool is_signed       // 命令ごとに決定
);
```

**重要な注意:**
- 負のオフセットは不可能。`ext 8190` は +8190 であり -2 ではない
- `add %rd, imm` に大きな ext 値を付けた場合、符号付きとして解釈すると負値になり誤動作する
- LLVM側の逆アセンブラ `applyPendingExt` にはこの符号区別のバグがある（表示専用なので実害は少ない）

**ext非対応命令:**
- シフト・ローテート命令（srl, sll, sra, sla, rr, rl）は ext による即値拡張ができない
- 即値フィールドは imm4、マッピングが特殊（0000=0, ..., 0111=7, 1xxx=8）
- ext蓄積中にシフト命令が来たら、ext を無視して実行（警告を出す）

### 2.4 ディレイドブランチ

- 分岐命令（jp, jr, call, ret, reti, retd）の `.d` 付きバリアントで、直後1命令がディレイスロット
- ディレイスロットに置ける命令の制約: 1サイクル命令、メモリアクセス不可、ext拡張不可、分岐不可

**ハードウェアバグ `jp.d %rb`**: DMA転送と重なるとディレイスロットが実行されない（errata #1）。プログラムで回避不可能。`call.d %rb` と `ret.d` には同問題なし。

### 2.5 デバッガ: GDB リモートシリアルプロトコル (RSP)

**決定**: GDB RSP を実装し、LLVM の lldb または GDB から接続可能にする。

**対応するRSP機能:**
- ブレークポイント（ソフトウェア）
- ステップ実行（命令単位）
- レジスタ読み書き（R0〜R15, PC, SP, PSR, ALR, AHR）
- メモリ読み書き
- 実行継続・停止

**フレームポインタなしのスタックアンワインド**: P/ECE ABI はフレームポインタを使用しない（SPベースのみ）。スタックトレースには DWARF CFI 情報（LLVM側で生成済み）の解釈、またはプロローグの pushn パターン解析が必要。

### 2.6 診断イベントとデバッガ連携

エミュレータ独自の診断機構（`DiagSink` インターフェース）を設ける。実機では黙って見過ごされるミスを積極的に検出し、コンパイラ・アセンブラのバグを早期発見する。

振る舞いはデバッガ接続の有無で変える。GDB RSP クライアント接続中は **strict モード**が有効になり、Warning レベルのイベントも Fault に格上げされる。

#### Fault（常にエラー停止）

CPU を停止し（`fault=true, in_halt=true`）、レジスタダンプを stderr に出力する。デバッガ接続中は GDB RSP の停止応答（T04）を返すので、レジスタ・メモリを調査してから中断できる。

| category | 診断イベント | 実機での挙動 |
|---|---|---|
| `undef` | 未定義オペコード | 動作未定義 |
| `jp_d_rb` | `jp.d %rb`（ハードウェアバグ） | DMA がディレイスロットをスキップ |
| `delay_slot_hard` | ディレイスロットに禁止命令（多サイクル・メモリアクセス・分岐） | 動作未定義 |
| `delay_slot_ext` | ディレイスロットに EXT 命令 | 次の命令に EXT が誤適用される |
| `misalign_read16` / `misalign_read32` | 非アライン読み込み（16/32ビット） | 実機はアドレス不整例外 |
| `misalign_write16` / `misalign_write32` | 非アライン書き込み（16/32ビット） | 実機はアドレス不整例外 |
| `bus_fault` | バスフォルト（上記 misalign で発生） | — |

#### Warning / Fault（デバッガ未接続: 警告ログ＋続行、接続中: エラー停止）

| category | 診断イベント | 実機での挙動 |
|---|---|---|
| `ext_incompat` | EXT + EXT 非対応命令（シフト・特殊レジスタ移動等） | EXT 黙って無視 |
| `ext_triple` | 3個以上の連続 EXT | 1個目と最後のものを使用、中間はすべて無視 |
| `delay_slot_soft` | ディレイスロットに D="-" 命令（1サイクル・メモリなし・EXT 不可、例: `nop`・`div1`） | 動作することが多い |

#### Warning（常に警告ログ＋続行）

| category | 診断イベント | 実機での挙動 |
|---|---|---|
| `delay_slot_sp_clobber` | `ret.d`/`call.d` のディレイスロットで SP を書き換える命令（`add/sub %sp, imm10`・`ld.w %sp, %rs`）を実行 | スタック破壊・誤ったアドレスへ分岐 |

**Fault 時の出力**: レジスタ（R0–R15・PC・SP・PSR・ALR・AHR）のダンプを stderr に出力して停止する。CI でテスト実行中に未定義命令等を踏んだ場合、即座に FAIL として検出できる。

**デバッガ接続中の Fault**: エラー終了ではなく GDB RSP の停止応答（T04、SIGTRAP 相当）を返す。開発者がレジスタ・メモリを調査してから実行を継続・中断できる。

**実アドレス不整例外との違い**: 実機は非アライン 16/32 ビットアクセスでトラップベクタに転送するが、エミュレータでは Fault として停止する。コンパイラのバグで非アラインアクセスが発生した場合、トラップに入る前に検出できる。

**Warning ログの資料的価値**: 既存 P/ECE ソフトを大量に動かして、Warning が出るが正常動作するパターンを集めれば、マニュアル未記載の「実は安全に動作する命令の組み合わせ」を特定するドキュメントになる。

### 2.7 命令デコーダ: 65536エントリテーブル

16bit命令空間の全65536パターンを配列として構築し、有効な命令にはデコード情報を、未定義パターンには無効オペコードマーカーを入れる。デコードと未定義検出が同時に達成され、テーブル引きなので速度にも寄与する。

---

## 3. 信頼性確立の順序

コンパイラとエミュレータを同時に開発する場合、バグの所在が不明になる問題を回避するため、以下の順序で信頼チェーンを構築する。

### 第1段階: CPUコア単体テスト

- 命令単位のテストスイートを作成（手書き＋実機突合）
- 各命令のバリエーション（キャリー有無、オーバーフロー境界、ゼロ結果等）を網羅
- テストケース生成は既存GCCツールチェイン（信頼済み）で行い、コンパイラに依存しない
- **LLVM TableGen からの自動テストベクタ生成**: llvm-mc でバイナリ生成 → llvm-objdump で期待出力取得 → エミュレータの逆アセンブラ出力および実行結果と自動比較

**重点テストケース（符号境界）:**
- `ext 0x1000 / add %r0, 0` — unsigned: +8192 になるべき
- `ext 0x1fff / and %r0, 0x3f` — signed: -1 (0xFFFFFFFF) になるべき
- `ext 0x1000 / cmp %r0, 0` vs `ext 0x1000 / add %r0, 0` — 同じ ext 値でも cmp は signed、add は unsigned

### 第2段階: 周辺デバイスの段階的追加

- タイマ、割り込み、DMA を一つずつ追加し、各デバイス独立にテスト
- テストプログラムは既存SDK (GCC) で作成

### 第3段階: デバッガ (GDB RSP) 実装

- 信頼できるCPUコア上にデバッガを構築
- 逆アセンブラ出力を LLVM llvm-objdump 出力と突合検証

### 第4段階: LLVM/Clang 出力の検証

- エミュレータのCPUコアは第1段階で独立検証済み
- 既存GCCで同じCコードをコンパイルし、GCC版とLLVM版の実行トレースを比較
- 問題が出れば「コンパイラの出力が悪い」と切り分け可能

### テスト戦略の3層構造

上記の「信頼チェーン」はテスト対象の順序を定めるものである。ここでは各段階で使用するテストの**粒度**を定める。

**フレームワーク**: Google Test。LLVM プロジェクト自体が使用しており、CMake 統合も成熟している。LLVM開発環境との認知負荷の統一を優先する。

#### 第1層: 純粋関数のユニットテスト（Google Test）

外部状態に依存しない純粋関数を対象とする。モックは不要。

| テスト対象 | 関数/モジュール | 検証内容 |
|-----------|---------------|---------|
| ext 即値合成 | `resolve_immediate()` | 符号あり/なし × ext 0/1/2個の全組み合わせ、符号境界値 |
| PSR フラグ計算 | `calc_add_flags()`, `calc_sub_flags()` 等 | N/Z/V/C の各フラグが正しくセット/クリアされるか |
| シフト量デコード | `decode_shift_imm4()` | imm4→シフト量マッピング（0000=0, ..., 0111=7, 1xxx=8） |
| 逆アセンブラ出力 | `disassemble()` | 命令バイト列→ニーモニック文字列の一致 |
| アドレスデコード | BCU のエリア判定ロジック | アドレス→エリア番号、ウェイトサイクル計算 |
| ELF パース | ELF ヘッダ・プログラムヘッダの解釈 | セグメント情報の正確な抽出 |

進捗報告書に記録されたバグ（SLA の V フラグ計算誤り、シフト量フィールド誤読）は、このレベルのテストで防げた。内部関数をテスト可能に切り出す設計を意識すること。

#### 第2層: コンポーネント単体テスト（Google Test + 実オブジェクト）

CPUコアやBCUを、最小限のメモリ環境とセットで直接叩く。モックフレームワークは使わず、テスト専用の簡易バス（フェイク）を実体として用意する。

```cpp
// テスト用の最小環境
class TestFixture : public ::testing::Test {
protected:
    Bus bus;        // RAM だけ繋いだ実オブジェクト
    Cpu cpu{bus};   // 実CPUコア

    void SetUp() override {
        // SRAM 領域に最小限の RAM をマップ
        bus.map_ram(0x100000, 0x1000);
        // テスト対象の命令列を書き込み
        cpu.reset();
        cpu.set_pc(0x100000);
    }

    // ヘルパー: 命令列を書き込んで N 命令実行
    void exec(std::initializer_list<uint16_t> insns) {
        uint32_t addr = 0x100000;
        for (auto w : insns) {
            bus.write16(addr, w);
            addr += 2;
        }
        cpu.set_pc(0x100000);
        for (size_t i = 0; i < insns.size(); ++i)
            cpu.step();
    }
};

TEST_F(TestFixture, AddImmUnsigned) {
    // add %r0, 5  (Class 3, imm6=5, rd=0)
    cpu.set_reg(0, 100);
    exec({0x6140});  // add r0, 5 のエンコーディング
    EXPECT_EQ(cpu.reg(0), 105);
    EXPECT_FALSE(cpu.psr().n);
    EXPECT_FALSE(cpu.psr().z);
}

TEST_F(TestFixture, ExtAddUnsigned) {
    // ext 0x1000 / add %r0, 0 → r0 += 8192 (unsigned)
    cpu.set_reg(0, 0);
    exec({0xD000, 0x6000});  // ext 0x1000, add r0, 0
    EXPECT_EQ(cpu.reg(0), 8192u);  // ゼロ拡張で +8192
}
```

これはモックではなくフェイク（簡易版の実オブジェクト）なので、テスト専用のインタフェース抽象化は不要。`piece_soc` / `piece_core` に含まれる実クラスをそのまま使う。

#### 第3層: システムテスト（ベアメタルELF実行）

手書きテスト（`tests/bare_metal/test_*.c`）と、ジェネレータ方式リグレッション試験（`tests/bare_metal/gen/`）の二段構成。いずれも S1C33 機械語で書いたテストプログラムをエミュレータで実行し、セミホスティングポートで PASS/FAIL を報告する。

**ジェネレータ方式試験（1,659 ケース）** — `tests/bare_metal/gen/gen_*.py` が Python オラクルつきでインラインアセンブリ C テストを生成する。現行の網羅範囲は次の通り:

| カテゴリ | ケース数 | 生成器 |
|---|---|---|
| ALU（add/sub/and/or/xor/not/cmp/ld imm）| 253 | `gen_alu.py` |
| メモリアクセス（Class 0/1 各種オフセット）| 129 | `gen_mem.py` |
| SP 相対ロード/ストア | 72 | `gen_sp.py` |
| シフト・ローテートほか | 628 | `gen_shift.py` |
| 乗除算・ステップ除算 | 69 | `gen_muldiv.py` |
| 分岐・遅延スロット | 236 | `gen_branch.py` |
| pushn / popn | 34 | `gen_pushpop.py` |
| MAC | 15 | `gen_mac.py` |
| ビット操作（bset/bclr/btst/bnot）| 164 | `gen_bitop.py` |
| 特殊レジスタ | 37 | `gen_special.py` |
| `call.d %rb` | 6 | `gen_calldrb.py` |
| int / reti / trap | 16 | `gen_trap.py` |

このスイートの整備過程で次の 3 件の不具合が発見・修正された（詳細は `git log`）:

- **エミュレータ**: 符号付きステップ除算（`div0s`/`div1`/`div2s`/`div3s`）が誤った商・剰余を生成していた。S1C33 の非復元型符号付き除算アルゴリズムに修正（legacy piemu から移植）。
- **エミュレータ**: `int imm2` がトラップ番号 `imm2` にディスパッチしていたが、仕様は `12 + imm2`（SW 例外、ベクタは base+48..60）が正しい。
- **LLVM バックエンド (S1C33)**: レジスタアロケータが `popn` を跨いで R0（callee-saved）を戻り値のステージングに用いて返り値を破壊することがあった。

これはリグレッションテストとして維持する。新機能追加や既存コードのリファクタリング時に、全体の動作が壊れていないことを保証する。

#### CMake 構成

```cmake
# 各ライブラリ層（それぞれの CMakeLists.txt で定義）
add_subdirectory(core)    # → libpiece_core.a
add_subdirectory(soc)     # → libpiece_soc.a  (PUBLIC: piece_core)
add_subdirectory(board)   # → piece_board INTERFACE (PUBLIC: piece_soc)
add_subdirectory(debug)   # → libpiece_debug.a (PUBLIC: piece_core)
add_subdirectory(host)    # 将来: SDL3

# 第1層・第2層: C++ ユニットテスト
enable_testing()
find_package(GTest REQUIRED)

add_executable(piece_unit_tests
    tests/unit/test_ext_imm.cpp
    tests/unit/test_psr_flags.cpp
    tests/unit/test_shift_decode.cpp
    tests/unit/test_disasm.cpp
    tests/unit/test_bcu.cpp
    tests/unit/test_cpu_instructions.cpp
    tests/unit/test_peripheral_intc.cpp
    tests/unit/test_peripheral_t8.cpp
    tests/unit/test_peripheral_t16.cpp
    tests/unit/test_peripheral_portctrl.cpp
    tests/unit/test_peripheral_rtc.cpp
)
target_link_libraries(piece_unit_tests PRIVATE piece_soc piece_core GTest::gtest_main)
add_test(NAME unit_tests COMMAND piece_unit_tests)

# 第3層: システムテスト（ベアメタルELF）
foreach(test IN ITEMS basic ext_imm alu_flags load_store shifts branches multiply div misc)
    add_test(
        NAME system_test_${test}
        COMMAND piece_emu ${CMAKE_SOURCE_DIR}/tests/bare_metal/test_${test}.elf
    )
endforeach()
```

`ninja test` で全層のテストが一括実行される。

---

## 4. セミホスティング（デバッグ専用ポート）

エミュレータ専用のデバッグ用I/Oポートを、実機には存在しない特殊メモリアドレスに配置する。ARM のセミホスティングや RISC-V の HTIF と同種の仕組みであり、テスト自動化・コンパイラデバッグに不可欠な基盤となる。

実機で同じコードを動かした場合は未使用メモリへの空振りアクセスになるだけなので、テストコードの実機互換性は壊れない。

なお、S1C33209 の外部バスは16bit幅だが、エミュレータのメモリハンドラが自由に実装する仮想デバイスなので、32bitワードアクセスで問題ない。

### 4.1 ベースアドレス

候補: `0x060000`（S1C33209 のデバッグベクタ周辺領域。量産機では未使用）。

### 4.2 ポート定義

```
DEBUG_BASE = 0x0060000

+0x00  CONSOLE_CHAR   (W)  1バイト出力。下位8bitを1文字としてホスト側コンソールに出力
+0x04  CONSOLE_STR    (W)  文字列ポインタ。エミュレータがそのアドレスからヌル終端まで読み出して出力
+0x08  TEST_RESULT    (W)  テスト判定。0=PASS, 非0=FAIL(値がエラーコード)。書き込み時点でエミュレータ停止
+0x0C  CYCLE_COUNT_LO (R)  サイクルカウンタ下位32bit（エミュレーション開始からの累計CPUサイクル数）
+0x10  CYCLE_COUNT_HI (R)  サイクルカウンタ上位32bit
+0x14  REG_SNAPSHOT   (W)  任意値書き込みで全レジスタ(R0〜R15, SP, PSR, ALR, AHR, PC)をホスト側ログに出力
+0x18  TRACE_CTL      (W)  実行トレース制御。1=開始, 0=停止
+0x1C  BKPT_SET       (W)  書き込んだアドレスにソフトウェアブレークポイントを設定
+0x20  BKPT_CLR       (W)  書き込んだアドレスのブレークポイントを解除
+0x24  HOST_TIME_MS   (R)  ホスト側の高精度時刻（ミリ秒）。ベンチマークのウォールクロック計測用
```

### 4.3 テストプログラム用ヘッダ

```c
/* piece_emu_debug.h — エミュレータ専用デバッグポート */
#ifndef PIECE_EMU_DEBUG_H
#define PIECE_EMU_DEBUG_H

#define EMU_DEBUG_BASE    0x00060000

#define EMU_CONSOLE_CHAR  (*(volatile char          *)(EMU_DEBUG_BASE + 0x00))
#define EMU_CONSOLE_STR   (*(volatile const char   **)(EMU_DEBUG_BASE + 0x04))
#define EMU_TEST_RESULT   (*(volatile int           *)(EMU_DEBUG_BASE + 0x08))
#define EMU_CYCLE_LO      (*(volatile unsigned int  *)(EMU_DEBUG_BASE + 0x0C))
#define EMU_CYCLE_HI      (*(volatile unsigned int  *)(EMU_DEBUG_BASE + 0x10))
#define EMU_REG_SNAPSHOT  (*(volatile int           *)(EMU_DEBUG_BASE + 0x14))
#define EMU_TRACE_CTL     (*(volatile int           *)(EMU_DEBUG_BASE + 0x18))
#define EMU_BKPT_SET      (*(volatile unsigned int  *)(EMU_DEBUG_BASE + 0x1C))
#define EMU_BKPT_CLR      (*(volatile unsigned int  *)(EMU_DEBUG_BASE + 0x20))
#define EMU_HOST_TIME_MS  (*(volatile unsigned int  *)(EMU_DEBUG_BASE + 0x24))

static inline void emu_putchar(char c)          { EMU_CONSOLE_CHAR = c; }
static inline void emu_puts(const char *s)      { EMU_CONSOLE_STR = s; }
static inline void emu_pass(void)               { EMU_TEST_RESULT = 0; }
static inline void emu_fail(int code)           { EMU_TEST_RESULT = code; }
static inline void emu_snapshot(void)           { EMU_REG_SNAPSHOT = 1; }
static inline void emu_trace_on(void)           { EMU_TRACE_CTL = 1; }
static inline void emu_trace_off(void)          { EMU_TRACE_CTL = 0; }
static inline void emu_breakpoint(void *addr)   { EMU_BKPT_SET = (unsigned int)addr; }

static inline unsigned long long emu_cycles(void) {
    unsigned int lo = EMU_CYCLE_LO;
    unsigned int hi = EMU_CYCLE_HI;
    return ((unsigned long long)hi << 32) | lo;
}

#endif /* PIECE_EMU_DEBUG_H */
```

### 4.4 用途別の活用例

**CPUコアのテスト自動化:**
```c
/* test_add.c — add命令の符号境界テスト */
#include "piece_emu_debug.h"

void test_add_unsigned_ext(void) {
    int result;
    /* ext 0x1000 / add %r0, 0  →  r0 += 8192 (unsigned) */
    asm volatile(
        "ld.w %r0, 0\n"
        "ext 0x1000\n"
        "add %r0, 0\n"
        "ld.w %0, %%r0\n"
        : "=r"(result));
    if (result == 8192)
        emu_pass();
    else
        emu_fail(1);
}
```

**コンパイラ出力のトレース比較:**
```c
emu_trace_on();
int result = target_function(arg1, arg2);
emu_trace_off();
emu_puts("result = ");
/* ... 結果出力 ... */
```

GCC版とLLVM版で同じコードを実行し、トレースログを diff すれば、コード生成の差異が一目瞭然。

**CI統合:**
```bash
# テストプログラムを実行し、終了コードで判定
./piece-emu --headless --timeout 5s test_suite.pfi
echo "Exit code: $?"   # 0=全テストPASS, 非0=いずれかFAIL
```

`--headless` モードではLCD描画・サウンドを無効化し、TEST_RESULT ポートへの書き込みで終了する。

### 4.5 実装状況（2026-04時点）

全ポートが `src/debug/semihosting.cpp` に実装済み。テストプログラム向けヘッダは 2 種類ある。

| ファイル | 用途 |
|---|---|
| `src/tests/bare_metal/semihosting.h` | 既存のベアメタルテスト用（`semi_*` 関数群）|
| `src/tests/bare_metal/piece_emu_debug.h` | 新規テスト向け完全版（`EMU_*` マクロ + `semi_*` 相当のインライン関数）|

#### CONSOLE_STR のアドレス実装について

設計仕様（上記 §4.2）は `+0x04` と記載しているが、S1C33 の 32 ビット書き込みは 2 回の 16 ビット bus write に分割される。実装では次の 2 ハーフワードをそれぞれ登録している。

```
+0x02  CONSOLE_STR lo (W16) 文字列アドレスの下位16ビットをラッチ
+0x04  CONSOLE_STR hi (W16) 上位16ビット受信時に (hi<<16)|lo を合成してから文字列を出力
```

`*(volatile uint32_t*)0x060002u = ptr;` という 32 ビット書き込みを行うと、
lo が +0x02 に書かれてラッチされ、hi が +0x04 に書かれた時点で出力がトリガされる。
このため、バーストモードのベアメタルコードおよび `semihosting.h` の `semi_puts()` と完全互換である。

#### BKPT_SET はヘッドレスモード専用 ⚠️

`BKPT_SET` / `BKPT_CLR` が操作するブレークポイントセット（`Cpu::breakpoints`）と、
GDB RSP スタブが管理するブレークポイントセット（`GdbRsp::breakpoints_`）は**現在は別物**である。

| モード | 発火時の動作 |
|---|---|
| ヘッドレス（`piece-emu` 直接実行）| `cpu_.step()` 内でレジスタダンプを stderr に出力し halt。正常動作。|
| GDB 接続中 | `cpu_.step()` が halt させるため `GdbRsp::run()` はループを抜けるが、GDB には「不明な halt」として届く。SIGTRAP 理由は伝わらず、GDB 側でブレークポイントとして認識されない。|

**将来の修正方針：** ブレークポイントチェックを `cpu_.step()` から除去し、
呼び出し側（ヘッドレスの main ループと `GdbRsp::run()`）でそれぞれ `cpu_.breakpoints` を
参照するように変更する。`GdbRsp::run()` は自身の `breakpoints_` と `cpu_.breakpoints` の
和集合をチェックすることで、セミホスティング経由のブレークポイントも SIGTRAP として返せる。

```cpp
// GdbRsp::run() の修正イメージ
if (breakpoints_.count(cpu_.state.pc) ||
    cpu_.breakpoints.count(cpu_.state.pc))
    break;  // → S05 SIGTRAP として返る
```

また、GDB 接続時に `gdb_rsp.cpp` が `breakpoints_.clear()` するのと同様に
`cpu_.breakpoints.clear()` も実行する必要がある。

---

## 5. エラッタ（エミュレータに影響するもの）

`errata.md` から、エミュレータ実装に直接影響する項目を抜粋する。

### 5.1 必ず再現すべきハードウェアバグ

**`jp.d %rb` の誤動作 (errata #1):**
- 条件A: 直前にメモリアクセス命令があるとディレイスロット未実行
- 条件B: DMA転送中にも同じ誤動作（プログラムで回避不可能）
- 初期実装では仕様通り正しく実装し、後に実機互換モードを追加

### 5.2 CPUコアに影響

**PSRフラグの暗黙更新 (errata #11):**
- ほぼ全ALU命令がフラグ更新する。テストスイートで網羅的に検証必須

**sin() の Vフラグ問題 (errata #8):**
- ライブラリ内で `xjrge` (条件: `!(N^V)`) を使用、直前のオーバーフロー演算のVフラグ残留で結果反転
- エミュレータのフラグ実装が正しければ自然に再現されるはず → フラグ実装の検証に利用可能

**シフト命令は ext 不可:**
- ext 蓄積中にシフト命令が来たら ext を無視

### 5.3 LLVMバックエンドで発見されたエンコーディング修正値

以下の op1 値は LLVM 実装中に実バイナリとの突合で修正されたもの。エミュレータのデコードテーブルにはこの修正後の値を使用すること。CPUマニュアルの記載とも突合して確認すべき。

**ld.b/ld.ub/ld.h/ld.uh レジスタ間（Class 5, op2=01）:**

| 命令 | 正しい op1 |
|------|-----------|
| `ld.b %rd, %rs` | 000 |
| `ld.ub %rd, %rs` | 001 |
| `ld.h %rd, %rs` | 010 |
| `ld.uh %rd, %rs` | 011 |

**swap/mirror/scan0/scan1（Class 4, op2=10）:**

| 命令 | 正しい op1 |
|------|-----------|
| `scan0 %rd, %rs` | 010 |
| `scan1 %rd, %rs` | 011 |
| `swap %rd, %rs` | 100 |
| `mirror %rd, %rs` | 101 |

**div0s〜div3s（Class 4, op2=11）:**

| 命令 | 正しい op1 |
|------|-----------|
| `div0s %rd, %rs` | 010 |
| `div0u %rd, %rs` | 011 |
| `div1 %rd, %rs` | 100 |
| `div2s %rd, %rs` | 101 |
| `div3s` | 110 |

**btst/bclr/bset/bnot/adc/sbc（Class 5, op2=00）:**

| op1 | 命令 |
|-----|------|
| 000 | `ld.w %special, %rs` |
| 001 | `ld.w %rd, %special` |
| 010 | `btst [%rb], imm3` |
| 011 | `bclr [%rb], imm3` |
| 100 | `bset [%rb], imm3` |
| 101 | `bnot [%rb], imm3` |
| 110 | `adc %rd, %rs` |
| 111 | `sbc %rd, %rs` |

**特殊レジスタ番号:**

| 特殊レジスタ | 正しい番号 |
|------------|----------|
| PSR | 0 |
| SP | 1 |
| ALR | 2 |
| AHR | 3 |

**PC相対分岐のオフセット計算（確定式）:**
```
target = PC + 2 * sign8    （PC = 分岐命令自身のアドレス、+2 は不要）
```

### 5.4 EXT+target 不可分性とディレイドブランチ不可分性（実装済み）

**EXT+target 不可分性（CPU マニュアル記載）:**
「ext命令と拡張対象の命令の間は、リセットとアドレス不整例外を除くトラップはハードウェアによりマスクされ、発生しません。」

**ディレイドブランチ+スロット不可分性:**
分岐命令（call.d / jp.d / jr.d）とそのディレイスロット命令の間も同様に、トラップは発生しません。ディレイスロット命令の完了後（分岐先アドレス適用後）に発火します。

**エミュレータの実装方針（`do_trap` / `step`）:**

周辺デバイスが `assert_trap()` を呼んだタイミングで `pending_ext_count > 0` または `in_delay_slot == true` の場合、トラップを即座に取らず defer する：

```cpp
// do_trap() の冒頭（通常のマスク可能チェックの後）
if (state.pending_ext_count > 0 || state.in_delay_slot) {
    state.deferred_trap_valid = true;
    state.deferred_trap_no    = no;
    state.deferred_trap_level = level;
    return;
}
```

`step()` のディレイスロット完了後に発火する：

```cpp
// step() の末尾（delay slot completion の後）
if (!state.pending_ext_count && !state.in_delay_slot
    && state.deferred_trap_valid && !state.fault) {
    state.deferred_trap_valid = false;
    do_trap(state.deferred_trap_no, state.deferred_trap_level);
}
```

これにより：
- EXT+target が完全に実行された後、正しい PC（target+2）でトラップが発火する
- `reti` で target+2 に戻るため、ターゲット命令が EXT なしで再実行される問題が解消される
- ディレイドブランチの場合は分岐先 PC で保存されるため、`reti` 後に正しいアドレスへ復帰する

**アドレス不整例外については**、バスアクセス中に `bus_.take_fault()` で検出し、`diag_fault` を通じて CPU を停止させる（`reti` からは戻れないため defer 機構の対象外）。

---

## 6. 参照実装の分析指針

### 6.1 P/EMU/SDL (piemu) の分析ポイント

`revert-to-single-thread` ブランチを優先的に確認する。

| ファイル | 確認すべき点 |
|---------|------------|
| `core/` | CPUコアのデコード方式（テーブル引き or switch-case）、ext 状態管理、フラグ更新の正確性 |
| `bcu.c` | アドレスデコーディング、ウェイトサイクル処理 |
| `iomem.c` | I/Oレジスタのマッピング、周辺デバイスとの接続 |
| `lcdc.c` | S6B0741 コマンド解釈、4階調の表現方法 |
| `flash.c` | SST39VF400A のコマンドシーケンスエミュレーション |
| `sram.c` / `fram.c` | メモリアクセスの実装 |
| `piemu.c` | メインループ構造、フレームタイミング |
| `pfi.h` | PFI フラッシュイメージフォーマット |

**既知の課題（TODO に記載されていたもの）:**
- Flash 書き戻し未対応
- USB エミュレーション未実装
- MMC/SD エミュレーション未実装
- エンディアン依存コード多数
- 速度が遅い

### 6.2 MAME C33 の分析ポイント

**ライセンス注意: GPL-2.0。コードの直接流用は不可。仕様・設計思想の参照のみ。**

MAME の設計で特に倣うべきは、CPUデバイスとメモリ空間の抽象化構造である：

- **`address_space_config`**: メモリ空間をアドレス幅・データ幅・エンディアンで宣言的に定義する。本エミュレータでも、BCUのエリア定義をデータ駆動（テーブル定義）で記述し、エリア追加や設定変更がコード変更なしで行えるようにする
- **`memory_access::cache` / `::specific`**: 命令フェッチ用の高速キャッシュとデバイスI/O用のディスパッチを分離する。本エミュレータでもフェッチパス（内蔵RAM・Flash）とI/Oパス（周辺レジスタ）を分離し、フェッチの高速化を図る
- **`read/write handler` のバインディング**: I/Oレジスタごとにread/writeハンドラを個別に登録する。ハンドラのシグネチャが統一されており、デバイスの追加が容易
- **`device_start()` / `device_reset()`**: デバイスのライフサイクルが明確に定義されている。ステートセーブ/ロードに必要な状態の列挙が `save_item()` で宣言的に行える
- **`state_add()`**: デバッガに公開するレジスタをフラットに列挙する。GDB RSP のレジスタマップに直結する

本エミュレータでは GPL コードは一切含めないが、上記の設計パターン（宣言的メモリマップ定義、ハンドラ登録ベースのI/Oディスパッチ、ライフサイクル管理）をクリーンルーム実装する。

| ファイル | 確認すべき点 |
|---------|------------|
| `c33dasm.cpp/h` | 逆アセンブラの ext 処理方式、命令デコードテーブル構造、op1 値の正誤 |
| `c33helpers.ipp` | PSR ビット定義（STD/ADV 共通） |
| `c33std.cpp/h` | レジスタ構造体、メモリ空間定義、デバイスライフサイクル |
| `s1c33209.cpp/h` | S1C33209 固有の内蔵メモリマップ、内蔵RAM・I/O領域の配置 |
| `aquaplus_piece.cpp` (`src/mame/skeleton/`) | P/ECE ドライバのアドレスマップ定義（Flash・SRAMの配置の答え合わせ） |

**特に重要**: `c33dasm.cpp` の逆アセンブラと LLVM `S1C33Disassembler` の出力を同一バイナリで比較し、不一致があれば原因を特定すること。3実装（MAME、LLVM、本エミュレータ）が一致すれば逆アセンブラの信頼性が高い。

---

## 7. 実装優先順位

| 優先度 | コンポーネント | 詳細 |
|--------|-------------|------|
| **P0** | CPUコア（命令デコード＋実行） | 65536エントリテーブル、ext 蓄積、フラグ更新 |
| **P0** | BCU（バスコントロールユニット） | エリア単位のアドレスデコード、ウェイトサイクル、デバイスサイズ。§1.4参照 |
| **P0** | メモリサブシステム | 内蔵RAM、SRAM、Flash（読み出し）、I/Oレジスタ（スタブ） |
| **P0** | ELFローダ（ベアメタル） | プログラムヘッダに従いセグメントをメモリにロード、エントリポイントからPC開始 |
| **P0** | セミホスティングポート | §4 のデバッグ専用ポート。最低限 TEST_RESULT と CONSOLE_CHAR/STR |
| **P0** | 逆アセンブラ | 蓄積→合成方式、LLVM出力と突合検証 |
| **P0** | CPUテストスイート | 命令単位テスト、符号境界テスト |
| **P0** | GDB RSPデバッガ | ブレークポイント、ステップ実行、レジスタ/メモリ読み書き。初期の主要UIとなる |
| **P1** ✅ | タイマ | 8bit×4, 16bit×6, 計時タイマ（HLT/SLEEPからの起床に必須） |
| **P1** ✅ | 割り込みコントローラ | 割り込み受付、レベル制御 |
| **P1** ✅ | HLT/SLEEP + イベント駆動スリープ | ホストCPU使用率低減 |
| **P1** ✅ | クロック切替 | P07書き込み検出、24/48MHz切替、タイマ発火間隔への反映 |
| **P2-1** ✅ | PFIイメージローダ | フルシステムエミュレーション（カーネル起動→AppStart到達確認） |
| **P2-1** ✅ | SIF3 シリアル I/F | TXD/STATUS/CTL レジスタ、HSDMA Ch0 インライン DMA |
| **P2-1** ✅ | HSDMA 高速 DMA | 4 チャネル、Ch0=LCD 転送、Ch1=サウンドコールバック |
| **P2-2** ✅ | LCDコントローラ | S6B0741 SPI コマンド解釈、128×88 4階調表示（`src/board/s6b0741`） |
| **P2-2** ✅ | SDL3 フロントエンド | `piece-emu-system`：LCD 表示・ボタン入力・60fps・非同期 GDB RSP |
| **P2-2** ✅ | ボタン入力 | SDL3 キーイベント → K5D/K6D マッピング（piece-emu-system 内） |
| **P2-4** | PWMサウンド | HSDMA Ch1 + SDL3 オーディオ出力 |
| **P2-5** | Flash書き込み | SST39VF400A コマンドシーケンス、PFI書き戻し |
| **P3** | USB (PDIUSBD12) | 将来拡張 |
| **P3** | 赤外線通信 | 将来拡張 |
| **P3** | MMC/SD | 将来拡張 |

---

## 8. PFI フラッシュイメージフォーマット

P/EMU/SDL が使用するフラッシュメモリイメージ形式。すべてリトルエンディアン。

```
DWORD dwSignature      ファイル識別子 'PFI1' (バイト列: "1IFP")
DWORD dwOffsetToFlash  フラッシュメモリイメージの開始オフセット
SYSTEMINFO siSysInfo   P/ECE システム情報構造体
（フラッシュメモリイメージが続く）
```

フラッシュメモリイメージは S1C33209 のアドレス 0xC00000 にマッピングされる。

`all.bin`（カーネルアップデートファイル）から PFI を生成する場合、先頭2セクタ（8192バイト、緊急カーネル）が欠けているため、0xC00000 に `0x00C02004`（通常カーネルエントリポイント）を書き込んで捏造する。

---

## 9. P/ECE ABI 概要（デバッガのスタックトレース用）

| 項目 | 内容 |
|------|------|
| Callee-saved | R0〜R3 |
| Scratch (caller-saved) | R4〜R7, R9 |
| カーネルテーブルベース | R8（常に 0x0、Reserved） |
| 戻り値 | R10（32bit以下）、R10+R11（64bit） |
| 引数 | R12〜R15（最大4ワード） |
| 可変引数関数 | R12〜R15 不使用、全引数スタック渡し |
| 構造体引数 | すべてスタック渡し |
| フレームポインタ | なし（SPベースのみ） |
| call命令 | リターンアドレスをスタックにプッシュ |
| ret命令 | スタックからポップして復帰 |
| レジスタ退避 | pushn/popn で操作 |

---
