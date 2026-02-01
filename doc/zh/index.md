leveldb
=======

_Jeff Dean, Sanjay Ghemawat_

> 注：本文是 `doc/index.md` 的中文翻译；以英文原文为准。（2026-01-30）

leveldb 库提供一个持久化的键值存储（key-value store）。键和值都是任意的字节数组。键在键值存储中的顺序由用户指定的比较器（comparator）函数决定。

## 打开数据库

一个 leveldb 数据库有一个名称，对应文件系统中的一个目录。数据库的全部内容都存放在这个目录中。下面的示例展示了如何打开一个数据库；如有必要则创建它：

```c++
#include <cassert>
#include "leveldb/db.h"

leveldb::DB* db;
leveldb::Options options;
options.create_if_missing = true;
leveldb::Status status = leveldb::DB::Open(options, "/tmp/testdb", &db);
assert(status.ok());
...
```

如果你希望在数据库已经存在时直接报错，在调用 `leveldb::DB::Open` 之前加上这一行：

```c++
options.error_if_exists = true;
```

## Status

你可能注意到了上面的 `leveldb::Status` 类型。leveldb 中多数可能遇到错误的函数都会返回这种类型的值。你可以检查结果是否 ok，也可以打印相关的错误信息：

```c++
leveldb::Status s = ...;
if (!s.ok()) cerr << s.ToString() << endl;
```

## 关闭数据库

当你使用完数据库后，直接删除数据库对象即可。例如：

```c++
... open the db as described above ...
... do something with db ...
delete db;
```

## 读写

数据库提供 Put、Delete 和 Get 方法来修改/查询数据库。例如，下面的代码把 key1 下存放的值移动到 key2：

```c++
std::string value;
leveldb::Status s = db->Get(leveldb::ReadOptions(), key1, &value);
if (s.ok()) s = db->Put(leveldb::WriteOptions(), key2, value);
if (s.ok()) s = db->Delete(leveldb::WriteOptions(), key1);
```

## 原子更新

注意：如果进程在 Put(key2) 之后、Delete(key1) 之前崩溃，那么相同的值可能会同时留在多个键下面。可以使用 `WriteBatch` 类原子地应用一组更新来避免这类问题：

```c++
#include "leveldb/write_batch.h"
...
std::string value;
leveldb::Status s = db->Get(leveldb::ReadOptions(), key1, &value);
if (s.ok()) {
  leveldb::WriteBatch batch;
  batch.Delete(key1);
  batch.Put(key2, value);
  s = db->Write(leveldb::WriteOptions(), &batch);
}
```

`WriteBatch` 保存了一串要对数据库执行的编辑操作，并且这些操作会按顺序应用。注意我们先调用 Delete 再 Put：这样如果 key1 与 key2 相同，就不会错误地把值整个丢掉。

除了原子性之外，`WriteBatch` 也可以通过把大量单独的变更放在同一个 batch 里来加速批量更新。

## 同步写入

默认情况下，每次写入 leveldb 都是异步的：它会在把写操作从进程推入操作系统之后就返回。操作系统内存到下层持久化存储的传输是异步发生的。可以为某次写入打开 sync 标志，让写操作直到数据被完全推送到持久化存储后才返回。（在 Posix 系统上，这通常通过在写操作返回前调用 `fsync(...)` 或 `fdatasync(...)` 或 `msync(..., MS_SYNC)` 来实现。）

```c++
leveldb::WriteOptions write_options;
write_options.sync = true;
db->Put(write_options, ...);
```

异步写入通常比同步写入快一千倍以上。异步写入的缺点是：机器崩溃可能导致最后几次更新丢失。注意：仅写入进程崩溃（即没有重启机器）不会造成任何丢失，因为即使 sync 为 false，更新在被认为完成之前也会从进程内存推入操作系统。

异步写入在很多场景下可以安全使用。例如，在向数据库加载大量数据时，你可以通过在崩溃后重启批量加载来处理丢失的更新。也可以采用一种混合方案：每 N 次写入做一次同步写入；如果发生崩溃，则在重启时从上一次同步写入完成之后的位置继续。（同步写入可以更新一个标记，用于描述崩溃后从哪里恢复。）

`WriteBatch` 提供了另一种替代异步写入的方式：把多个更新放进同一个 WriteBatch 中，并使用一次同步写入来共同应用（即把 `write_options.sync` 设为 true）。这样一次同步写入的额外成本会被 batch 中的所有写入分摊。

## 并发

