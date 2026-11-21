# KV260 ボード実装

## FPGA 設計

- KV260 revB の K26（`xck26-sfvc784-2LV-c`）を対象とし、公式 SOM 1.4 プリセットを使用する。
- AXI DMA は Scatter-Gather モードで動作する。メモリーとストリームのデータ幅は 128 ビット、最大バースト長は 64 ビート、アドレス幅は 40 ビット、長さフィールドは 23 ビットである。ホストパケットは最大 1 MiB である。
- PS ポート HP0 は MM2S と Scatter-Gather ディスクリプターに、HP1 は S2MM に使用する。DMA は非コヒーレントのため、Device Tree に `dma-coherent` は指定しない。
- DMA の AXI-Lite アドレスは `0xa0000000`、ソルバーの AXI-Lite アドレスは `0xa0010000` である。
- MM2S と S2MM の割り込みは、それぞれ `pl_ps_irq0[0]` と `pl_ps_irq0[1]` に接続する。GIC SPI 89 と 90 に対応し、どちらもアクティブ High である。
- ファン制御のため、PS TTC0 wave[2] を EMIO 経由で `fan_en_b`（A12）に接続する。ピンは LVCMOS33、ドライブ強度 4 mA、スルーレート SLOW に制約する。

## FPGA ビルド

### ビルド設定

Vivado 同梱の XilinxBoardStore にアクセスできるよう、ビルド前に Vivado の環境を設定する。

`ACCELDOKU_CORES` はソルバーのコア数を 1〜6 で指定する。既定値は 6 である。
単一コアの場合も同じ実装を使用する。ABI は常に 6 コア分のカウンター領域を確保し、未実装のコアのカウンターはゼロを返す。

クロック設定は `CMakeLists.txt` で管理し、PS 設定、タイミング制約、DTBO に適用する。
動作周波数はコアが 115,384,615 Hz、DMA が 187,500,000 Hz である。
制約周期はそれぞれ 1 ps 単位で切り下げた **8.666 ns** と **5.333 ns** で、トップレベル合成と配置配線の両方に適用する。XPM CDC 制約もこの周期で評価する。

`VIVADO_EXECUTABLE` と `DTC_EXECUTABLE` でツールのパスを指定できる。
`ACCELDOKU_VIVADO_JOBS` は、CMake の `--parallel` オプションとは独立して Vivado 内部の並列数を指定する。

### PS クロックとブートファームウェア

上記の動作周波数は、PS の IOPLL 出力が約 1.5 GHz であることを前提とする。
コアクロックは IOPLL / 13、DMA クロックは IOPLL / 8 である。
Device Tree overlay はこれらの周波数を要求するが、実際の周波数はブートファームウェアが設定するクロック構成に依存する。
IOPLL が 1 GHz の場合、同じ要求に対してそれぞれ約 111.111 MHz、166.667 MHz になることがある。

Acceldoku とドライバーをロードした後、実機で実際のクロックを確認できる。

```sh
grep -E ' iopll +| pl[01]_ref +' /sys/kernel/debug/clk/clk_summary
dmesg | grep 'actual clocks'
```

必要な PS クロックを設定する方法の一つとして、AMD の Yocto 2024.2 構成に基づく K26 Starter Kit BootFW 1.04 をインストールできる。
その標準 PS 初期化は IOPLL を約 1.5 GHz に設定し、共有する PS 周辺クロックの分周も設定する。
以下の手順は Ubuntu 24.04 を使用する KV260 revB で実施したもので、Acceldoku のビットストリームの再生成は不要である。

