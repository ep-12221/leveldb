leveldb Log format（日志格式）
============================

> 注：本文是 `doc/log_format.md` 的中文翻译；以英文原文为准。（2026-01-30）

日志文件内容由一系列 32KB 的 block 组成。唯一的例外是文件末尾可能包含一个不完整的 block。

每个 block 由一系列 record 组成：

    block := record* trailer?
    record :=
      checksum: uint32     // crc32c of type and data[] ; little-endian
      length: uint16       // little-endian
      type: uint8          // One of FULL, FIRST, MIDDLE, LAST
      data: uint8[length]

record 不会从一个 block 的最后 6 个字节之内开始（因为放不下）。这些剩余字节构成 trailer，必须全部为 0，读者必须跳过它们。

旁注：如果当前 block 恰好剩余 7 个字节，并且要追加一个长度非零的新 record，写入方必须先写出一个 FIRST record（其中用户数据长度为 0）来填满结尾这 7 个字节，然后再在后续 block 中写出所有用户数据。

未来可能会增加更多的 type。有些 reader 会跳过自己不理解的 record type；另一些 reader 可能会报告有数据被跳过。

    FULL == 1
    FIRST == 2
    MIDDLE == 3
    LAST == 4

FULL record 包含一个完整用户 record 的全部内容。

FIRST、MIDDLE、LAST 用于表示一个被拆分成多个片段的用户 record（通常因为 block 边界）。FIRST 是用户 record 的第一个片段，LAST 是最后一个片段，MIDDLE 是中间的所有片段。

示例：考虑一串用户 record：

    A: length 1000
    B: length 97270
    C: length 8000

**A** 会以 FULL record 的形式存储在第一个 block 中。

**B** 会被拆成三个片段：第一个片段占据第一个 block 的剩余部分；第二个片段占满第二个 block；第三个片段占据第三个 block 的前缀。这会导致第三个 block 剩余 6 个字节空闲，它们会作为 trailer 留空。

**C** 会以 FULL record 的形式存储在第四个 block 中。

----

## 相比 recordio 格式的一些优点：

1. 不需要任何用于重新同步（resync）的启发式规则：直接跳到下一个 block 边界并扫描即可。如果发生损坏，就跳到下一个 block。额外的好处是：即使一个日志文件的部分内容被嵌入为另一个日志文件内部的 record，我们也不会被搞混。

2. 在近似边界处分割（例如用于 mapreduce）很简单：找到下一个 block 边界，然后跳过 record，直到遇到 FULL 或 FIRST record。

3. 不需要为大 record 做额外缓冲。

## 相比 recordio 格式的一些缺点：

1. 不会把很小的 record 打包。这可以通过增加一种新的 record type 来修复，因此这是当前实现的缺点，不一定是该格式本身的必然缺点。

2. 不支持压缩。同样，这也可以通过增加新的 record type 来修复。

