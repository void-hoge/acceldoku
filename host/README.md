# ホスト側のドライバーと API

KV260 の Linux から `/dev/acceldoku` を使うためのカーネルドライバー、C ioctl ABI、
C++20 ライブラリー、ベンチマークと実機試験を収める。C/C++ とも ABI v9 を使用する。

| パス | 内容 |
| --- | --- |
| `driver/include/acceldoku_ioctl.h` | ioctl ABI の宣言 |
| `driver/src/acceldoku.c` | DMA・バッファー・制御を扱うカーネルモジュール |
| `lib/include/acceldoku/stream_device.hpp` / `lib/src/stream_device.cpp` | C++ API と実装 |
| `tools/acceldoku_run.c` | ioctl を直接使う C ベンチマーク |
| `tools/cpp/benchmark/benchmark.cpp` | `StreamDevice` を使う C++ ベンチマーク |
| `tools/cpp/oneshot.cpp` | 全問題を事前投入するワンショットベンチマーク |
| `tools/cpp/verify/` | fastdoku と出力チェックサムを比較する固定件数検証 |
| `tests/` | ABI、ストリーム、停止通知、C++ API の実機試験 |

## バッファーと所有権

- TX/RX は独立したキューとして循環
- バッファ数はそれぞれユーザー指定
- 1 バッファは 1MiB/16384問
- バッファ数を増やしても、全体の物理連続領域を要求しない
- ユーザーは `try_dequeue_*()` で空いた入力バッファ、または完了した出力バッファの番号を借用
  - 借りられるものがなければ、待たずに空を返す
  - 使用後は明示的に enqueue または TX の借用を解除
- HW は RX バッファーが満杯になった場合、または登録済みの全問題が終わった場合に返す
- 空きの RX がなくなると背圧が伝播し、最終的に空き TX もなくなる
  - RX の返却で再開
- TX と RX のいずれか、または片方向の完了を待てる
  - `poll` の完了通知を待ち、初期CPU所有 TX は待機前に取得する
- 入力への問題コピー・ID付与と、出力内容を読むかどうかは呼び出し側が選ぶ
- 統計は名前付きフィールドで取得する

## 現行HWの転送形式

- 1 問 64 バイトの little endian
- ID は先頭 8 バイト
- セルは 4 bit ずつ格納
- 入出力ともbyte 8がstatus、セルはbyte 9から。81セルを行優先・先のセルを下位nibbleに格納。
- 入力statusは0を設定し、HWは無視する。出力statusは0=解あり、1=解なし、2=不正セル値。
- byte 49の上位4 bitからbyte 63までは予約領域。入力はゼロを設定しHWは無視、出力はゼロ。
- 出力statusが0以外ならセルは全てゼロ。出力順は入力順とは限らず、IDで対応付ける。
- HW versionは `0x46414405`。統計のレジスター配置は旧HWと異なる。
- C++ の `Puzzle` は81セル、`PuzzleRecord` は64バイトの転送レコード。

## C++ ストリーミング API

`host/lib/include/acceldoku/stream_device.hpp` の `acceldoku::StreamDevice` は ioctl ABI v9 を使う。
`try_dequeue_tx()` / `try_dequeue_rx()` は各方向のバッファー番号を返し、空なら `std::nullopt` を返す。
`StreamDevice::batch_capacity()` は1バッファーに格納できるレコード数を返す。
`encode(id, puzzle)` は81セルを64バイトの `PuzzleRecord` に変換する。
`record_id()`、`record_status()`、`decode_cells()` で出力を解釈できる。
`write_tx(index, span)` は先頭から上書きする。`first_id` を追加するとコピー時にIDを連番で置き換える。
`enqueue_tx(index)` は最後に正常終了した書き込みの 1..16384 件を投入する。
書き込み前または最後の書き込みが空の場合は DMA に投入せず TX を返却する。
送信しない TX は空の書き込みの後に `enqueue_tx(index)`、RX は `enqueue_rx(index)` で返却する。
RX の件数は `rx_size(index)`、内容は `read_rx(index)` が返すコピーなしの
`std::span<const PuzzleRecord>` で取得する。保持する必要があれば呼び出し側でコピーする。
ビューは `enqueue_rx(index)` の成功時またはデバイスの破棄時に無効になり、
借用中の `stream_off()`・`stream_on()` をまたいでは有効である。
範囲外や借用中でない番号の操作は例外となる。投入失敗時は借用を保持し、その場で例外を送出する。
各番号は対応する方向で借用中にのみ利用できる。同じ番号の同時使用や返却後の利用は呼び出し側で防ぐ。
再貸出後の番号は以前の借用と区別されない。明示返却しないバッファーは借用中のままになる。
TX と RX はそれぞれ一つのスレッドが扱える。デバイスの開始・停止・破棄は
バッファー操作と同期する。開始・停止はバッファーを借用中でも可能で、
借用中の TX は再貸出せず、借用中の RX は返却されるまで再投入しない。
`stream_on()` は借用されていない RX を投入する。
`stream_off()` は転送中・待機中のデータを破棄するが、借用中の RX データは保持する。
停止後は全待機スレッドの終了を確認してから再開する。
`wait()` は `poll` による完了待ちで、初期CPU所有のTXは通知されない。
開始時は初期TXを `try_dequeue_tx()` で取得・投入してから `wait()` する。
未投入で返された空きTXもpollでは通知されず、次の待機前に再取得する。
`wait(WaitFor::tx | WaitFor::rx)` で両方向を待てる。