一个数据库在同一时刻只能被一个进程打开。leveldb 的实现会从操作系统获取一个锁来防止误用。在单个进程内部，同一个 `leveldb::DB` 对象可以安全地在多个并发线程之间共享。也就是说，不同线程可以在不做任何外部同步的情况下向同一个数据库写入、获取迭代器、或调用 Get（leveldb 会自动做必要的同步）。但其他对象（例如 Iterator 和 `WriteBatch`）可能需要外部同步：如果两个线程共享这类对象，它们必须使用自己的锁机制来保护对该对象的访问。更多细节可见公开头文件。

## 遍历

下面的示例展示如何打印数据库中所有 key,value 对：

```c++
leveldb::Iterator* it = db->NewIterator(leveldb::ReadOptions());
for (it->SeekToFirst(); it->Valid(); it->Next()) {
  cout << it->key().ToString() << ": "  << it->value().ToString() << endl;
}
assert(it->status().ok());  // Check for any errors found during the scan
delete it;
```

下面的变体展示如何只处理区间 [start,limit) 内的键：

```c++
for (it->Seek(start);
   it->Valid() && it->key().ToString() < limit;
   it->Next()) {
  ...
}
```

你也可以按逆序处理条目。（注意：逆序遍历可能比正序遍历慢一些。）

```c++
for (it->SeekToLast(); it->Valid(); it->Prev()) {
  ...
}
```

## 快照

快照提供了覆盖整个键值存储状态的一致、只读视图。`ReadOptions::snapshot` 可以设为非 NULL，表示这次读取应在 DB 状态的某个特定版本上执行。如果 `ReadOptions::snapshot` 为 NULL，则读取会在当前状态的一个隐式快照上执行。

快照由 `DB::GetSnapshot()` 创建：

```c++
leveldb::ReadOptions options;
options.snapshot = db->GetSnapshot();
... apply some updates to db ...
leveldb::Iterator* iter = db->NewIterator(options);
... read using iter to view the state when the snapshot was created ...
delete iter;
db->ReleaseSnapshot(options.snapshot);
```

注意：当一个快照不再需要时，应该通过 `DB::ReleaseSnapshot` 接口释放。这允许实现清理那些仅用于支持“按该快照读取”而维护的状态。

## Slice

上面 `it->key()` 和 `it->value()` 的返回值是 `leveldb::Slice` 类型的实例。Slice 是一个简单结构：包含长度，以及指向外部字节数组的指针。返回 Slice 比返回 `std::string` 更省，因为不需要拷贝可能很大的键和值。此外，leveldb 方法不会返回以 `'\0'` 结尾的 C 风格字符串，因为 leveldb 的键和值允许包含 `'\0'` 字节。

C++ 字符串与以 `'\0'` 结尾的 C 风格字符串都可以轻松转换为 Slice：

```c++
leveldb::Slice s1 = "hello";

std::string str("world");
leveldb::Slice s2 = str;
```

Slice 也可以轻松转换回 C++ 字符串：

```c++
std::string str = s1.ToString();
assert(str == std::string("hello"));
```

使用 Slice 时要小心：调用者必须确保 Slice 指向的外部字节数组在 Slice 使用期间保持有效。例如，下面这段代码是有 bug 的：

```c++
leveldb::Slice slice;
if (...) {
  std::string str = ...;
  slice = str;
}
Use(slice);
```

当 if 语句作用域结束后，str 会被销毁，slice 的底层存储也就消失了。

## 比较器

前面的示例使用了默认的键排序函数：按字节字典序排序。但你也可以在打开数据库时提供自定义比较器。例如，假设每个数据库键由两个数字组成，我们应先按第一个数字排序，若相同再按第二个数字排序。首先，定义一个合适的 `leveldb::Comparator` 子类来表达这些规则：

```c++
class TwoPartComparator : public leveldb::Comparator {
 public:
  // Three-way comparison function:
  //   if a < b: negative result
  //   if a > b: positive result
  //   else: zero result
  int Compare(const leveldb::Slice& a, const leveldb::Slice& b) const {
    int a1, a2, b1, b2;
    ParseKey(a, &a1, &a2);
    ParseKey(b, &b1, &b2);
    if (a1 < b1) return -1;
    if (a1 > b1) return +1;
    if (a2 < b2) return -1;
    if (a2 > b2) return +1;
    return 0;
  }

  // Ignore the following methods for now:
  const char* Name() const { return "TwoPartComparator"; }
  void FindShortestSeparator(std::string*, const leveldb::Slice&) const {}
  void FindShortSuccessor(std::string*) const {}
};
```

