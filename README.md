# Acceldoku: FPGA による推論・探索型数独ソルバー

## 概要

Acceldoku は、推論規則の並列適用と深さ優先探索を組み合わせた、高スループットの数独ソルバーアクセラレーターである。
大量の問題を連続処理するよう最適化されており、KV260（K26 SOM）上で 17 ヒント問題集を **27.623M solves/s**、`HardestDatabase110626` を **0.814M solves/s** で解く。
これは Ryzen 7 PRO 6850U 上で 16 スレッドを使用した fastdoku のそれぞれ **5.24 倍、3.65 倍**である。

## 結果

以下は、KV260 上の `acceldoku_benchmark` で問題集を繰り返し投入した結果と、Ryzen 7 PRO 6850U 開発機上の [fastdoku](https://github.com/rocketbombs/fastdoku/blob/main/README.md) の結果を比較したものである。
単位は **M solves/s（毎秒百万問）**である。

| 問題集                            |   問題数 | KV260 FPGA | fastdoku, 1 スレッド | fastdoku, 16 スレッド |
|-----------------------------------|----------:|-----------:|-------------------:|---------------------:|
| Kaggle 1 million Sudoku games     | 1,000,000 |     31.225 |              2.087 |               14.759 |
| `puzzles1_unbiased`               | 1,000,000 |     30.128 |              0.754 |                5.504 |
| `puzzles2_17_clue`                |    49,158 |     27.623 |              0.713 |                5.270 |
| `puzzles3_magictour_top1465`      |     1,465 |      5.512 |              0.200 |                1.428 |
| `puzzles4_forum_hardest_1905`     | 2,135,371 |      1.067 |             0.0551 |                0.397 |
| `puzzles5_forum_hardest_1905_11+` |    48,766 |      0.936 |             0.0480 |                0.341 |
| `puzzles6_forum_hardest_1106`     |       375 |      0.814 |             0.0319 |                0.223 |
| `puzzles7_serg_benchmark`         |    10,000 |     17.193 |              0.814 |                5.881 |

測定条件：

- **FPGA:**
  - KV260、6 コア構成
  - コアクロック：115.385 MHz、DMA クロック：187.5 MHz
  - この周波数に必要な PS クロック設定については、[KV260 の手順](fpga/kv260/README.md#ps-クロックとブートファームウェア)を参照
  - 入力バッファー 8 個、出力バッファー 64 個を使用し、問題集を 60 秒間繰り返し投入した 3 回の測定の中央値
  - 出力内容は読み取らず、結果の件数のみを計数
- **CPU:**
  - Ryzen 7 PRO 6850U
  - fastdoku の `auto` をネイティブ最適化付きでビルド
  - 1 秒間のウォームアップ後、最初の解を 60 秒間繰り返し求めた 3 回の測定の中央値
  - スレッドは計測前に作成し、解は破棄して件数のみを記録

## アーキテクチャ


| ![Acceldoku アクセラレーターのブロック図](images/block_diagram.svg) |
|:----------------------------------------------------------------:|
| アクセラレーターのブロック図                                     |

| ![ソルバーコアのブロック図](images/core_block_diagram.svg) |
|:-----------------------------------------------------------:|
| 個々のソルバーコアのブロック図                              |

各コアは、セルごとに 9 ビットのフラグを持つ 729 ビットの候補行列レジスターを更新して問題を解く。
推論エンジンは毎サイクル、盤面全体に Singles と Locked Candidates の規則を並列に適用する。
これらの規則を候補行列に繰り返し適用することで、連鎖的に推論を進める。

新しい問題は、キューとして使う BRAM ベースの両端キューを通じて推論エンジンに供給される。
推論が停滞すると、未確定のセルを選択し、その候補を 2 組に分割する。
一方の組で直ちに推論を続け、もう一方を両端キューの先頭に追加する。

矛盾が生じた場合は現在の候補行列を破棄し、両端キューの先頭から別の状態を取り出す。
解が見つかった場合は、両端キューに残る分岐を破棄する。

この両端キューは、入力問題のキューとバックトラック用のスタックを兼ねる。

## ビルドと使い方

### 必要環境

- 開発機
  - 合成・配置配線：
    - CMake 3.25 以降
    - Ninja
    - Vivado 2026.1 以降（K26 デバイスサポートと `dtc` を含む）
  - 検証：
    - `build-essential`
    - Rust / Cargo 1.83 以降
    - Python 3
    - Icarus Verilog
    - Verilator
    - Yosys
    - Vivado 2026.1
- KV260
  - Ubuntu 24.04
  - `build-essential`
  - CMake 3.25 以降
  - Ninja
  - Python 3
  - Rust / Cargo 1.83 以降
  
`apt` で提供される `rustc` と `cargo` は古い場合がある。
最新版のインストールには `rustup` を使用する（`apt` でインストール可能）。

```sh
apt install rustup
rustup default stable
```

### FPGA ファームウェアの生成

開発機で設計の合成・配置配線を行い、ビットストリームと Device Tree overlay を生成する。
出力先は `build/kv260-pl/fpga/kv260/firmware/acceldoku/` である。

```sh
cmake --preset kv260-pl
cmake --build --preset kv260-pl
```

プリセットの設定時に以下のオプションを指定できる。

- `-DACCELDOKU_CORES`：ハードウェアに実装するソルバーコア数（1〜6、既定値：6）
- `-DACCELDOKU_VIVADO_JOBS`：Vivado が使用するコア数

### KV260 でのビルドとロード

ソースと生成したファームウェアディレクトリを KV260 に転送する。
`git clone --recursive https://github.com/void-hoge/acceldoku.git` を実行すると、サブモジュールもクローンできる。

実機の Ubuntu に必要なパッケージをインストールする。Rust / Cargo 1.83 以降は別途インストールして `PATH` に追加する。

```sh
sudo apt install build-essential cmake ninja-build python3 "linux-headers-$(uname -r)"
```

ドライバーとツールをビルドする。

```sh
cmake --preset kv260-ps
cmake --build --preset kv260-ps
```

`build/kv260-ps/host/driver/src/acceldoku.ko`、C/C++ ツール、C++ ライブラリが生成される。
モジュールは実行中のカーネルのヘッダーを使用してビルドされる。

実機の root シェルで、転送したファームウェアのパスを指定してロードする。
先にデバイスを使用中のプログラムとメモリーマッピングをすべて閉じる。

```sh
FIRMWARE_DIR=/path/to/copied/firmware/acceldoku
mkdir -p /lib/firmware/xilinx/acceldoku
cp "$FIRMWARE_DIR"/* /lib/firmware/xilinx/acceldoku/
if test -d /sys/module/acceldoku; then rmmod acceldoku; fi
xmutil unloadapp
xmutil loadapp acceldoku
xmutil listapps
insmod build/kv260-ps/host/driver/src/acceldoku.ko
ls -l /dev/acceldoku
```

### 実行と検証

`/dev/acceldoku` が使用可能になったら、ベンチマークを実行する。入力は 1 行に 1 問、1 問につき 81 セルで、空きセルは `.` または `0` で表す。

```sh
# 引数：問題集、入力バッファー数、出力バッファー数、測定時間（秒）
build/kv260-ps/host/tools/cpp/acceldoku_benchmark sim/data/17puz49158.txt 8 64 10
# 引数：問題集
build/kv260-ps/host/tools/cpp/acceldoku_verify sim/data/17puz49158.txt
```

ベンチマークは指定時間中、問題集を繰り返し投入して出力結果を計数する。
`acceldoku_verify` は各問題を一度ずつ投入し、出力のチェックサムを fastdoku と比較する。

### テスト

開発機で CPU リファレンスと RTL のテストを実行するには、サブモジュールを初期化して次を実行する。

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

KV260 では `kv260-ps` プリセットでビルドし、`/dev/acceldoku` を使用可能にして、デバイスを使用する他のプログラムを閉じる。
リポジトリのルートからホストテストを実行する。

```sh
cmake --preset kv260-ps
cmake --build --preset kv260-ps
ctest --preset kv260-ps
```

これらのテストは DMA リングの動作、ドライバーの所有権とマッピングの寿命、停止通知、C++ API、テキスト入力の変換を検証する。デバイスを使用するテストは直列に実行される。テキスト入力のテストは CPU リファレンスおよび RTL のテストとともに GitHub Actions でも実行される。詳細は [host/README.md](host/README.md) と [sim/README.md](sim/README.md) を参照する。

## ライセンス

- MIT

### 問題集

| 問題集                                                                     | 問題数 | 元の出典・収集元                                                                                                                                                                                                                                 |
|----------------------------------------------------------------------------|--------:|----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| [`sim/data/17puz49158.txt`](sim/data/17puz49158.txt)                       |  49,158 | Gordon Royle の 17 ヒント問題集を基にした [dobrichev のコレクション](https://sites.google.com/site/dobrichev/sudoku-puzzle-collections)と[追加の問題](http://forum.enjoysudoku.com/scan-solution-grids-for-17-clues-as-of-blue-t34012-90.html#p282968)。 |
| [`sim/data/HardestDatabase110626.txt`](sim/data/HardestDatabase110626.txt) |     375 | Enjoy Sudoku フォーラムの [HardestDatabase110626.txt](http://forum.enjoysudoku.com/the-hardest-sudokus-new-thread-t6539.html#p65791)。 |
