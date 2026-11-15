# ホスト側の要求仕様

## 要求仕様

- TX/RX は独立したキューとして循環
- バッファ数はそれぞれユーザー指定
- 1 バッファは 1MiB/16384問
- バッファ数を増やしても、全体の物理連続領域を要求しない
- ユーザーは `try_dequeue_*()` で空いた入力バッファ、または完了した出力バッファの番号を借用
  - 借りられるものがなければ、待たずに空を返す
  - 使用後は明示的に enqueue または TX の借用を解除
- HW は、そのバッファがフルになった場合、または登録済みの全ての問題が終わったらそのバッファを返す
- 空きの RX がなくなると背圧が伝播し、最終的に空き TX もなくなる
  - RX の返却で再開
- TX と RX のいずれかを待つ、TX のみを待つ、RX のみを待つ API が必要
  - `poll` の完了通知を待ち、初期CPU所有 TX は待機前に取得する
- Puzzle = std::array<uint8_t, 64>? 多分 ID と 81 セル？
  - HW が扱う入力と出力の形式が揃ってるか確認
  - 揃ってなければ HW 側を揃えて (多分 valid フラグみたいなのが出力側にはあるはず)
- 入力バッファへの問題コピーと ID の付与、出力バッファからの結果の取得は呼び出し側の責務
  - ベンチマーク時、出力内容を読むとおそすぎるので、件数だけ数える
  - 入力は 512 バイトのプリフェッチと NEON で書き込むと間に合うらしい
- 統計は数値添字ではなく名前付きフィールドにする
- 公開宣言はinclude/、実装はsrc/に分ける。


## verifyが確認していたこと

- ID の重複・欠落がない
- 問題を解ききった後、新たな問題を解き始めた時、古い問題が混入しない。
- 解ありなら 81 セルが 1-9 で、入力と数独の制約を満たし、CPU の参照解と一致する。
- 解なし、不正セルが正しく扱える
- 出力の予約フィールドがゼロ
- 投入・出力・dispatch・collect の HW 件数とコア別合計が一致し、各コアの最大 inflight が 2 以下

## 現行HWの転送形式

- 1 問 64 バイトの little endian
- ID は先頭 8 バイト
- セルは 4 bit ずつ格納
- 入出力ともbyte 8がstatus、セルはbyte 9から。81セルを行優先・先のセルを下位nibbleに格納。
- 入力statusは0を設定し、HWは無視する。出力statusは0=解あり、1=解なし、2=不正セル値。
- byte 49の上位4 bitからbyte 63までは予約領域。入力はゼロを設定しHWは無視、出力はゼロ。
- 出力statusが0以外ならセルは全てゼロ。出力順は入力順とは限らず、IDで対応付ける。
- HW versionは `0x46414404`。旧HWとは入力セルの位置が異なる。
- ドライバーとABIは `driver/src/fades.c` / `driver/include/fades_ioctl.h` に残している。

## C++ ストリーミング API

`host/lib/include/fades/stream_device.hpp` の `fades::StreamDevice` は ioctl ABI v8 を使う。
`try_dequeue_tx()` / `try_dequeue_rx()` は各方向のバッファー番号を返し、空なら `std::nullopt` を返す。
`encode(id, puzzle)` は81セルを64バイトの `PuzzleRecord` に変換する。
`record_id()`、`record_status()`、`decode_cells()` で出力を解釈できる。
`write_tx(index, span)` は先頭から上書きする。`first_id` を追加するとコピー時にIDを連番で置き換える。
`enqueue_tx(index)` は最後に正常終了した書き込みの 1..16384 件を投入する。
書き込み前または最後の書き込みが空の場合は投入せず例外を送出する。
送信しない TX は `release_tx(index)`、RX は `enqueue_rx(index)` で明示的に返却する。
RX の件数は `rx_size(index)`、内容は `read_rx(index, destination)` で取得する。
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
cmake -S . -B build/cpp -DFADES_BUILD_CPP=ON
cmake --build build/cpp --target fades_benchmark
build/cpp/host/tools/cpp/fades_benchmark sim/data/17puz49158.txt 8 64 10
```

入力は一行81文字の問題テキスト（空セルは `.` または `0`）。
ベンチマークは計測前に変換して初期 TX を投入し、その後は入力を反復コピーし ID を更新する。
結果は件数のみ数え、計測には開始・継続コピー、投入・回収、求解と最終排出を含む。

## 現行C I/F：ABI v8

`driver/include/fades_ioctl.h` をincludeし、`/dev/fades` のioctl / mmap / pollを使う。
現在のC ABIでは明示的に `QBUF` / `DQBUF` を行う。
統計は `fades_solver_stats` の名前付きフィールドで取得する。

### 設定と個別mmap

```c
struct fades_dma_config cfg = {.tx_buffers = 8, .rx_buffers = 64};
ioctl(fd, FADESIOC_DMA_CONFIG, &cfg);

struct fades_buffer_info b = {.queue = FADES_QUEUE_TX, .index = 3};
ioctl(fd, FADESIOC_QUERYBUF, &b);
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
詳細・2枠との比較は [ABI6_RESULTS.md](ABI6_RESULTS.md) を参照。

### C使用例と実機試験

`host/tools/fades_run.c` をkv260-psの通常buildで `build/kv260-ps/host/tools/fades_run` に生成する。
入力は上記の64-byteレコードを連結したバイナリーファイル。
指定時間だけ連続投入し、結果は内容を読まず件数のみ集計する。

```sh
# 連続投入：入力集合を反復コピーし、一意IDを付与。出力内容は読まず件数だけ数える。
# 開始後の入力コピー、投入・回収、RX返却、DMA/求解、最終排出を計測に含む。
build/kv260-ps/host/tools/fades_run input.raw 8 64 10

python3 host/tests/check_abi.py
python3 host/tests/check_poll_stop.py
python3 host/tests/check_stream.py --tx 1 --rx 1
python3 host/tests/check_stream.py --tx 8 --rx 64
python3 host/tests/check_stream.py --tx 65 --rx 65
python3 host/tests/check_example.py build/kv260-ps/host/tools/fades_run \
    --corpus sim/data/17puz49158.txt --seconds 5 --repeat 3
```

連続投入の計測はセットアップ・ファイル読み込み・初回の入力コピーと投入・出力内容の検証を含まない。
アプリも新ヘッダーでビルドする。v7からは `STREAMOFF` のioctl番号とABI番号が変更され、
ioctlマクロは `FADESIOC_*` に改名された。構造体・他のioctl番号・mmap方法は同じ。