開発機に [Yocto の必要パッケージ](https://docs.yoctoproject.org/brief-yoctoprojectqs/index.html#build-host-packages)と `repo` をインストールし、次を実行する。

```sh
mkdir kria-yocto
cd kria-yocto
repo init -u https://github.com/Xilinx/yocto-manifests.git -b rel-v2024.2 --depth=1
repo sync -c -j4 --no-tags
source ./setupsdk
MACHINE=k26-smk bitbake xilinx-bootbin
```

生成された `BOOT-k26-smk-*.bin` は `build/tmp/deploy/images/k26-smk/` に置かれる。
ビルドフローの詳細は AMD の [BOOT.BIN 生成ガイド](https://xilinx.github.io/kria-apps-docs/bootfw/build/html/docs/bootfw_boot.bin_generation.html)を参照する。

生成ファイルを実機に転送し、実際のパスを指定して root で次を実行する。
更新時はシリアルコンソールと電源操作を利用できるようにしておく。

```sh
xmutil bootfw_status
xmutil bootfw_update -i /path/to/BOOT-k26-smk.bin
xmutil bootfw_status
systemctl reboot
```

更新ツールは未使用側の A/B スロットに書き込み、次回の試験起動対象に設定する。
Linux 起動後、`xmutil bootfw_status`、IOPLL と周辺クロックを確認し、Acceldoku をロードして実際の周波数と動作を確認する。
[トップの README](../../README.md#テスト) に従って実機テストも実行する。
新しいファームウェアを確認できたら、**次の再起動前に**試験イメージを起動可能として確定する。

```sh
xmutil bootfw_update -v
xmutil bootfw_status
```

試験イメージを確定しない場合、次の再起動では以前の起動可能なイメージに戻る。
起動が U-Boot プロンプトで止まった場合は、`run bootcmd` で通常の起動を継続できる。
復旧方法と更新手順の詳細は、AMD の [Kria SOM Boot Firmware Update ガイド](https://xilinx-wiki.atlassian.net/wiki/spaces/A/pages/3020685316/Kria+SOM+Boot+Firmware+Update)を参照する。

検証した実機では、BootFW 1.04 で Linux からのソフトウェア再起動後も IOPLL が 1.5 GHz に維持されることを確認した。
電源を切ってからのコールドブートは未確認である。
初回の試験起動では `run bootcmd` が必要だったが、その後のソフトウェア再起動は自動で完了した。

### ビルドの流れ

ブロックデザイン（BD）は IP ごとの out-of-context 合成ではなく全体をまとめて合成し、合成時にもソルバーにクロック制約を適用する。
`Performance_ExplorePostRoutePhysOpt` ストラテジーによる配置・物理最適化・配線後、`phys_opt_design -directive AggressiveExplore` を実行してからビットストリームを生成する。

| ターゲット | 操作 |
|--------|-----------|
| `kv260-bd` | BD の生成と検証のみを実行（`bd/`） |
| `kv260-synth` | 設計を合成（`synth/`） |
| `kv260-bitstream` | 配置配線、ビットストリーム生成、出力の検査（`implementation/`） |
| `kv260-package` | 必要に応じてビットストリームを生成し、DTBO とともにファームウェアとしてパッケージ化 |

各ステージは別々の Vivado プロジェクトを使用し、ソース、制約、コア数が変わると再生成される。
出力検査はセットアップ、ホールド、パルス幅、CDC、バススキュー、ファンのピンを対象とする。成功時に `build_complete` を作成し、ファームウェアのパッケージ化にはこのファイルが必要である。

### 生成物

```text
build/kv260-pl/fpga/kv260/
  firmware/acceldoku/
    acceldoku.bit.bin
    acceldoku.dtbo
    shell.json
  implementation/
    timing_summary.rpt    セットアップ、ホールド、パルス幅、未制約パス
    utilization.rpt       LUT、FF、CLB、BRAM、URAM の使用量
    cdc.rpt               CDC レポート
    bus_skew.rpt          保持バスと FIFO ポインターのスキュー
    io.rpt, drc.rpt       I/O および DRC レポート
    verified_routed.dcp   検査済みチェックポイント
    acceldoku.xsa         ハードウェア定義
    project/              Vivado プロジェクトと詳細な runme.log
```

## 制御レジスター

| オフセット | 説明 |
|--------|-------------|
| 0x000 | ビット 0：リセット（初期値 1）、ビット 1：有効化 |
| 0x004 | 出力スロット容量 N。リセット中のみ設定可能 |
| 0x008 | TX 開始前に送信待ちを含む登録投入件数を加算。読み出すと累積件数の下位 32 ビットを返す |
| 0x00c | ビット 0：保持されるフレーミングエラー、ビット 1：スナップショット処理中 |
| 0x010 | ビット 0 に 1 を書き込むとカウンターのスナップショットを要求 |
| 0x014 | 識別値 `0x46414405`（統計レジスター配置 v9） |
| 0x018 | 累積登録投入件数の上位 32 ビット |
| 0x100–0x2df | 64 ビットカウンター 60 個。下位 32 ビットが先 |

カウンターの順序は `host/driver/include/acceldoku_ioctl.h` のコメントを参照する。
異なるクロックドメインのカウンターはハンドシェイクにより保持・サンプリングし、すべての結果が排出された後に件数を比較する。
