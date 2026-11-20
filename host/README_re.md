# ホストインターフェース

KV260 の PS 側 Linux から `/dev/acceldoku` を使用するためのカーネルドライバ。
C ioctl ABI、C++ ライブラリ、ベンチマークと実機試験用エントリポイントを含む。

## 実行モデル

CPU 側から FPGA デバイス側への入力を TX、その逆方向を RX と呼ぶ。
両方向はそれぞれがバッファキューで構成され、複数の固定サイズバッファを独立して循環させる
1 つのバッファのサイズは 1 MiB、1 問 64 バイトのレコードを最大 16,384 問格納する。
各バッファは独立に DMA バッファを確保するため、全体が連続している必要ない。
使用するバッファの数は、両方向で各々 1-4096 バッファまで指定できるが、実際に確保可能なバッファの数はシステムで利用可能な DMA メモリに依存する。

デバイスに問題を投入するためには、まず始めに CPU が所有する TX バッファに問題を書き込み、DMA に投入する。
TX の完了は、バッファがデバイスから返却されて再利用されたことを示し、求解完了を意味しない。
空の RX を投入すると、デバイスは結果を書き込み、バッファが満杯になるか投入済みの全問題が完了すると、端数を含めてバッファを RX として返却する。
結果の順序は入力順とは限らないため、ID で対応付ける必要がある。

デバイス側で解が書き込まれた RX は、CPU からデバイスに再投入されるまで上書きされない。
空きの RX バッファがなくなると背圧により処理が停止し、RX が再投入されるまで待機する。

DMA リングは方向ごとに 32 枠で、バッファの数とは独立している。

## C ioctl API

C の API は `host/driver/include/acceldoku_ioctl.h` に定義されている。
`/dev/acceldoku` を `open` し、`ioctl`, `mmap`, `poll` で操作する。
典型的には、以下のような流れで操作する。

0. `/dev/acceldoku` を `open` する。
1. `ACCELDOKUIOC_DMA_INFO` で ABI バージョン、バッファサイズ、バッファ数上限を確認する。
2. `ACCELDOKUIOC_DMA_CONFIG` に TX/RX バッファ数を指定する。
3. 各方向の各バッファについて `ACCELDOKUIOC_QUERYBUF` でオフセットとサイズを取得し、返された値で個別に `MAP_SHARED` マップする。
4. 初期状態では全ての RX/TX バッファが CPU 所有なため、空の RX と、入力を書き込んだ TX を `ACCELDOKUIOC_QBUF` で投入する。
5. `ACCELDOKUIOC_STREAMON` で処理開始。
6. `poll` と `ACCELDOKUIOC_DQBUF` で完了を回収し、必要に応じて再投入する。
7. 必要なら全結果の排出後、`ACCELDOKUIOC_SOLVER_STATS` で統計を取得する。
8. `ACCELDOKUIOC_STREAMOFF` で停止し、各バッファを `munmap` して `close` する。

使用例は `host/tools/acceldoku_run.c` を参照。

## C++ ストリーミング API

C++ ライブラリは `host/lib/include/acceldoku/stream_device.hpp` に定義されている。
本ライブラリの `acceldoku::StreamDevice` は C ioctl API のラッパーであり、基本的に同一の実行モデルに従う。
典型的には、以下のような流れで操作する。

0. `StreamDevice(path, tx_buffers, rx_buffers)` を構築する。
   デバイスのオープン、ABI の確認、バッファの設定とマッピングは自動で行われる。
1. `try_dequeue_tx()` で TX バッファを借用し、`write_tx()` で入力レコードを書き込んで `enqueue_tx()` で投入する。
2. `stream_on()` で処理を開始する。
3. `wait(WaitFor::tx)`、`wait(WaitFor::rx)` または両方を指定し、バッファの空きを待つ。
4. `try_dequeue_tx()` / `try_dequeue_rx()` で利用可能なバッファを取得する。
   取得できなければ `std::nullopt` が返る。
5. 借用した RX は `read_rx()` で結果を読み、`enqueue_rx()` で返却する。
   借用した TX は必要に応じて書き込んで再投入する。
6. 必要なら全結果の排出後、`stats()` で統計を取得する。
7. `stream_off()` で停止する。デストラクタは停止を試み、マッピングと fd を解放する。

バッファ番号は TX/RX ごとに独立し、借用中のみ使える。
`read_rx()` の読み取り専用ビューは RX の返却成功時またはデバイスの破棄時に無効になる。
`stream_off()` は転送中・待機中のデータを破棄するが、借用中のバッファと RX ビューは維持する。

`enqueue_tx()` は書き込み前または最後の書き込みが 0 件の場合、DMA に投入せず TX を返却する。
初期 TX と送信せず返却した TX は `wait()` に通知されないため、待機前に `try_dequeue_tx()` で取得する。
TX と RX はそれぞれ別のスレッドで扱える。
開始・停止・破棄はバッファ操作と同期し、停止後は待機スレッドが終了してから再開する。
使用例は `host/tools/cpp/benchmark/benchmark.cpp` を参照。

