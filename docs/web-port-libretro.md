# piece_libretro の Web 移植 — 動作記録とパッチ手順

**対象読者**: piece-emu を Web ブラウザ上で動かしたい人 / エージェント
**対応バイナリ**: `build-em/libretro/piece_libretro.a`(Emscripten ビルド成果物)
**実装ファイル**: `src/CMakeLists.txt`, `src/libretro/CMakeLists.txt`

---

## 目的

P/ECE エミュレータを **RetroArch Web** のコアとして組み込み、Web ブラウザ上で動作させる。
ネイティブの SDL3 フロントエンド(`piece-emu-system`)や CLI フロントエンド(`piece-emu`)を入れられない環境(他人の PC、スマホ、xrdp セッション等)向けの配布経路として位置づける。

到達点(2026-05-19 時点):

- `piece_libretro.a` を Emscripten で生成可能
- RetroArch Web に静的リンクして読み込みが成功
- ファイルシステム初期化完了まで到達、Run ボタン有効化を確認
- 別ホストの Chrome から接続し、コアが実走行することを確認

---

## ビルド方法

ネイティブビルドと同じ CMake ツリーから、Emscripten ツールチェーンに切り替えて configure するだけで `piece_libretro` 単独ターゲットがビルドできる。

```sh
source /path/to/emsdk/emsdk_env.sh
emcmake cmake -S src -B build-em -G Ninja
ninja -C build-em piece_libretro
# → build-em/libretro/piece_libretro.a
```

CMake 側で行っている分岐(詳細は [src/CMakeLists.txt](../src/CMakeLists.txt) と [src/libretro/CMakeLists.txt](../src/libretro/CMakeLists.txt)):

- `if(EMSCRIPTEN)` で `PIECE_NATIVE_FRONTENDS=OFF` を立てる
- `find_package(GTest/CLI11/SDL3)` をネイティブ時のみ実行(vcpkg 非依存)
- `add_subdirectory(debug/host/tools)` をネイティブ時のみ
- `piece_writeback` / `piece-emu` / `piece-emu-headless-system` / `piece-emu-system` / GTest テスト群もネイティブ時のみ
- `piece_libretro` は Emscripten 時 `STATIC`(`.a`)、ネイティブ時 `SHARED`(`.so` / `.dll` / `.dylib`)

`vcpkg` ツールチェーンファイルは Emscripten ビルドでは指定しない(`emcmake` が Emscripten のツールチェーンを注入する)。

### Emscripten のバージョン

検証時の組み合わせ:

- Emscripten 5.0.7(263db4cffa6f9fc2ec514a70abac81362ea41849)
- piece_libretro 側で POSIX 依存はなし(`piece_libretro.cpp` / `pfi_loader_blob.cpp` / `piece_peripherals.cpp` のいずれも `mmap` / `pthread` / signal 等を使っていない)

---

## RetroArch Web への組み込み

`piece_libretro.a` を RetroArch の Emscripten ビルドに `STATIC_LIBS` として渡し、`piece_libretro_libretro.js` / `piece_libretro.wasm`(本検証では `piece_libretro.{js,wasm}` という命名で配信)を生成する。配信側のレイアウトは典型的に次のような形になる(本検証で動かした構成):

```
/                       index.html, libretro.css, libretro.js
/                       core_list.js, browserfs.min.js
/                       piece_libretro.js, piece_libretro.wasm
/assets/frontend/       bundle.zip.aa, bundle.zip.ab
/assets/cores/          .index-xhr
```

`core_list.js` で `piece` を登録:

```js
const libretroCores = {
    "piece": "Aquaplus - PIECE"
};
```

---

## ⚠ 既知の互換性問題 — BrowserFS × 新世代 Emscripten

RetroArch Web に同梱されている `browserfs.min.js`(BrowserFS の `EmscriptenFS` アダプタ)は、`node_ops` / `stream_ops` のメソッドをプロトタイプ上に定義し、メソッド本体で `this.fs.realPath(...)` を参照している。

一方、Emscripten 4.x 以降(本検証は 5.0.7)の `FS` 実装は、`doSetAttr` / `doChmod` / `stat` などの内部実装でメソッドを次の形で抜き出して呼ぶ:

```js
doSetAttr(stream, node, attr) {
    var setattr = stream?.stream_ops.setattr;
    var arg = setattr ? stream : node;
    setattr ??= node.node_ops.setattr;
    setattr(arg, attr);          // ← this バインドなし
}
```

