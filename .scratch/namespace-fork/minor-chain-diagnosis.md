# Minor 缺少 Last：fork 快照行被保存为孤立 shadow

日期：2026-10-13。定位基线：`d48bbccd0444c888c77daaeb373b4e3a374b3829`。

2026-10-13 用户要求：1210 降为低优先级 TODO，暂不处理；重点定位 minor 异常。诊断提交 `b2ca9c0a7` 只包含诊断、复现和四件套入口，临时修改从生产源码撤除。

**2026-10-14 更新：用户授权修复后，已完成生产入口修复和定向验证。** 这是快照行到持久化行的格式转换缺陷，不要求重新设计 Namespace/fork 架构。生产实现与最终结果见本文末章；以下保留诊断过程。

## 结论

不是备库专有问题。在单进程主库、不部署备库、不执行 major freeze 的条件下，可以稳定复现同样的 `-4016 / meet another rowkey but not meet Last flag`。

根因是 **fork 接管重写 SSTable 时，没有把快照查询返回的行转换成正确的持久化多版本行格式**。查询扫描器可以返回带 shadow 标记的合成行。`ObForkSnapshotRowScan::get_next_row()` 对返回值补 First/Last，却保留 shadow 标记和 shadow 专用 SQL sequence。于是新文件里出现单独一条 `First + Shadow + Last`，没有后续的正式版本行。

minor 合并器依赖 shadow 后面还有同一行的正式版本记录。它跳过 shadow 后，提前读到下一行，最终由 writer 的行链校验报错。不能通过去掉 writer 校验或增加 Namespace 特判修复。

这次有问题的是子空间的 core tablet；不是表定义、`ObStorageSchema` 正文或原生 schema 目录读写导致的行格式问题。

## 复现缩减

原入口为 `run_standby_suite.py --case schema_history`。当前代码上，完整用例虽 PASS（375.58 秒），主库仍发生 43 次 Last 异常，备库未发生。因此原来的“最终 major 和查询成功”断言不足以检查 minor 正确性。

已去掉备库、major、预先创建并删除的 Namespace、本地索引和全局索引；保留下面的独立主库场景仍失败：

1. 自动 bootstrap，合并调度周期设为 3 秒。
2. 等待 10 秒，使父 core 行积累多个提交版本。
3. 创建两分区表并写入三行；fork 子 Namespace。
4. 父、子分别 ADD COLUMN；子更新三行。
5. 执行 minor freeze，观察子 core tablet 的 minor。

命令：

```bash
python3 .scratch/namespace-fork/minor_last_flag_probe.py \
  --binary build_release/src/observer/seekdb \
  --pre-fork-wait 10 --skip-major --skip-seed --skip-indexes --seconds 30
```

该命令在生产基线上已实际执行并非零退出。断言直接检查 Last 异常；成功还必须看到目标 tablet 的 minor DAG 完成，以及父子数据符合预期。不能把“日志没有报错但目标 minor 未执行”算成功。

更小的仅建库/fork/子建库试验没有复现；不据此断言其他 DDL 安全。完整的逐项实验参数、日志、实例位置和二进制 SHA256 见 [results.json](minor-chain-diagnosis/results.json)。

## 逐行证据

原始证据：[row-chain-evidence.txt](minor-chain-diagnosis/row-chain-evidence.txt)。单次失败 trace：`YB427F000001-00065DBA8B7F9E83-0-0`。

目标：Namespace 3 的逻辑 core tablet 1，物理 tablet `4611686430744248321`。行键为 `__all_global_stat / 1 / change_stream_min_dep_lsn`。

| 位置 | 提交版本 | First | Shadow | Last | SQL sequence |
| --- | --- | --- | --- | --- | --- |
| fork 查询扫描器返回 | 1791905710534935032 | 1 | 1 | 0 | -9223372036854775807 |
| 子 SSTable 被 minor 读入 | 同上 | 1 | 1 | 1 | 同上 |
| 另一旧文件中的正式版本 | 1791905637555838006 | 0 | 0 | 1 | 0 |

fork SCN 为 `1791905710605374022`，该行可见。问题发生在保存查询结果时，不是把不可见的新版本误读进来。

后续过程：

1. `merge_same_rowkey_iters()` 同时看到新文件里的孤立 shadow 和旧文件里的正式版本。
2. 合并器产生一条新的汇总 shadow，暂不带 Last，准备继续输出正式版本。
3. `skip_shadow_row()` 推进新文件迭代器；新文件中下一条已经是 `normal_schema_version`。
4. 旧文件迭代器仍停在 `change_stream_min_dep_lsn`，同一个“相同行键”的合并循环里却混入另一个行键。
5. writer 检出前一行尚未闭合，返回 `-4016`。

代码入口：

- `src/storage/ddl/ob_tablet_fork_task.cpp`：`ObForkSnapshotRowScan::construct_access_ctx()` / `get_next_row()` / `ObTabletForkRewriteTask::process_rewrite_sstable_task()`。
- `src/storage/access/ob_sstable_row_whole_scanner.cpp`：`init_micro_scanner()` 依据 query flag 选择查询扫描器；当前 fork 路径没有选择 minor 合并扫描器。
- `src/storage/compaction/ob_partition_merger.cpp`：`merge_same_rowkey_iters()` / `skip_shadow_row()`。
- `src/storage/blocksstable/ob_macro_block_writer.cpp`：`check_order()` 是检测到错误的位置。

## 历史来源与范围

