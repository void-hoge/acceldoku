# RTL・参照実装の検証

テストは目的別のディレクトリにまとめ、各 `CMakeLists.txt` がソース・生成物・
実行条件を定義する。他の試験のコードや生成物には依存しない。
CPU照合には `reference/sudoku.hpp` の変換・検証を通して fastdoku を使う。
任意の候補マスクによる矛盾や探索は、コア・推論エンジンの有向試験で検証する。

## 実行

必要ツール: CMake 3.25以降、Ninja、C++20、Rust / Cargo 1.83以降、Python 3、Icarus Verilog、Verilator、Yosys。
実XPMの試験にはVivado 2026.1のxsimを使う。
初回は `git submodule update --init --recursive` で fastdoku を取得する。
Cargo はシミュレーションをビルドするときに C ABI ラッパーを静的ライブラリーとしてビルドする。
ARM64 では fastdoku の NEON バックエンドが有効になる。

```sh
cmake --preset dev -DACCELDOKU_CORES=6
cmake --build --preset dev
ctest --preset dev
ctest --preset dev -L '^acceldoku_core$'
ctest --preset dev -L '^acceldoku$'
ctest --preset dev -L '^async_queue$'
```

`dev` はCPU参照とRTLシミュレーションをビルドする。
ラベルは `sim/` のディレクトリ名と同じで、検証対象を指定すると、その対象の試験をまとめて実行する。
`-L` は正規表現なので `^...$` で完全一致を指定する。
ラベルを省略するとCPU参照試験も含めた登録済みの全試験を実行する。
`ctest --preset dev -N` で一覧を表示し、`-R` で個別の試験名を選ぶこともできる。
1コア構成は `-DACCELDOKU_CORES=1`。
コア数を同じ出力先で変更した場合も、再configure・buildで生成コードを更新する。

`xvlog` がPATH上または `$XILINX_VIVADO/bin` にない場合、XPM試験は登録しない。
`-DXVLOG_EXECUTABLE=/tools/Xilinx/2026.1/Vivado/bin/xvlog` で明示指定できる。
問題集は `-DACCELDOKU_DATASET=/absolute/path/to/dataset.txt` で変更可能。

GitHub Actions の Host tests は push、pull request、手動実行時に Ubuntu 上で
6コア構成の `dev` プリセットをビルドし、登録された全試験を実行する。
Vivado がないため XPM 試験は登録されない。KV260 実機を使う
テキスト入力変換の試験も実行する。実機を使う `host/tests/` の試験は [host/README.md](../host/README.md) の手順で実行する。

## 試験一覧

| ディレクトリ／ラベル | 試験名 | 検証対象・目的 |
|----------------------|--------|----------------|
| `deduction_engine` | `deduction-engine`、`formal-deduction-engine`、`formal-row-changes`、`formal-locked-masks` | Singles、Locked Candidates、推論ステップ、推論・行変化・共通マスクと独立仕様の等価性 |
| `branch_generator` | `branch-generator`、`formal-branch-generator` | 候補分割の有向試験・独立逐次仕様との等価性 |
| `acceldoku_core` | `acceldoku-core`、`synth-core`、`lint-acceldoku_core` | コア1個のCPU照合・探索・バックトラック・順序・停止・キュー周回・reset・サイクル集計、構造合成、lint |
| `acceldoku` | `acceldoku`、`lint-acceldoku` | `ACCELDOKU_CORES` 個のコア群と供給・ID管理・回収・非同期キューの統合試験、lint |
| `acceldoku_axi` | `axi`、`lint-acceldoku_axi`、`lint-acceldoku_kv260` | AXI-LiteのAW/W独立受理・WSTRB・応答停止・設定保護・snapshot・未実装コアのカウンター0、AXI境界とボードラッパーのlint |
| `async_queue` | `async-queue-xpm` | Xilinx提供の非同期FIFOモデルでfull/count・周回・停止・resetを検証（Vivado xsim） |
| `reference` | `reference-dataset` | CPU参照実装・入力形式の基本試験と、全問題集の解の存在・一意性 |

