# 事务化向量 delta：客户端写入吞吐与真正空闲 CPU

被测原型为 `37a7bc65b`，二进制 `seekdb-delta-perf8`；基线 master `76f871eb5`，
包括原始 192 执行器版及只改为 8 执行器的诊断对照。

## 口径

[上一轮表格](vector-transactional-delta-freshness.md)的写入 P99 是客户端一批 100 行
的 SQL 提交延迟，不是吞吐。上一轮主动限速：4000 档三种实现都约 4003 条/秒，
1000 档都约 1001 条/秒。这个数据只能说明跟上给定速率，不能说明最大写入能力。

本轮客户端吞吐补测使用一个同步连接、每批 100 行、不限速、没有并发 ANN 查询。
计时从开始准备第一批请求到最后一批 INSERT 成功返回，包含客户端编码和往返，
不包含建表、索引初始化、后续等待后台追平。它是本工作负载下的客户端确认吞吐，
不是多连接数据库最大吞吐，也不是新数据进入 ANN 索引的持续处理上限。

数据仍是 Cohere 100k/768 维、cosine、M=16、ef_construction=200；远端
`6.13.50.37`，服务绑定 CPU 48–55，配置 8 CPU/6 GiB，客户端 CPU 56–59。
各实现顺序运行，使用新实例和同一份本地缓存数据，没有重新编译服务端。

## 客户端吞吐结果

| 实现 | 客户端提交条/秒 | 写完 10 万条耗时 | 写完后等待追平 | 提交并追平总耗时 |
|---|---:|---:|---:|---:|
| master，192 执行器 | 9561 | 10.46s | 3.48s | 13.94s |
| master，8 执行器 | 25263 | 3.96s | 8.51s | 12.46s |
| 原型，第 1 轮 | 23622 | 4.23s | 9.00s | 13.23s |
| 原型，第 2 轮 | 23724 | 4.22s | 9.00s | 13.21s |

同为 8 个工作线程的比较中，本轮原型客户端确认吞吐低约 6%–6.5%；它额外维护
delta，前台更快不是已证实的收益。每个主要对照一次，原型重复两次；不是容量
上限或统计显著性测试。两个原型轮次的 CPU 正式计时窗口分别为 30 秒和 10 秒。


INSERT 提交和后台构图可以重叠；客户端拿到成功时，后台仍可能积压。
脚本另调用 `dbms_index_manager.refresh()` 等待消费水位越过已提交数据，并核对
delta 为 0、index_id 为 100000。`write_and_barrier_s` 记录整个批次提交并追平的
经过时间，包含 API 轮询粒度，不是单条可见延迟。

## 空闲时实际执行什么

### 基线

1. ChangeStream 执行工作线程按需启动，没有工作会收缩到 0。
2. `CSDispatcher` 没有待分派事务时等待条件变量，10 秒超时。
3. 没有异步索引时，`CSFetcher` 等 schema 发布通知，10 秒超时；有异步索引但
   日志已追平时，200 ms 后再检查日志。读取位置已经推进，不会从头扫描索引表。

代码见 `ob_change_stream_fetcher.cpp:712`、`ob_change_stream_dispatcher.cpp:342`。

### 当前原型

1. `VecBuild` 构图线程同样收缩到 0。
2. `VecDelta` 协调线程仍存在。无工作时睡 100 ms，然后取快照、遍历 schema、
   针对每个可用异步索引扫描 delta，判断是否有待消费记录。
3. 这个用于发现工作的第一次扫描仍是整个键范围。上一轮缩小的是构图成功之后
   INSERT SELECT/DELETE 的转存范围，没有消除这里的空闲扫描。
4. delta 的 SQL 可见行数为 0，只代表没有可见的待处理记录。在本轮未回收旧键的
   存储状态下，底层仍要遍历历史键并判断删除、可见性；判断结果为空也会消耗 CPU。

代码见 `vector_delta_consumer.cpp:68`、`ob_plugin_vector_index_utils.cpp:761`、
`ob_plugin_vector_index_utils.cpp:1007`。因此，不能用“构图工作线程数为 0”代替
空闲 CPU 测量，也不能用上一轮写完后仍持续 ANN 查询的 drain 阶段当成真正空闲。

## 空闲 CPU 实测

分别测无向量索引、一个空异步索引、写过 10 万条且完全追平的一个异步索引。
每阶段预留 15 秒稳定时间；主要对照的空闲窗口为 30 秒，原型重复轮为 10 秒。
**窗口内关闭全部 SQL 连接，没有
写入、查询或 SQL 监控探针**，仅从 `/proc` 采样进程与线程累计 CPU 时间。

下面百分比以单核为分母：100% 表示占满一个 CPU 核。不是 8 核预算的百分比。
进程数值是完整 seekdb，包括常规后台任务；线程明细单独定位向量维护开销。