```cpp
if (auto tx = device.try_dequeue_tx()) {
    device.write_tx(*tx, puzzles, first_id);
    device.enqueue_tx(*tx);
}
if (auto rx = device.try_dequeue_rx()) {
    received += device.rx_size(*rx); // 内容を読むなら read_rx() を使う
    device.enqueue_rx(*rx);
}
```

```sh
cmake --preset kv260-ps
cmake --build --preset kv260-ps
build/kv260-ps/host/tools/cpp/acceldoku_benchmark sim/data/17puz49158.txt 8 64 10
```

入力は一行81文字の問題テキスト（空セルは `.` または `0`）。
ベンチマークは計測前に変換して初期 TX を投入し、その後は入力を反復コピーし ID を更新する。
結果は件数のみ数え、計測には開始・継続コピー、投入・回収、求解と最終排出を含む。

### 固定件数の検証

Rust / Cargo 1.83 以降と fastdoku サブモジュールが必要。検証機向けにビルド済みの
`libacceldoku_fastdoku_ffi.a` がある場合は `-DACCELDOKU_FASTDOKU_ARCHIVE=/絶対パス` も指定できる。

```sh
cmake --preset kv260-ps
cmake --build --preset kv260-ps
build/kv260-ps/host/tools/cpp/acceldoku_verify sim/data/17puz49158.txt
```

入力ファイルの空行以外の全問題を一度ずつ投入する。TX/RX バッファー数は各 4、
検証スレッド数も 4 に固定する。計測前に各問題を fastdoku で解き、
解が一意または解なしであることを確認する。複数解の場合は検証を中止する。
ID は 0 からの連番。期待値と実際の各64バイトレコード（ID、status、セル、予約領域）を
4 本の 32-bit word ハッシュと 64-bit avalanche でハッシュし、件数、剰余和、XOR を比較する。
各 RX バッチの読み取りを並列化し、
全スレッドの処理完了後に RX を返却する。AArch64 の little-endian 環境では
レコード内の 4 本のハッシュを NEON で同時に計算する。
集約は出力順に依存しないが、ハッシュ衝突の可能性がある。リファレンス解答と
期待チェックサムの生成は計測外、出力の読み取りとチェックサムは計測内。
最後の部分バッチを含め全件排出し、ハードウェアの累積件数も確認する。

## 現行C I/F：ABI v9

`driver/include/acceldoku_ioctl.h` をincludeし、`/dev/acceldoku` のioctl / mmap / pollを使う。
現在のC ABIでは明示的に `QBUF` / `DQBUF` を行う。
統計は `acceldoku_solver_stats` の名前付きフィールドで取得する。

