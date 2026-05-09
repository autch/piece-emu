# piece-emu — libretro Core

[piece-emu](../../README.md) の libretro コア版です。RetroArch などの libretro 互換フロントエンドから P/ECE エミュレータを起動できます。

This is a libretro core packaging of [piece-emu](../../README.md). Use it with RetroArch (or any libretro-compatible frontend) to run P/ECE titles directly from the frontend's UI.

---

## 配布物 / What You Need

バイナリ配布版を使う場合、以下のファイルが揃っていることを確認してください。

If you're using the binary distribution, make sure you have these files:

| ファイル / File | 役割 / Purpose |
|---|---|
| `piece_libretro.dll` (Windows) / `piece_libretro.so` (Linux) / `piece_libretro.dylib` (macOS) | コア本体 / The core library |
| `piece_libretro.info` | コア情報ファイル（メニュー UI 用ラベルとファイル拡張子フィルタ）/ Core info file (menu labels + file extension filter) |
| `piece.pfi` | P/ECE フラッシュイメージ（コンテンツ）/ P/ECE flash image (content) |

`piece.pfi` の作り方は [トップ README の「PFI イメージを用意する」](../../README.md#pfi-イメージを用意する--preparing-a-pfi-image) を参照してください。`mkpfi` と `pfar` も同じバイナリ配布に同梱されています。

For how to build `piece.pfi`, see [Preparing a PFI Image](../../README.md#pfi-イメージを用意する--preparing-a-pfi-image) in the top-level README. The `mkpfi` and `pfar` tools are bundled in the same binary distribution.

---

## インストール / Installation

RetroArch の **コアディレクトリ** と **コア情報ディレクトリ** にそれぞれファイルを配置します。両ディレクトリの場所は RetroArch の `Settings → Directory` で確認できます。

Place the core into RetroArch's **Cores** directory and the info file into the **Core Info** directory. You can confirm both paths under `Settings → Directory` inside RetroArch.

| OS | コアディレクトリ / Cores | コア情報ディレクトリ / Core Info |
|---|---|---|
| Windows | `<RetroArch>\cores\` | `<RetroArch>\info\` |
| Linux | `~/.config/retroarch/cores/` | `~/.config/retroarch/info/` |
| macOS | `~/Library/Application Support/RetroArch/cores/` | `~/Library/Application Support/RetroArch/info/` |

```sh
# Linux 例 / Linux example
cp piece_libretro.so   ~/.config/retroarch/cores/
cp piece_libretro.info ~/.config/retroarch/info/
```

**設置後は RetroArch を一度再起動してください。** 起動時に info ファイルがキャッシュされるため、再起動しないとファイル選択ダイアログのフィルタなどが反映されません。

**Restart RetroArch after installing.** The info file is cached at startup, so a restart is required for menu features (file picker filter, etc.) to take effect.

---

## 使い方 / Running

### メニュー UI 経由 / From the Menu UI

1. メイン画面 → **Load Core** → `piece-emu` を選択
2. **Load Content** → `piece.pfi` を選択（拡張子フィルタで `.pfi` のみが表示されます）
3. ゲームのランチャーが起動します

Steps:

1. Main Menu → **Load Core** → select `piece-emu`
2. **Load Content** → select `piece.pfi` (the picker filters to `.pfi`)
3. The launcher boots

### コマンドライン経由 / From the Command Line

```sh
retroarch -L /path/to/piece_libretro.so /path/to/piece.pfi
```

`-L` で直接コアを指定する場合、コア情報ファイルは必須ではありませんが、設置しておくと Quick Menu のラベルが正しく出ます。

When using `-L`, the core info file isn't strictly required, but installing it gives proper labels in the Quick Menu.

---

## 入力マッピング / Input Mapping

| RetroArch JOYPAD | P/ECE ボタン / P/ECE button |
|---|---|
| D-Pad Up / Down / Left / Right | 十字キー / D-pad |
| `A` (right face button)  | A |
| `B` (bottom face button) | B |
| `Start` | START |
| `Select` | SELECT |

ラベルは `Quick Menu → Controls → Port 1 Controls` で「D-Pad Up」「A Button」のように P/ECE 固有名で表示されます。RetroArch のキーマップ・ゲームパッドマッピング機能から自由に再割り当て可能です。

Labels appear under `Quick Menu → Controls → Port 1 Controls` as P/ECE-specific names. You can rebind them freely via RetroArch's keyboard / gamepad mapping settings.

---

## コアオプション / Core Options

`Quick Menu → Options` から設定できます。Configurable under `Quick Menu → Options`.

| オプション / Option | 既定値 / Default | 説明 / Description |
|---|---|---|
| **Swap A/B Buttons** | Off | Off: RetroArch 標準（A=P/ECE A、B=P/ECE B）。On: P/ECE 物理配置を再現（A=P/ECE B、B=P/ECE A）／Off: RetroArch standard. On: P/ECE physical layout (right face = B, left face = A). |

---

## セーブデータ / Save Data (`.srm`)

カーネルや各アプリが PFFS（P/ECE のオンフラッシュファイルシステム）に書き込んだ内容（ハイスコア・設定・ユーザデータ）は、RetroArch の `<saves>/<contentname>.srm` に自動保存されます。書き出しタイミングは RetroArch の `Settings → Saving → SaveRAM Autosave Interval` で制御できます（既定: コンテンツ終了時のみ）。

PFFS writes (high scores, settings, user data) made by the kernel or by each application are automatically saved to `<saves>/<contentname>.srm` by RetroArch. Write timing is controlled by `Settings → Saving → SaveRAM Autosave Interval` (default: only on content close).

### `.srm` は完全な PFI ファイル / `.srm` is a Complete PFI File

このコアの `.srm` は **PFI フォーマットそのもの**で書き出されます。つまり:

The `.srm` produced by this core **is** a PFI file. You can:

- 同梱の `pfar` で `.srm` の中身を直接覗ける / List or extract files inside the `.srm` using the bundled `pfar`:
  ```sh
  pfar mygame.srm -l                  # PFFS 内のファイル一覧 / list contents
  pfar mygame.srm -e somefile.dat     # ファイル抽出 / extract a file
  ```
- `.srm` をそのまま `.pfi` にリネームすれば、それ自体が起動可能な PFI イメージになる / Rename `.srm` to `.pfi` and it boots as a self-contained PFI image — useful for backing up a save state as a redistributable bootable image
- `fusepfi`（Linux/macOS）で `.srm` を FUSE マウントしてファイル単位で編集可能 / On Linux/macOS, `fusepfi` mounts the `.srm` as a filesystem for per-file editing

セーブデータをセーブステート的に瞬間的に巻き戻す機能はまだ実装されていません（後述の Phase 2 制限を参照）。

There is no instant rewind / save state mechanism yet (see "Limitations" below).

---

## 制限事項 / Limitations

現バージョン（Phase 2）の制限です。Current limitations (Phase 2):

- **セーブステート (`Save State` / `Load State`) 未対応** — RetroArch のメニューから「ステートを保存」しても何も起こりません。フラッシュへの永続化（`.srm`）は動作します。
  Save / Load State is not implemented. Frontend menu options for it are no-ops. Flash persistence via `.srm` works.
- **リワインド未対応** — 上記と同じ理由。
  Rewind is not supported (same reason).
- **未実装周辺デバイス** — USB / 赤外線 (IrDA) / MMC / ADC / IDMA。これらに依存するアプリは動かないか、機能制限が出ます。
  Unimplemented peripherals: USB, IrDA, MMC, ADC, IDMA. Apps that rely on these will not work or will work with reduced functionality.

---

## トラブルシューティング / Troubleshooting

| 症状 / Symptom | 原因と対処 / Cause and fix |
|---|---|
| **Load Content 時に `.pfi` が選択肢に出ない** / `.pfi` files don't appear in Load Content | `piece_libretro.info` が info ディレクトリにないか、RetroArch を再起動していない。インストール手順を再確認してください。 / `piece_libretro.info` isn't installed in the info directory, or RetroArch hasn't been restarted. Recheck the installation steps. |
| **Quick Menu のボタン名が "Joypad B" など汎用名のまま** / Generic button names ("Joypad B" etc.) in Quick Menu | 同上。info ファイル経由で input descriptor が読まれていません。 / Same as above — info file not picked up. |
| **コアは読み込まれるがコンテンツが起動しない** / Core loads but content fails | `.pfi` が空 or 破損。`pfar piece.pfi -v` で SYSTEMINFO を確認してください。 / The `.pfi` is empty or corrupted. Inspect with `pfar piece.pfi -v`. |
| **音が出ない** / No audio | RetroArch の `Settings → Audio → Mute` を確認。あるいはコア側ですべての PFFS 操作中はカーネル都合で発音が止まることがあります。 / Check `Settings → Audio → Mute`. Also, audio may briefly stall while the kernel is committing PFFS writes — this is normal P/ECE kernel behaviour. |
| **ハイスコアが保存されない** / High scores aren't saved | RetroArch を `Close Content` で正しく終了しているか確認。プロセス強制終了では `.srm` が書き出されません。あるいは `Settings → Saving → SaveRAM Autosave Interval` を 0 以外（例: 30 秒）に設定。 / Make sure you exit via `Close Content`. Killing the process won't flush the `.srm`. Alternatively, set `Settings → Saving → SaveRAM Autosave Interval` to a non-zero value (e.g. 30s). |

---

## ライセンス / License

本コアは [MIT ライセンス](../../LICENSE) で配布されています（プロジェクト全体と同じ）。`libretro.h` ヘッダは libretro プロジェクトのもので、こちらも MIT 互換ライセンスで配布されています。

This core is distributed under the [MIT License](../../LICENSE), the same as the rest of the project. The bundled `libretro.h` header comes from the libretro project and is also MIT-compatible.
