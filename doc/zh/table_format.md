leveldb File format（文件格式）
=============================

> 注：本文是 `doc/table_format.md` 的中文翻译；以英文原文为准。（2026-01-30）

    <beginning_of_file>
    [data block 1]
    [data block 2]
    ...
    [data block N]
    [meta block 1]
    ...
    [meta block K]
    [metaindex block]
    [index block]
    [Footer]        (fixed size; starts at file_size - sizeof(Footer))
    <end_of_file>

该文件包含内部指针。每个指针称为 BlockHandle，包含如下信息：

    offset:   varint64
    size:     varint64

关于 varint64 格式的解释，参见 [varints](https://developers.google.com/protocol-buffers/docs/encoding#varints)。

1. 文件中的 key/value 对按顺序存储，并分区为一系列 data block。这些 block 从文件开头开始依次排列。每个 data block 按 `block_builder.cc` 中的代码格式化，然后可选地压缩。

2. data block 之后是一些 meta block。支持的 meta block 类型见下文。未来可能会增加更多 meta block 类型。每个 meta block 同样使用 `block_builder.cc` 格式化，然后可选地压缩。

3. “metaindex” block：包含每个其他 meta block 的一条记录，其中 key 是 meta block 名称，value 是指向该 meta block 的 BlockHandle。

4. “index” block：每个 data block 一条记录，其中 key 是一个字符串，满足该 key 大于等于该 data block 的最后一个 key，且小于其后继 data block 的第一个 key。value 是该 data block 的 BlockHandle。

5. 文件末尾是一个固定长度的 footer，其中包含 metaindex 和 index block 的 BlockHandle，以及一个 magic number。

        metaindex_handle: char[p];     // Block handle for metaindex
        index_handle:     char[q];     // Block handle for index
        padding:          char[40-p-q];// zeroed bytes to make fixed length
                                       // (40==2*BlockHandle::kMaxEncodedLength)
        magic:            fixed64;     // == 0xdb4775248b80fb57 (little-endian)

## “filter” Meta Block

如果在打开数据库时指定了 `FilterPolicy`，则每个 table 中都会存储一个 filter block。“metaindex” block 中会包含一条从 `filter.<N>` 映射到该 filter block 的 BlockHandle 的记录，其中 `<N>` 是 filter policy 的 `Name()` 方法返回的字符串。

filter block 存储了一系列 filter，其中 filter i 是对“文件偏移落在以下范围内的 block 中的所有 key”调用 `FilterPolicy::CreateFilter()` 得到的输出：

    [ i*base ... (i+1)*base-1 ]

当前 “base” 为 2KB。例如，如果 block X 和 Y 的起始位置落在 `[ 0KB .. 2KB-1 ]`，则 X 和 Y 中所有 key 都会被转换为一个 filter（通过调用 `FilterPolicy::CreateFilter()`），该 filter 会作为 filter block 的第一个 filter 存储。

filter block 的格式如下：

    [filter 0]
    [filter 1]
    [filter 2]
    ...
    [filter N-1]

    [offset of filter 0]                  : 4 bytes
    [offset of filter 1]                  : 4 bytes
    [offset of filter 2]                  : 4 bytes
    ...
    [offset of filter N-1]                : 4 bytes

    [offset of beginning of offset array] : 4 bytes
    lg(base)                              : 1 byte

filter block 末尾的 offset 数组允许高效地从某个 data block 的偏移定位到对应的 filter。

## “stats” Meta Block

该 meta block 包含一系列统计信息。key 是统计项名称，value 是统计值。

TODO(postrelease): record following stats.

    data size
    index size
    key size (uncompressed)
    value size (uncompressed)
    number of entries
    number of data blocks