統計は STREAMON で初期化され、STREAMOFF でリセットされる。取得は排出後、
STREAMOFF 前に行う。コアとDMAは異なる時点に採取されるため、途中の件数は一致しない。
`processing_cycles` は最初の配送からスナップショットまで継続し、排出後の待ちも含む。
`dispatch_wait_cycles` は前処理済みの問題が配送可能だが全コアが受け入れ不能な時間。
各コアの `core_queue_peak` は未処理問題と保留枝を含む内部dequeの最大占有数、
`core_max_pending` は未探索の保留枝の最大数で、最大探索深さとは異なる。
`core_branches` は分岐回数、`core_evaluated` は分岐・矛盾・解のいずれかに
到達した状態数（矛盾による完了は1回）。全件排出後は、コアごとの
`core_branches / core_collected` が1問あたりの平均分岐数、
`core_busy_cycles / core_evaluated` が1状態あたりの平均busyサイクル数。
後者はメモリー・出力待ちなども含む。分母が0なら平均は算出できない。
`acceldoku_benchmark` と `acceldoku_verify` はこれらの統計と平均値を項目ごとに出力する。
分母が0の平均値は `N/A` と表示する。

### 設定と個別mmap

```c
struct acceldoku_dma_config cfg = {.tx_buffers = 8, .rx_buffers = 64};
ioctl(fd, ACCELDOKUIOC_DMA_CONFIG, &cfg);

struct acceldoku_buffer_info b = {.queue = ACCELDOKU_QUEUE_TX, .index = 3};
ioctl(fd, ACCELDOKUIOC_QUERYBUF, &b);
void *tx3 = mmap(NULL, b.map_bytes, PROT_READ | PROT_WRITE,
                 MAP_SHARED, fd, (off_t)b.mmap_offset);
```

実際のコードではすべての戻り値を確認する。`mmap` の失敗は `MAP_FAILED`。
64-bitのオフセットを使用するため、32-bit userspaceでは `_FILE_OFFSET_BITS=64` を指定する。

- `DMA_INFO`：ABI、1面のサイズ、方向ごとの上限、設定済みTX/RX面数を取得。
  open直後の面数は0。
- `DMA_CONFIG`：TX/RXそれぞれ1〜4096面、1面1 MiBを指定する。全体が物理連続である
  必要はなく、各面を独立確保する。実際の確保限界は利用可能なDMAメモリーによる。
  物理RAM総量を超える要求や確保失敗は `ENOMEM`。途中失敗は全解放して再試行可能。
  データ領域は `__GFP_NORETRY` で確保し、要求のためにOOM killerを起動しない。
- 設定成功はopenごとに1回だけ。再設定は `EBUSY`。予約フィールドは0。
- `QUERYBUF`：queue/indexを指定すると、その面の `mmap_offset` / `map_bytes` を返す。
  オフセットは不透明な識別値で、物理/DMAアドレスではない。算出規則に依存しないこと。
- 1面ずつ、返されたサイズとオフセットで `MAP_SHARED` を使ってマップする。
  別の面の仮想アドレスが連続する保証はない。ポインターを面ごとに保存する。

### 投入・回収と所有権

```text
open → CONFIG → 各面のQUERYBUF / mmap
     → 空のRX面をQBUF → TX面に書き込んでQBUF → STREAMON
     → poll / DQBUF / QBUF
     → 最終TX投入後はRXのみpoll → 投入件数分を回収
     → STREAMOFF → munmap / close
```

- TXの `QBUF` は `records=1..16384`、RXは `records=0`。
- 設定直後は全TX/RX面がCPU所有。空のRXと入力を書き込んだTXをそれぞれ投入する。
- `QBUF` 後はDMA側所有。`DQBUF` で回収するまでその面にアクセスしない。
- TXの完了は転送元として再利用可能という意味で、求解完了ではない。
- RXの `DQBUF` は実際の出力件数を返す。内容を読む必要はなく、件数だけ数えて返却できる。
- `DQBUF` は常にノンブロッキングで、完了なしは `EAGAIN`。
  `poll` の `POLLOUT` は完了TX、`POLLIN` は完了RXの回収可能を表す。
  初期CPU所有のTX面があること自体は `POLLOUT` の条件ではない。
- 待機対象は `poll` のeventsで指定する。TXのみは `POLLOUT`、RXのみは `POLLIN`、
  両方は `POLLOUT | POLLIN`。pollは完了を消費せず、回収するまで即座に戻る。
- 入力終了通知は不要。RXは満杯または登録済み全問題の完了で端数を含めて返る。
  最終投入後はRXのみを待ち、回収件数が投入件数に達したら終了する。idle後の再投入も可能。
