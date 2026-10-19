# Chorensha68k-For-Megadrive

X68000 用シューティング「超連射68k」(ver1.10) のメガドライブ移植プロジェクト。

現在はテスト版: ステージ1序盤の **2 重スクロール BG・自機・超連射ショット・敵 2 種
（ザコ $190 / 中型機 $1F0）と敵弾 3 種** を動かせます。

* 解析メモ: [docs/ANALYSIS.md](docs/ANALYSIS.md)
* ビルド済み ROM: `out/chorensha68k_md.bin`

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
| `tools/fixrom.py` | ROM ヘッダとチェックサムの設定 |
| `tools/mdrun.py` | libretro コア（Genesis Plus GX）でのヘッドレス実行・スクリーンショット |

## 未対応

サウンド、ボム、パワーアップアイテム、自機の残機/ゲームオーバー、ステージ1 の型 $208 以降の敵、
ステージ2以降、タイトル等。
