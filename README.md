# Chorensha68k-For-Megadrive

X68000 用シューティング「超連射68k」(ver1.10) のメガドライブ移植プロジェクト。

現在はテスト版: ステージ1序盤の **2 重スクロール BG・自機・超連射ショット・敵 2 種
（ザコ $190 / 中型機 $1F0）と敵弾 3 種** を動かせます。

* 解析メモ: [docs/ANALYSIS.md](docs/ANALYSIS.md)
* ビルド済み ROM: `out/chorensha68k_md.bin`（768 KB）

## ROM 構成

X68000 版は起動時に `assets.tgz` を展開し、ADPCM を MCDRV のバッファへ読み込んで使うが、
メガドライブの RAM は 64 KB しかないため、すべて変換済み・展開済みの形で ROM に置く。

* 効果音: SZ2_PCM.CNF の全 62 番（35 サンプル）を MSM6258 ADPCM → 8bit 符号なし PCM
  (15.625 kHz, YM2612 DAC にそのまま書ける形式) に変換。10.4 kHz 指定の番号は伸長済み。
  Z80 ドライバが ROM をバンク切替しながら DAC へ流す（`tools/z80_pcm_driver.py`）。
* グラフィック / マップ: MD のタイル・ネームテーブル・スプライト形式に変換済み。
* `make` の最後に `tools/rom_budget.py` が現在のサイズと、全素材を載せた場合の見積もりを表示する。
  見積もり合計は約 1.8 MB で 4 MB 以内に収まる（下表）。

| 項目 | サイズ |
|---|---|
| PCM 全数（8bit 15.625 kHz） | 614 KB（実測） |
| スプライト PCG 全数（パレット違い 2 組分） | 486 KB |
| スプライト構成表（.ref/.frm 全数） | 134 KB |
| BG タイル + チップ表（map / end） | 69 KB |
| BG マップ | 16 KB |
| BGM（MDX を YM2612 用に変換、3 倍で見積もり） | 206 KB |
| ゲームコード（SZ2.X の text 相当） | 240 KB |
| データ表 | 88 KB |
| **合計** | **約 1854 KB（4 MB の 45%）** |

## 操作

| ボタン | 動作 |
|---|---|
| 方向 | 移動 |
| A / B | ショット（原作通り押すたびに 12 フレームの連射。連打してください） |
| C | オート連射（MD 版追加） |

## ビルド

```
sudo apt-get install gcc-m68k-linux-gnu binutils-m68k-linux-gnu python3-pil
pip3 install capstone            # 逆アセンブル (make disasm) を使う場合のみ
make XDF=path/to/CH68_110_1.xdf
```

ディスクイメージから `SZ2.X` と `assets.tgz` を取り出し、グラフィック・マップ・テーブルを
変換して `out/chorensha68k_md.bin` を生成します。

## ツール

| ファイル | 役割 |
|---|---|
| `tools/xdf_extract.py` | XDF (Human68k FAT12) からファイル抽出 |
| `tools/xfile.py` | .X 実行形式ローダ（リロケーション表の解析） |
| `tools/dis68.py`, `tools/cg.py` | 逆アセンブラ / コールグラフ表示 |
| `tools/x68gfx.py` | PCG・パレット・チップ・マップの読み込み |
| `tools/convert_assets.py` | MD 用データ (C ソース) 生成 |
| `tools/convert_pcm.py` | ADPCM → MD DAC 用 PCM 変換 |
| `tools/z80_pcm_driver.py` | Z80 PCM ドライバ生成 |
| `tools/rom_budget.py` | ROM サイズ集計と全素材搭載時の見積もり |
| `tools/fixrom.py` | ROM ヘッダとチェックサムの設定 |
| `tools/mdrun.py` | libretro コア（Genesis Plus GX）でのヘッドレス実行・スクリーンショット |

## 未対応

BGM、ボム、パワーアップアイテム、自機の残機/ゲームオーバー、ステージ1 の型 $208 以降の敵、
ステージ2以降、タイトル等。