| 场景 | master，192 执行器 | master，8 执行器 | 原型，第 1 轮 | 原型，第 2 轮 |
|---|---:|---:|---:|---:|
| 无向量索引 | 0.30% | 0.23% | 0.23% | 0.30% |
| 一个空异步索引 | 0.23% | 0.23% | 0.20% | 0.30% |
| 写过 10 万条、已追平 | 0.30% | 0.33% | 15.40% | 14.50% |

两轮原型中，`VecDelta` 自身分别占 15.06% 和 14.30% 单核 CPU，解释了绝大多数
差值。所有已追平空闲采样中，构图工作线程都是 0。第一轮原型协调线程在 30 秒
中有 255 次自愿上下文切换，符合“扫描花时间，再睡 100 ms”的循环；基线
`CSFetcher` 为 156 次，`CSDispatcher` 为 3 次，符合各自等待周期。

因此，原型约消耗 0.145–0.154 个 CPU 核，相当于这次 8 核配置的约 1.8%–1.9%。
这是一个曾有 10 万条记录的索引，不是整个实例所有场景的固定开销。


线程 CPU 使用 Linux 计时刻度；显示为 0 只能说明低于该窗口的测量分辨率。
上下文切换次数用于交叉核对唤醒周期，不等于每次切换都执行了完整消费轮次。

### 调用栈证据

第二轮原型在 CPU 正式计时之后，对 `VecDelta` 单线程采样 15 秒，取得 435 个
用户态 CPU 样本、无丢样。主要栈为：

```text
VectorDeltaConsumer::run1
  → consume_round → consume_index
  → append_delta_from_table
  → check_delta_buffer_table_readnext_status
  → ObTableScanIterator / ObMultipleScanMerge
  → ObMemtableScanIterator / ObMvccRowIterator
  → ObMvccValueIterator::lock_for_read_、B-tree 遍历、行解码与可见性判断
```

`lock_for_read_` 自身约占采样 CPU 的 18.86%，B-tree ScanHandle::get 为 7.50%，
ObReadRow::iterate_row 为 7.42%。这些是采样线程运行中的 CPU 占比，不是整个核
的利用率，也不能把 `lock_for_read_` 的函数名解释成发生了 18.86% 锁等待。

[完整采样报告](vector-transactional-delta-results/idle-throughput/idledeltaprofile/perf-report.txt)
确认耗时来自发现待处理记录时的底层扫描。结合 delta=0、无客户端读写和线程已
收缩的证据，可以排除“还有构图积压所以不算空闲”这个解释。


## 结论与后续

客户端吞吐要区分限速混合读写测试与不限速单客户端纯写测试。当前原型没有证明
比同为 8 执行器的基线写入更快，多维护 delta 的成本仍然存在。

**当前原型存在已复现的空闲 CPU 回退。线程池复用没有解决协调线程反复扫描
已清空 delta 的问题。** 在已有大量历史记录的索引上，应先做到没有新增提交时
不反复扫描，再评估多索引场景。不能只拉长轮询间隔掩盖成本，否则会重新拉大
新数据可见延迟；应评估提交变化通知/变化检测与持久 delta 的配合，持久 delta
继续作为待处理工作的依据。这是后续实现工作，本轮保留原二进制完成测量和定位。

本轮没有测多索引数量或历史记录规模曲线，不能由单索引数字直接线性外推。

后续讨论收敛到[复用日志发现变化的设计提案](vector-transactional-delta-log-discovery.md)，
评估避免新增前台共享通知名单的方式。该方案尚未实现，不能将此页实测视为其效果。

## 复现与证据

```sh
PYTHONPATH=<root>/python OPENBLAS_NUM_THREADS=1 OMP_NUM_THREADS=1 \
taskset -c 56-59 python3.8 <root>/scripts/idle_throughput.py \
  --root <root> --label <new_label> --binary seekdb-delta-perf8
```

`<root>` 为 `/data/1/nijia.nj/test/vector-delta-perf-20261015`。基线二进制分别为
`seekdb-master-perf`、`seekdb-master-eight-workers`。每次 label 必须新建。
脚本支持 `--profile-loaded`：CPU 正式计时结束后才采样协调线程调用栈，避免把
profiler 开销计入空闲数字。

第一次 `idlebase8` 使用了错误的等待接口 `dbms_vector.refresh_index`，后续检查
发现 index_id 仅有 41100 条，脚本断言失败并停止实例，未产生已追平空闲结果。
正式轮次改用 `dbms_index_manager.refresh()`，并核对持久记录计数。

证据保存在 [idle-throughput](vector-transactional-delta-results/idle-throughput/)；
包括每批写入时间、逐线程采样、原始文件哈希、二进制身份及未完成的首次试验。