然后用这个自定义比较器创建数据库：

```c++
TwoPartComparator cmp;
leveldb::DB* db;
leveldb::Options options;
options.create_if_missing = true;
options.comparator = &cmp;
leveldb::Status status = leveldb::DB::Open(options, "/tmp/testdb", &db);
...
```

### 向后兼容性

比较器 Name 方法的返回值会在数据库创建时附加到数据库上，并在之后每次打开数据库时检查。如果这个名字发生变化，`leveldb::DB::Open` 会失败。因此，只有当新的键格式与比较函数与现有数据库不兼容，并且可以接受丢弃所有既有数据库内容时，才应该修改该名字。

不过，只要提前做一点规划，你仍然可以随着时间逐步演进键格式。例如，你可以在每个键的末尾存一个版本号（对大多数用途来说 1 字节就够）。当你想切换到新的键格式时（例如，为 `TwoPartComparator` 处理的键增加一个可选的第三部分），可以做到：(a) 保持比较器名字不变；(b) 为新写入的键递增版本号；(c) 修改比较函数，使其根据键中版本号来决定如何解释该键。

## 性能

可以通过修改 `include/options.h` 中各类型的默认值来调优性能。

### 块大小

leveldb 会把相邻的键分组成同一个块（block），而 block 是与持久化存储之间传输的基本单位。默认块大小约为 4096 字节（未压缩）。主要做大范围扫描的应用可能希望增大这个值。大量点查（point read）且 value 很小的应用，可能希望切换为更小的块大小（前提是性能测试显示有提升）。通常使用小于 1KB 的块意义不大，使用大于几 MB 的块也意义不大。另外，块越大通常压缩效果越好。

### 压缩

每个 block 在写入持久化存储之前都会被单独压缩。默认启用压缩，因为默认压缩方法非常快，并且对不可压缩的数据会自动禁用压缩。在极少数情况下，应用可能希望完全禁用压缩，但应仅在基准测试显示性能有提升时才这样做：

```c++
leveldb::Options options;
options.compression = leveldb::kNoCompression;
... leveldb::DB::Open(options, name, ...) ....
```

### 缓存

数据库内容存储在文件系统中的一组文件里，每个文件存放一系列已压缩的 block。如果 options.block_cache 非 NULL，则它会用于缓存常用的、未压缩的 block 内容。

```c++
#include "leveldb/cache.h"

leveldb::Options options;
options.block_cache = leveldb::NewLRUCache(100 * 1048576);  // 100MB cache
leveldb::DB* db;
leveldb::DB::Open(options, name, &db);
... use the db ...
delete db
delete options.block_cache;
```

注意：cache 缓存的是未压缩数据，因此应按应用层数据大小来设置其容量，而不是按压缩后的大小来折算。（压缩 block 的缓存交给操作系统的 buffer cache，或由客户端提供的自定义 Env 实现来处理。）

当执行批量读取时，应用可能希望禁用缓存，避免批量读取的数据挤掉大部分缓存内容。可以用“每个迭代器”的选项来实现：

```c++
leveldb::ReadOptions options;
options.fill_cache = false;
leveldb::Iterator* it = db->NewIterator(options);
for (it->SeekToFirst(); it->Valid(); it->Next()) {
  ...
}
delete it;
```

### 键的布局

注意：磁盘传输与缓存的单位是 block。按数据库排序顺序相邻的键通常会被放到同一个 block 中。因此，应用可以通过把常一起访问的键放得更接近（在键空间上更相邻），并把不常用的键放到键空间的另一个区域来提升性能。

例如，假设我们在 leveldb 之上实现一个简单文件系统。我们可能想存储的条目类型有：

    filename -> permission-bits, length, list of file_block_ids
    file_block_id -> data

我们可以把 filename 键都用一个字母前缀（比如 '/'），把 `file_block_id` 键用另一个字母前缀（比如 '0'），这样只扫描元数据就不会被迫读取并缓存庞大的文件内容。

### 过滤器

由于 leveldb 在磁盘上的组织方式，一次 `Get()` 调用可能涉及多次磁盘读取。可选的 FilterPolicy 机制可用于显著减少磁盘读取次数。

```c++
leveldb::Options options;
options.filter_policy = NewBloomFilterPolicy(10);
leveldb::DB* db;
leveldb::DB::Open(options, "/tmp/testdb", &db);
... use the database ...
delete db;
delete options.filter_policy;
```

