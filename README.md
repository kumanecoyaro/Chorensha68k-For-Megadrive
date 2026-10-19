# Chorensha68k-For-Megadrive

X68000 用シューティング「超連射68k」(ver1.10) のメガドライブ移植プロジェクト。

現在: **ステージ1 を最初からボス撃破・ステージクリアまで通しでプレイ可能**
（クリア後はステージ1 を繰り返す。ステージ2 以降の BG は未変換）。

* 解析メモ: [docs/ANALYSIS.md](docs/ANALYSIS.md)
* ビルド済み ROM: `out/chorensha68k_md.bin`（1.9 MB）

## 方式: 原作の 68000 コードをそのまま動かす

X68000 もメガドライブも CPU は 68000 なので、ゲームロジック（ステージ進行、全敵、自機、ボム、
当たり判定、得点、残機…）は **SZ2.X の機械語をそのまま実行**する。手で移植し直すのではなく、
`tools/relocate.py` がアドレスだけを付け替える。

* .X のリロケーション表に全絶対アドレスが載っているので、それを書き換えて
  text / data を ROM（$10000〜）へ、ゲームが使う BSS（オブジェクトプールは 383 → 128 個に縮小）と
  書き込まれる数少ない data 変数を RAM（$FF0000〜、約 39 KB）へ置き直す。
* 自分の text 領域に状態を持つライブラリ（当たり判定グリッド、乱数インデックス）は、
  PC 相対アクセスのままで動くようルーチンごと RAM にコピーする。
* X68000 固有のライブラリ呼び出しは関数の先頭を `jmp フックテーブル` に書き換え、
  MD 側の実装（`src/hooks.s` → C / アセンブラ）へ飛ばす:

| 原作 | MD 版 |
|---|---|
| XSP（スプライト: xsp_set / xobj_set） | 全 PCG・全 ref を MD スプライトブロックに変換済み（`tools/convert_sprites.py`）。VRAM はタイルキャッシュ（LRU）、X68000 の 16 パレットは MD の 2 パレットに減色マップ。XSP と同じく呼び出しを優先度別に記録し、手前から 64 枚まで展開（`src/xsp_fast.s`） |
| グラフィック VRAM への BG チップ描画 | 原作のスクロール処理はそのまま。チップ描画を Plane A/B への書き込みに置換（VBlank で実行） |
| テキスト VRAM | スコア行はウィンドウ面、他の行（残機・ボム表示、メッセージ）はスプライト。フォントは原作の SZ_TEXT.pcg |
| IOCS ジョイスティック | MD パッド（A ショット / B ボム / C 連射 / START ポーズ） |
| MCDRV / ADPCM | MDX ドライバ（`src/music.c`）+ Z80 PCM ドライバ |
| リプレイ記録、ファイル読み込み、ネームエントリ | 省略 |

## ROM 構成

すべて変換済み・展開済みで ROM に置く（MD の RAM は 64 KB）。`make` の最後に `tools/rom_budget.py`
がサイズを表示する。現在 1.9 MB、全ステージ分の BG を加えた見積もりで約 2.3 MB（4 MB の 57%）。

| 項目 | サイズ |
|---|---|
| 原作プログラム（SZ2.X text + data、再配置済み） | 296 KB |
| BG マップ（map.bg0 / bg1） | 16 KB |
| スプライトタイル（全 PCG / ref、4bpp） | 555 KB |
| スプライト表（ピース・ブロック・パレット変換表・フォント） | 184 KB |
| BG タイル + チップ表（ステージ1） | 29 KB |
| 効果音 PCM（8bit 15.625 kHz、全 62 番） | 614 KB |
| BGM（MDX 13 曲 + 音色） | 72 KB |

* 効果音: MSM6258 ADPCM → 8bit 符号なし PCM（YM2612 DAC 用）、ピーク正規化。Z80 がバンク切替しながら
  DAC へ流し、同時に 68000 からの YM2612 書き込みキューを処理する。
* BGM: MDX をそのまま格納し、両チップ共通の音色パラメータだけで YM2612 で演奏。FM 6 音に収めるため
  曲ごとに 2ch を統合（重ねているパート優先）。FM6 は効果音再生中だけ DAC に切り替わるので、
  音程の動きが一番少ないパートを FM6 に置く。

## 操作

| ボタン | 動作 |
|---|---|
| 方向 | 移動 |
| A | ショット（原作通り押すたびに連射。連打してください） |
| B | ボム |
| C | オート連射（MD 版追加） |
| START | ポーズ |

## ビルド

```
sudo apt-get install gcc-m68k-linux-gnu binutils-m68k-linux-gnu python3-pil
pip3 install capstone            # 逆アセンブル（ビルドに必要）
make XDF=path/to/CH68_110_1.xdf
```

## ツール

| ファイル | 役割 |
|---|---|
| `tools/xdf_extract.py` | XDF (Human68k FAT12) からファイル抽出 |
| `tools/xfile.py` | .X 実行形式ローダ（リロケーション表の解析） |
| `tools/dis68.py`, `tools/cg.py` | 逆アセンブラ / コールグラフ表示 |
| `tools/relocate.py` | 原作プログラムの再配置・フック・RAM 配置 |
| `tools/convert_sprites.py` | 全 XSP スプライト・パレット・テキストフォントの変換 |
| `tools/convert_assets.py` | BG タイル・チップ・BG パレットの変換 |
| `tools/x68gfx.py` | PCG・パレット・チップ・マップの読み込み |
| `tools/convert_pcm.py` | ADPCM → MD DAC 用 PCM 変換 |
| `tools/z80_pcm_driver.py` | Z80 ドライバ生成（DAC 再生 + YM2612 書き込みキュー） |
| `tools/mdx.py`, `tools/convert_bgm.py` | MDX 解析 / チャンネル割り当て・音程表の生成 |
| `tools/rom_budget.py` | ROM サイズ集計 |
| `tools/build_variant.sh` | デバッグ用ビルド（`-DTEST_MUTEKI` 無敵, `-DFRAME_PROBE` 処理時間計測, `-DZ80_PEEK` ほか） |
| `tools/fixrom.py` | ROM ヘッダとチェックサムの設定 |
| `tools/mdrun.py` | libretro コア（Genesis Plus GX）でのヘッドレス実行・スクリーンショット |
| `tools/uctrace.py` | unicorn の 68000 で ROM を実行（ROM 書き込み検出・プロファイル・命令数計測） |

## 未対応 / 既知の問題

* ステージ2 以降（BG のタイル・パレット未変換。現在はステージ1 をループ）
* タイトル画面・オプション・ネームエントリ（起動するとすぐステージ1）
* パレットのフェード（BG パレットは固定）
* スプライト色は X68000 の 16 パレット → MD 2 パレットへの減色
* MD のスプライト上限（64 枚 / ライン 16 枚）を超えると奥のものから消える
* 処理速度: 通常時で原作（55.5 Hz）の 9 割強。弾や破片が多い場面では処理落ちする