コア・統合・CPU参照試験は既定の全49,158問を使い、コア試験には追加4,096件のストレス入力がある。
`acceldoku-core` のDUTは常に単一の `acceldoku_core`。`acceldoku` のDUTは既定6コアの `acceldoku` 全体で、
`ACCELDOKU_CORES` でコア数を変更できる。AXI-Lite制御やPS・DMAを含むボード全体は含まない。
統合試験は解なし・不正セル・満杯・出力停止・部分入力と未回収出力のresetも検証する。
Verilator用キューは64エントリの抽象モデル。XPMのFWFT段・初期化レイテンシは
cycle単位では再現しないため、実キューの性質をxsimでも確認する。

`lint`、`synth`、`formal`、`fifo-test` のビルドターゲットから該当検査を実行することもできる。
ホスト試験は [host/README.md](../host/README.md) を参照。

## 出力・サイクル回帰

生成物は選択したビルドディレクトリの `sim/<試験ディレクトリ>/` に置く。
各試験の作業ディレクトリも独立している。

コア試験は `sim/acceldoku_core/` に `results.csv` と
`results.csv.totals.json` を生成する。CSVは問題ごとの完了間隔、totalsは最後の出力排出を含む全体値。

```sh
ctest --preset dev -L '^acceldoku_core$'
cmake --build --preset dev --target core-summary

cmake --preset dev -DACCELDOKU_BASELINE=/absolute/path/to/saved/results.csv
cmake --build --preset dev --target regression
```

`core-summary` はコア試験のCSVから `summary.json` を生成する。
`regression` はベースラインの指定時に有効になり、49,158問+4,096件のCSVとtotalsの
完全一致を確認する。ベースラインは両ファイルを保存すること。

単一コアのHWはボード実装を `ACCELDOKU_CORES=1` で生成する。
詳細は [KV260ビルド手順](../fpga/kv260/README.md)。

## 問題集

次の2ファイルはBinarySudokuの原本で、ファイル名・内容・CRLFを保持する。
1行81文字、空欄 `.`、ヒント `1`〜`9`、行優先。

| ファイル                    | 問題数 | 用途                               |
|-----------------------------|-------:|------------------------------------|
| `17puz49158.txt`            | 49,158 | 17ヒント集合、試験と測定の既定入力 |
| `HardestDatabase110626.txt` |    375 | 探索負荷の大きい難問集合           |

出所: [BinarySudoku problems/9x9](https://github.com/void-hoge/binarysudoku/tree/a494907924083cbfc9e14a74c75f614ead752c28/problems/9x9)
（commit `a494907924083cbfc9e14a74c75f614ead752c28`）。`.gitattributes` で原本の改行を維持する。

### ベンチマーク用問題集

fastdoku の README の性能表に掲載された問題集を、上流のファイル名に `.txt` を付けて `data/` に置く。
コメント行と空行を含むファイルがあり、ホストツールはこれらを読み飛ばす。

| ファイル | 問題数 |
|---|---:|
| `puzzles0_kaggle.txt` | 100,000 |
| `puzzles1_unbiased.txt` | 1,000,000 |
| `puzzles2_17_clue.txt` | 49,158 |
| `puzzles3_magictour_top1465.txt` | 1,465 |
| `puzzles4_forum_hardest_1905.txt` | 2,135,371 |
| `puzzles5_forum_hardest_1905_11+.txt` | 48,766 |
| `puzzles6_forum_hardest_1106.txt` | 375 |
| `puzzles7_serg_benchmark.txt` | 10,000 |

tdoku のファイルは [tdoku](https://github.com/t-dillon/tdoku) の `data.zip`
（commit `af426180dc53aef89b82868e7b3fdfcf42165654`）から取得した。
`puzzles2_17_clue.txt` と `puzzles6_forum_hardest_1106.txt` の問題内容は、上記の
`17puz49158.txt` と `HardestDatabase110626.txt` にそれぞれ一致する。