抜き出された関数が strict mode で呼ばれるため `this === undefined`、BrowserFS 側で `this.fs.realPath(...)` を読みに行って次のクラッシュになる:

```
Uncaught TypeError: Cannot read properties of undefined (reading 'fs')
    at qe.setattr (browserfs.min.js:2:5119)
    at Object.doSetAttr (piece_libretro.js:3:58972)
    at Object.doChmod (piece_libretro.js:3:66118)
    at Object.chmod (piece_libretro.js:3:66353)
    at Object.open (piece_libretro.js:3:68801)
    at Object.writeFile (piece_libretro.js:3:71811)
    at mountBrowserFS (libretro.js:179:14)
    at finishFileSystemSetup (libretro.js:194:4)
```

これは **RetroArch Web 同梱の BrowserFS と新世代 Emscripten の非互換** であり、`piece_libretro.a` 側を変えても直らない。piece-emu のビルド成果物自体は正常。

### パッチ手順

RetroArch Web の `index.html` の中で `browserfs.min.js` を読み込んだ **直後**、`libretro.js` を読み込む **前** に以下のインラインスクリプトを差し込む。`BrowserFS.EmscriptenFS` のコンストラクタをラップし、生成された各インスタンスの `node_ops` / `stream_ops` メソッドを `bind()` 済みの own プロパティとして焼き付ける:

```html
<script src="browserfs.min.js"></script>
<script>
  // Bind BrowserFS.EmscriptenFS node_ops/stream_ops methods to their owning
  // object.  Required because Emscripten 4.x+ extracts node_ops methods
  // (`var fn = ops.foo; fn(...)`) without preserving `this`, which breaks
  // BrowserFS's adapter that relies on `this.fs.realPath(...)`.
  (function () {
    function bindOpsObject(opsObj) {
      if (!opsObj) return;
      let proto = Object.getPrototypeOf(opsObj);
      while (proto && proto !== Object.prototype) {
        for (const k of Object.getOwnPropertyNames(proto)) {
          if (k === 'constructor') continue;
          const v = proto[k];
          if (typeof v === 'function' &&
              !Object.prototype.hasOwnProperty.call(opsObj, k)) {
            opsObj[k] = v.bind(opsObj);
          }
        }
        proto = Object.getPrototypeOf(proto);
      }
    }
    const OrigEFS = BrowserFS.EmscriptenFS;
    function PatchedEFS(...args) {
      const inst = new OrigEFS(...args);
      bindOpsObject(inst.node_ops);
      bindOpsObject(inst.stream_ops);
      return inst;
    }
    PatchedEFS.prototype = OrigEFS.prototype;
    BrowserFS.EmscriptenFS = PatchedEFS;
  })();
</script>
<script src="core_list.js"></script>
<script src="libretro.js"></script>
```

### パッチ適用後のコンソール

成功時に観察できるログ列:

```
WEBPLAYER: xhrfs setup successful
WEBPLAYER: wasm runtime initialized
WEBPLAYER: idbfs setup successful
WEBPLAYER: zipfs setup successful
WEBPLAYER: filesystem initialization successful
```

`Uncaught TypeError ... reading 'fs'` が出なくなり、`Run` ボタンが活性化する。なお無関係な装飾系の 404(`media/canvas.png`、`media/retroarch.ico`)と CORS エラー(`rawgit.com` の jquery.hotkeys)は残るが、コアの動作には影響しない。

---

## 動作確認した環境

| 項目 | 値 |
|---|---|
| Emscripten | 5.0.7 |
| ブラウザ | Chrome(別ホストから HTTP 経由で接続) |
| RetroArch Web | 標準ビルド + 上記 BrowserFS パッチ |
| 接続元 | xrdp **以外** のホストの Chrome |

xrdp 経由の Chrome では WebGL が利用できず、`Run` ボタンを押した後の RetroArch の GL コンテキスト初期化で停止する(本問題は piece-emu には無関係。xrdp の制約)。

---

## 今後の作業

- BrowserFS パッチを RetroArch Web の配布物に常駐させる(upstream への投げ込み or フォーク管理)
- `.pfi` ロードの UX 整備(現状は RetroArch の Add Content フローを通る)
- `mkpfi` / `pfar` の Web フロントエンド化(将来)

メモリ上の関連エントリ: `project_web_port_libretro.md`(Index は [MEMORY.md](../../../.claude/projects/-home-autch-src-llvm-c33-piece-emu/memory/MEMORY.md))。