上面的代码为数据库关联了一个基于 Bloom filter（布隆过滤器）的过滤策略。基于 Bloom filter 的过滤依赖于为每个 key 在内存中保留一定数量的 bit（此处为每个 key 10 bit，因为这是传给 `NewBloomFilterPolicy` 的参数）。该过滤器将把 Get() 调用所需的非必要磁盘读取数量降低大约 100 倍。增加每 key 的 bit 数会进一步降低磁盘读取，但会消耗更多内存。我们建议：工作集装不进内存、并且做大量随机读取的应用应设置 filter policy。

如果你使用自定义比较器，应确保使用的 filter policy 与比较器兼容。例如，考虑一个在比较键时忽略尾部空格的比较器：这时不能使用 `NewBloomFilterPolicy`。应用应提供一个同样忽略尾部空格的自定义 filter policy。例如：

```c++
class CustomFilterPolicy : public leveldb::FilterPolicy {
 private:
  leveldb::FilterPolicy* builtin_policy_;

 public:
  CustomFilterPolicy() : builtin_policy_(leveldb::NewBloomFilterPolicy(10)) {}
  ~CustomFilterPolicy() { delete builtin_policy_; }

  const char* Name() const { return "IgnoreTrailingSpacesFilter"; }

  void CreateFilter(const leveldb::Slice* keys, int n, std::string* dst) const {
    // Use builtin bloom filter code after removing trailing spaces
    std::vector<leveldb::Slice> trimmed(n);
    for (int i = 0; i < n; i++) {
      trimmed[i] = RemoveTrailingSpaces(keys[i]);
    }
    builtin_policy_->CreateFilter(trimmed.data(), n, dst);
  }
};
```

高级应用也可以提供不使用 bloom filter 的 filter policy，而用其他机制来概括一组键。详情见 `leveldb/filter_policy.h`。

## 校验和

leveldb 会为写入文件系统的所有数据关联校验和。它提供了两处控制点，用来决定对这些校验和进行多“激进”的验证：

`ReadOptions::verify_checksums` 可以设为 true，从而强制对某次读取而从文件系统读取的所有数据做校验和验证。默认不做这种验证。

`Options::paranoid_checks` 可以在打开数据库前设为 true，使得数据库实现一旦检测到内部损坏就尽早报错。取决于数据库的哪一部分被损坏，错误可能在打开数据库时就出现，也可能在后续的数据库操作中出现。默认关闭 paranoid checking，这样即使持久化存储的某些部分损坏，数据库仍可继续使用。

如果数据库已损坏（例如，在开启 paranoid checking 时无法打开），可以使用 `leveldb::RepairDB` 尽可能恢复数据。

## 近似大小

`GetApproximateSizes` 方法可用于获取一个或多个 key 区间所使用的文件系统空间字节数的近似值。

```c++
leveldb::Range ranges[2];
ranges[0] = leveldb::Range("a", "c");
ranges[1] = leveldb::Range("x", "z");
uint64_t sizes[2];
db->GetApproximateSizes(ranges, 2, sizes);
```

上述调用会将 `sizes[0]` 设为 key 区间 `[a..c)` 所使用的文件系统空间字节数近似值，并将 `sizes[1]` 设为 key 区间 `[x..z)` 的近似值。

## Env（环境抽象）

leveldb 实现发出的所有文件操作（以及其他操作系统调用）都会通过一个 `leveldb::Env` 对象路由。更复杂的客户端可能希望提供自己的 Env 实现，以获得更好的控制。例如，一个应用可能希望在文件 IO 路径上引入人工延迟，以限制 leveldb 对系统中其他活动的影响。

```c++
class SlowEnv : public leveldb::Env {
  ... implementation of the Env interface ...
};

SlowEnv env;
leveldb::Options options;
options.env = &env;
Status s = leveldb::DB::Open(options, ...);
```

## 移植

要把 leveldb 移植到新平台，需要提供 `leveldb/port/port.h` 导出的类型/方法/函数的“平台特定”实现。更多细节见 `leveldb/port/port_example.h`。

此外，新平台可能还需要一个新的默认 `leveldb::Env` 实现。可参考 `leveldb/util/env_posix.h`。

## 其他信息

关于 leveldb 实现的更多细节可见以下文档：

1. [实现说明](impl.md)
2. [不可变 Table 文件的格式](table_format.md)
3. [Log 文件的格式](log_format.md)