## データフォーマット

デバイスは以下のデータフォーマットを用いる。
C++ ライブラリではこのデータフォーマットは `acceldoku::PuzzleRecord` として扱われる。

| バイト                  | 内容                                                                 |
|-------------------------|----------------------------------------------------------------------|
| 0-7                    | 64 bit ID、little endian                                             |
| 8                       | status。入力は 0、出力は 0: 解あり、1: 解なし、2: 不正セル値         |
| 9-49                   | 行優先の 81 セルを 4 bit ずつ格納。先のセルが下位 nibble。空セルは 0 |
| 49 の上位 4 bit、50〜63 | 予約領域。入力は 0、出力も 0                                         |

C++ ライブラリの標準データフォーマットは `acceldoku::Puzzle` であり、これは `std::array<std::uint8_t, 81>` のエイリアスである。
メモリ配置は配置は行優先で、空セルは 0、数字は 1-9 で表す。

`encode(id, puzzle)` で puzzle に id を埋め込んで `PzzuleRecord` に変換する。
また、`PuzzleRecord` の ID、status を取り出すにはそれぞれ `record_id()`、`record_status()` を使用する。
`PuzzleRecord` を `Puzzle` に変換するには `decode_cells()` を使用する。

## エントリポイント

KV260 上のリポジトリルートでビルドする。

```sh
cmake --preset kv260-ps
cmake --build --preset kv260-ps
```

カーネルドライバ `build/kv260-ps/host/driver/src/acceldoku.ko` に生成される。以下は `/dev/acceldoku` を使用可能にしてから実行する。

```sh
# 指定秒数の連続投入。TX バッファ数、RX バッファ数、秒数を指定。
build/kv260-ps/host/tools/acceldoku_run sim/data/17puz49158.txt 8 64 10
build/kv260-ps/host/tools/cpp/acceldoku_benchmark sim/data/17puz49158.txt 8 64 10

# fastdoku による検証
build/kv260-ps/host/tools/cpp/acceldoku_verify sim/data/17puz49158.txt
```

`acceldoku_run`, `acceldoku_benchmark` は、入力を反復コピーして投入するベンチマークである。
出力は内容を読まずに件数を数える。

`acceldoku_verify` は各問題を fastdoku で事前に解き、一意解または解なしであることを確認した上で、実機と fastdoku でチェックサムが一致するか確認する。
Rust / Cargo 1.83 以降と fastdoku サブモジュールが必要で、ビルド済みライブラリーを使う場合は CMake に `-DACCELDOKU_FASTDOKU_ARCHIVE=/絶対パス` を指定できる。

## HW カウンタ

acceldoku にはパフォーマンスを確認するための HW カウンタが実装されている。
実際の型名は C では `struct acceldoku_solver_stats`、C++ ではその別名 `acceldoku::SolverStats` で使用できる。
C は `ACCELDOKUIOC_SOLVER_STATS`、C++ は `StreamDevice::stats()` で名前付きフィールドのスナップショットを取得する。

統計は `STREAMON` で初期化され、`STREAMOFF` でリセットされる。
最終的な値は全件排出後、停止前に取得する。
コアと DMA は異なる時点に採取されるため、処理途中の各段階の件数は一致しない。

| フィールド                                 | 内容                                                                 |
|--------------------------------------------|----------------------------------------------------------------------|
| `accepted` / `emitted`                     | HW が受け入れた入力件数 / 出力した結果件数                           |
| `dispatched` / `collected`                 | コアへ配送した件数 / コアから回収した件数                            |
| `processing_cycles`                        | 最初の配送からスナップショットまでのコアサイクル。排出後の待ちも含む |
| `dispatch_wait_cycles`                     | 前処理済み問題が配送可能だが全コアが受け入れ不能なサイクル           |
| `core_dispatched[6]` / `core_collected[6]` | 各コアへの配送件数 / 各コアからの回収件数                            |
| `core_busy_cycles[6]`                      | 各コアの busy サイクル                                               |
| `core_output_wait_cycles[6]`               | 各コアの出力待ちサイクル                                             |
| `core_input_wait_cycles[6]`                | 各コアが有効かつアイドルで、要求がないサイクル                       |
| `core_queue_peak[6]`                       | 未処理問題と保留枝を含む内部 deque の最大占有数                      |
| `core_branches[6]`                         | 各コアの分岐回数                                                     |
| `core_max_pending[6]`                      | 未探索の保留枝の最大数。探索深さではない                             |
| `core_evaluated[6]`                        | 分岐・矛盾・解のいずれかに到達した状態数。矛盾による完了は 1 回      |

全件排出後は `core_branches / core_collected` で 1 問あたりの平均分岐数、`core_busy_cycles / core_evaluated` で 1 状態あたりの平均 busy サイクル数を求められる。
後者にはメモリー・出力待ちも含む。
分母が 0 の場合は算出できず、ベンチマークと検証ツールは `N/A` と表示する。