`git blame` 指向 `15a5fe4002`：为 fork 扫描结果强制补 First/Last 的代码在 Namespace fork 前的基线 `dbe8fcdbb2ed1ea97a12d781ff0ffad8638e01b8` 中已经存在。

这证明有问题的转换逻辑早于这轮 Namespace/schema 重构；**未在该旧版本上动态复现本次 minor，不能把代码历史证据写成旧版本测试通过或失败**。Namespace fork 接管 core tablet 会执行这条通用 fork 重写路径，使频繁更新的 core 行暴露了问题。普通用户表的 fork 也共享这个入口，正式修复须覆盖它。

## 单点实验

在上述入口、补 First/Last 之前，仅对返回的 shadow 快照行做以下转换：

```cpp
if (row->is_shadow_row()) {
  ObDatumRow *mutable_row = const_cast<ObDatumRow *>(row);
  mutable_row->mvcc_row_flag_.set_shadow_row(false);
  mutable_row->storage_datums_[trans_idx + 1].set_int(0);
}
```

这是诊断实验，尚不是正式补丁。提交版本、行值、快照边界以及 minor 算法均不变。

相同参数的 A/B 结果：

| 二进制 | Last 异常 | 目标 minor 完成 | 父子数据 |
| --- | --- | --- | --- |
| 原生产版本 | 1（首次捕获后结束） | 0 | 用例在错误断言处失败 |
| 仅修正格式的实验版 | 0 | 1 | 全部符合预期 |

日志：`/tmp/ns-minor-ab-original.log`、`/tmp/ns-minor-ab-normalized.log`。诊断插桩版本身也复现了相同错误，见 `/tmp/ns-minor-chain-diagnostic.log`。

正式修复方向：在 fork 重写的边界统一定义“快照结果行 → 可持久化版本行”的转换，保留版本和值，正确处理 shadow/隐藏 sequence/First/Last，并验证 NOP、删除行、跨微块行链以及普通用户表 fork。不要改 minor 来接受孤立 shadow，也不要引入 ns_id 特判。

## 2026-10-13 四件套诊断结果

`minor_last_flag_probe.py` 已进入 `run_four_gates.py` 的 full/direct，并提供 `--profile minor-chain --gate direct` 定向入口。生产基线预期为红；正式修复后须把它跑绿。实验版通过不等于生产问题已修复。

四件套实际验证：`/tmp/ns-minor-four-gates-ab/direct.log` 通过，目标 minor 完成 1 次；撤除临时修改、重新编译后的生产版本在 `/tmp/ns-minor-four-gates-production/direct.log` 再次因同一 Last 异常失败。清理后编译日志 `/tmp/ns-minor-diagnosis-clean-build.log` 通过，`git diff -- src` 为空；没有把实验补丁留在生产源码中。

1210：用户指定基线已得到相同的“两次无默认库失败、两次 USE 后成功”，按用户要求保留为低优先级 TODO，不在本轮处理。

## 2026-10-14 生产修复与验证

`ObForkSnapshotRowScan::get_next_row()` 统一将可见快照结果转成单条正式版本行：

- 若结果带 shadow，清除 shadow 标记，用既有多版本行键 helper 定位隐藏 SQL sequence 并置为 0。
- 设置 First/Last；去掉把正常查询折叠结果报告为异常的告警。
- 保留提交版本、DML 类型、NULL/NOP、compacted 标记和所有用户列值，以便继续与较早文件融合。
- 不增加 Namespace 分支、缓存或持久状态，不修改 minor 或 writer 的校验。

生产编译 `/tmp/ns-minor-fix-build.log` 通过；没有诊断插桩。验证记录见 [fix-results.json](minor-chain-diagnosis/fix-results.json)。

| 验证 | 结果与证据 |
| --- | --- |
| 原单主库 Last 复现 | 通过；目标 core tablet minor 完成、无 Last 异常，父子数据正确。`/tmp/ns-minor-fix-gates/direct.log` 中 `minor_last_flag` PASS |
| 普通 FORK TABLE | 子空间自有表、继承来源、自动编号及重启读取通过。`/tmp/ns-minor-fix-gates-resume/direct.log` |
| 多版本用户表重写 | 96 次更新，同一行版本链所在输入 SSTable 含 106 条物理记录、6 个微块；实际执行目标用户 SSTable rewrite 与 minor。部分列更新、NULL、删除、fork 后提交不可见及崩溃重启的数据断言均通过。`/tmp/ns-minor-fix-snapshot-rows-final.log` |
| 最初 schema_history 主备场景 | 384.93 秒通过；主库/备库 major、12 个用户/索引 tablet、备库崩溃恢复、提升后 DDL/major、历史索引 checksum 通过。新增全程 Last 日志断言，主备均为 0。`/tmp/ns-minor-fix-standby/results.json` |

新增 `fork_snapshot_row_probe.py` 和原 Last 定向测试、普通 FORK TABLE 测试均纳入四件套 `minor-chain` profile；原 `schema_history` 场景现检查 Last 异常，不能再用最终 major 成功掩盖中间 minor 失败。

测试过程保留：新增用户表用例首轮在“文件 end_log_scn 必须达到后来取得的 current_scn()”这一错误等待条件上超时，实际 minor 已完成。改为检查目标 minor 成功记录、目标 SSTable rewrite 和最终数据；移除恒为 0 的虚拟 `ref_tablet_id` 等待条件。首轮日志 `/tmp/ns-minor-fix-gates/direct.log` 保留，修正后分段通过和最终长版本链通过分别在上述日志中。不是单次 full 四件套全绿；未跑全量 mysqltest/sysbench。

1210 继续保留为低优先级 TODO，未修改。
