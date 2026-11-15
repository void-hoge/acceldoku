# FADES: FPGA-Accelerated Deductive and Explorative Sudoku Solver

KV260 向けの SystemVerilog 数独ソルバー。Singles / Locked Candidates を並列適用し、停滞時は候補を二分して深さ優先探索する。
PS の Linux プログラムから DDR / AXI DMA 経由で問題を投入し、ID 付きの結果を回収する。

既定の実機構成は **6 コア × 約115.385 MHz、DMA 187.5 MHz**。コア数は1〜6で指定できる。
最初の解を返し、解なしも結果を 1 件返す。問題の一意性を証明する処理ではない。

入力の候補マスク変換・確定数字の先行伝播は、全コア共有の2段パイプラインで行う。

ビルド・検証の基本操作は **configure → build → test**。
以下のコマンドは、対象の開発機またはKV260上のリポジトリルートで実行する。

## 1. ホスト側

Linux カーネルドライバー、C ioctl ABI、C++20 ライブラリーと実機試験は `host/` にある。
現行 ABI は **v8**。TX/RX バッファー数を独立に指定し、1面 1 MiB の領域を個別に mmap する。
`fades::StreamDevice` はバッファー番号を借用し、明示的に投入・返却する C++ API。
入出力形式、API、所有権と使用例は [host/README.md](host/README.md) を参照。

| 対象 | ソース | 用途 |
| --- | --- | --- |
| C ioctl ABI / ドライバー | `host/driver/include/fades_ioctl.h`、`host/driver/src/fades.c` | `/dev/fades` の DMA・バッファー・制御 |
| C++ ライブラリー | `host/lib/include/fades/stream_device.hpp`、`host/lib/src/stream_device.cpp` | レコード変換とストリーミング操作 |
| C / C++ ベンチマーク | `host/tools/fades_run.c`、`host/tools/cpp/benchmark.cpp` | 入力を反復投入し、結果を件数のみ集計 |

C++ ベンチマークは 1 行 81 文字の問題テキストを入力する。デバイスの準備後、KV260 で実行する。

```sh
build/kv260-ps/host/tools/cpp/fades_benchmark sim/data/17puz49158.txt 8 64 10
```

## 2. 実機を準備する

開発機で回路を生成し、KV260 上で PS プログラムとドライバーをビルドしてロードする。

```sh
# 開発機: CMake 3.25以降、Ninja、Vivado 2024.2 + K26サポート、dtc
cmake --preset kv260-pl -DFADES_CORES=6 -DFADES_VIVADO_JOBS=2
cmake --build --preset kv260-pl
```

通常のbuildで、回路生成・出力検査からfirmwareパッケージ生成まで実行する。
合成・配置配線にはcore 8.666 ns / DMA 5.333 nsの制約を適用し、
配線後の `AggressiveExplore` 物理最適化と最終タイミング検査も自動実行する。
Vivadoのパスを指定する場合は、configure時に
`-DVIVADO_EXECUTABLE=/tools/Xilinx/Vivado/2024.2/bin/vivado` を追加する。
`FADES_VIVADO_JOBS` はVivado内部の並列数。

| ターゲット        | 処理                                             |
|-------------------|--------------------------------------------------|
| `kv260-bd`        | BD生成・検証                                     |
| `kv260-synth`     | 合成まで                                         |
| `kv260-bitstream` | 配置配線・bitstream生成・タイミング／CDC等の検査 |
| `kv260-package`   | firmwareパッケージまで                           |

生成物は `build/kv260-pl/fpga/kv260/firmware/fades/`。
Vivadoプロジェクトと検査レポートは `build/kv260-pl/fpga/kv260/implementation/` に生成する。
ソースとfirmwareはユーザーが任意の方法で実機へコピーする。
ビルド処理は接続先や実機上の配置先を仮定せず、自動転送もしない。

KV260上で必要なパッケージを入れ、ソースを配置したリポジトリルートでビルドする。

```sh
sudo apt install build-essential cmake ninja-build python3 "linux-headers-$(uname -r)"
cmake --preset kv260-ps
cmake --build --preset kv260-ps
```

通常のbuildで `build/kv260-ps/host/driver/src/fades.ko`、Cツール
`build/kv260-ps/host/tools/fades_run`、C++ライブラリーと
`build/kv260-ps/host/tools/cpp/fades_benchmark` を生成する。
実機自身のコンパイラーと実行中カーネルのヘッダーを使う。
firmwareを配置し、`xmutil loadapp fades` と `insmod fades.ko` を実行すると **`/dev/fades`** が使える。
転送・Arm64 ビルド・ロードの手順と、各ボード用ファイルの役割は
[fpga/kv260/README.md](fpga/kv260/README.md) にまとめる。

## 3. 開発・検証する

CPU参照に fastdoku を使用するため、Rust / Cargo と submodule の取得が必要。

```sh
git submodule update --init --recursive
cmake --preset host -DFADES_CORES=6
cmake --build --preset host
ctest --preset host                         # CPU参照・RTLの全試験
ctest --preset host -L '^fades_core$'        # コア単体：全問題集・ストレス・構造合成・lint
ctest --preset host -L '^fades$'             # 指定コア数で供給・回収を含む統合試験・lint
ctest --preset host -L '^deduction_engine$'  # 推論の有向試験・形式検証
ctest --preset host -L '^async_queue$'       # 実装に使うXPM非同期FIFOの試験
```