- v7で `END_INPUT` を削除し、v8では番号6を `STREAMOFF` に割り当てた。
- `STREAMOFF` は転送をリセットし、全面をCPU所有に戻す。設定とマッピングは維持する。
- `STREAMOFF` が成功すると、初回 `STREAMON` 前の呼び出しを含め、待機方向にかかわらず
  `poll` は `POLLHUP` を返す。停止後にpollした場合も、次の `STREAMON` 成功まで通知が続く。
  初回open直後は通知しない。停止失敗時は `POLLERR`、デバイス取り外し時は `POLLERR | POLLHUP`。
- 複数スレッドでfdを共有する場合は、`STREAMOFF` 後に全poll待機スレッドが停止を確認して
  終了するまで待ってから再開する。先に `STREAMON` が成功すると、停止通知を見逃しうる。
- 同時openは1つ。VMAはファイルを保持するため、fdをcloseしてもmmapが残る間は
  メモリーと排他所有が維持される。終了時は明示的に停止し、全VMAとfdを解放する。
  最後の参照の解放後、次のopenは未設定状態から始まる。

### 大容量バッファとDMAリング

出力読み出しを入力供給のクリティカルパスから外すことが目的。
TX/RXを事前投入し、十分なRX容量があれば、アプリが出力を一度も回収しなくても
ドライバーが次の面を登録して転送を継続する。完了面は上書きせず保持する。
RXが全て埋まれば背圧で停止し、面の返却で再開する。

DMAリングは方向ごとに32枠固定、バッファ数とは独立。1枠はデータを格納せず、
1面のDMAアドレス・転送長・状態を保持する。HW完了後に枠だけ再利用するので、
バッファが65面以上でも転送・蓄積できる。リング長は公開設定にしない。
所有数を明示管理するため、空/満杯判定のためのガード枠は不要。

## ビルドと実機試験

KV260 のリポジトリルートで実行する。ロード手順とカーネルヘッダーの準備は
[KV260 ボード実装](../fpga/kv260/README.md) を参照。

```sh
cmake --preset kv260-ps
cmake --build --preset kv260-ps
```

`build/kv260-ps/host/driver/src/acceldoku.ko`、Cツール
`build/kv260-ps/host/tools/acceldoku_run`、C++ライブラリーと
`build/kv260-ps/host/tools/cpp/acceldoku_benchmark`、
`acceldoku_verify` が生成される。

以下は `/dev/acceldoku` を利用できる状態で実行する。Cツールは1行81セルの問題テキストを読む。
空セルは `.` または `0`、空行と `#` コメント行は無視する。変換は計測前に行う。
両ベンチマークとも指定時間だけ連続投入し、
結果は内容を読まず件数のみ集計する。

```sh
# 連続投入：入力集合を反復コピーし、一意IDを付与。出力内容は読まず件数だけ数える。
# 開始後の入力コピー、投入・回収、RX返却、DMA/求解、最終排出を計測に含む。
build/kv260-ps/host/tools/acceldoku_run sim/data/17puz49158.txt 8 64 10
build/kv260-ps/host/tools/cpp/acceldoku_benchmark sim/data/17puz49158.txt 8 64 10

python3 host/tests/check_abi.py
python3 host/tests/check_poll_stop.py
python3 host/tests/check_stream.py --tx 1 --rx 1
python3 host/tests/check_stream.py --tx 8 --rx 64
python3 host/tests/check_stream.py --tx 65 --rx 65
python3 host/tests/check_example.py build/kv260-ps/host/tools/acceldoku_run \
    --corpus sim/data/17puz49158.txt --seconds 5 --repeat 3

# C++ API の変換・所有権・停止再開を実機で確認
c++ -std=c++20 -O2 -D_FILE_OFFSET_BITS=64 \
    -Ihost/lib/include -Ihost/driver/include \
    host/lib/src/stream_device.cpp host/tests/cpp_smoke.cpp -o /tmp/acceldoku_cpp_smoke
/tmp/acceldoku_cpp_smoke
```

連続投入の計測はセットアップ・ファイル読み込み・初回の入力コピーと投入・出力内容の検証を含まない。
アプリは現行ヘッダーでビルドする。v7からは `STREAMOFF` のioctl番号とABI番号が変更され、
ioctlマクロは `ACCELDOKUIOC_*` に改名された。