ラベルは検証対象の `sim/` ディレクトリ名。対象を選ぶと関連する検証をまとめて実行する。
`-L` は正規表現なので、対象名の完全一致には `^...$` を付ける。
コア・統合試験は全49,158問を使い、単一コア試験には追加4,096件のストレス入力がある。
`-N` で試験一覧を表示し、個別の試験は `-R` でも選択できる。

```sh
ctest --preset host -N
ctest --preset host -R '^axi$'
ctest --preset host -L '^branch_generator$'
ctest --preset host -L '^reference$'
```

XPM試験は `xvlog` が見つかった場合に登録する。パスを明示する場合:

```sh
cmake --preset host -DXVLOG_EXECUTABLE=/tools/Xilinx/Vivado/2024.2/bin/xvlog
cmake --build --preset host
ctest --preset host -L '^async_queue$'
```

試験の詳細と回帰比較は [sim/README.md](sim/README.md)。必要ツールは
CMake 3.25以降、Ninja、C++20、Python 3、Icarus Verilog、Verilator、Yosys。
XPM試験にはVivadoが必要。`kv260-ps` ではRTL検証ツール・Vivado・カーネルのクロスビルド環境は不要。

### ビルド対象と出力の分離

| プリセット | 実行場所 | 通常のbuildで生成するもの | 出力先 |
|------------|----------|--------------------------|--------|
| `host` | 開発機 | CPU参照・RTL試験、C++ライブラリー・ベンチマーク | `build/host/` |
| `kv260-pl` | 開発機 | FPGA回路・検査レポート・firmware、C++ライブラリー・ベンチマーク | `build/kv260-pl/` |
| `kv260-ps` | KV260 | カーネルドライバー、Cツール、C++ライブラリー・ベンチマーク | `build/kv260-ps/` |

各プリセットは `cmake --build --preset <名前>` で必要なものをまとめてビルドする。
CPU参照・RTL試験は `ctest --preset host`、実機試験は `host/tests/` のPythonを使う。
C++ライブラリーとベンチマークは既定で有効。不要な場合はconfigure時に
`-DFADES_BUILD_CPP=OFF` を指定する。RTL・カーネルモジュールをビルドせずC++のみを
ビルドする場合は `cmake -S . -B build/cpp` と `cmake --build build/cpp` を使う。

コア数はプリセットとは独立した `-DFADES_CORES=1`〜`6`（既定6）。
ボードトップからコア群まで伝播し、シミュレーションにも同じ値を使う。
ホストABIの6コア分のカウンターレイアウトは固定で、未実装コア分は0になる。
構成変更時は同じビルドディレクトリで再configureしてからビルドする。

```sh
cmake --preset host -DFADES_CORES=1
cmake --build --preset host
ctest --preset host -L '^fades$'
```

複数構成を保持するときは `-B` で出力先を分ける:

```sh
cmake --preset host -B build/host-1core -DFADES_CORES=1
cmake --build build/host-1core
ctest --test-dir build/host-1core --output-on-failure -L '^fades$'
```

**`-B` で出力先を変えた場合は、build・testでもそのディレクトリを指定する。**

ビルドの並列数を制限したい場合は `--parallel 4` などを追加できる。
これはビルドジョブ数で、FPGAのコア数とは独立。省略時はNinjaの既定の並列度を使う。

ツールのパスなどのローカル設定は `CMakeUserPresets.json` に記述できる（Git管理外）。
一覧は `cmake --list-presets`、試験一覧は `ctest --preset host -N`。

## ソースを読む順序

| 対象 | 読むファイル | 役割 |
| --- | --- | --- |
| ホスト利用 | `host/README.md` | 転送形式、C/C++ API、ベンチマーク・実機試験 |
| Linux接続 | `host/driver/include/fades_ioctl.h` → `host/driver/src/fades.c` | 公開ABI → バッファ・DMA・制御 |
| C++接続 | `host/lib/include/fades/stream_device.hpp` → `host/lib/src/stream_device.cpp` | 公開API → ioctl / mmap / poll |
| ボード生成 | `fpga/kv260/scripts/build.tcl` | PS・DMA・クロックの接続、実装、出力検査 |
| PLの入口 | `rtl/fades_axi.sv` → `fades.sv` | 制御 → 供給・ID管理・結果回収・コア群 |
| 探索 | `rtl/fades_core.sv` → `deduction_engine.sv` / `branch_generator.sv` | 状態列・探索 → 推論 / 分岐 |
| クロック境界 | `rtl/async_queue.sv` | XPM非同期FIFOと試験用モデル |
| CPU照合 | `reference/sudoku.hpp`、`reference/fastdoku/` | RTL試験用の fastdoku 参照ソルバーと形式変換 |

`build/` はビルド生成物。

## 実測

KV260 / Linux `6.8.0-1036-xilinx`、ABI v8、TX 8面 / RX 64面で、
C++ベンチマークによる17ヒント49,158問の反復投入を5秒ずつ3回測定した。
結果は **27.6092 / 27.6097 / 27.6097 Mproblem/s**。各回の処理数は138,166,272件。

入力変換・初期TX投入は計測前に行い、計測には `STREAMON`、継続コピー・一意ID付与、
ioctl、DMA・求解、RX返却と最終排出を含む。**出力の内容は読まず、件数だけ集計**した。
実機の PS クロック読み出しは pl0 187,499,999 Hz、pl1 115,384,615 Hz。
ロードした `fades.bit.bin` の SHA-256 は
`1bc89fe1f9f3083061d277276cc28b6e7ceb0c5a59a733e2319df5677570fba8`。
ビットストリーム内部のクロック構成はこの測定では別途確認していない。
入力問題の出所は [sim/README.md](sim/README.md#問題集)。
