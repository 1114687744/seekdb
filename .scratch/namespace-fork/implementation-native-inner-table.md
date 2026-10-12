# 原生内表访问层：现有 KV 接入

## 范围

本轮实现通用原生行访问，并将现有 `InstanceMetaStore` 的全部读写、事务、快照操作接入。使用者包括实例目录 KV 和现有物理布局 KV。Schema 的 SQL 读写改造留在后续任务。

## 实现

- `storage/access/native_inner_table.{h,cpp}`：`NativeInnerTable`，显式接收物理 tablet 和固定表定义，准备存储读写描述；封装 AccessService、原生事务、读者登记、快照句柄和迭代器。
- `storage/instance_meta/instance_meta_store.{h,cpp}`：把 collection/key/value 转成三列行、把集合内键范围转成联合主键范围。现有调用接口保持可用，所有原生访问经过新模块。
- 目录页 GC 的短时排他协调仍由 KV 目录适配层承担，覆盖入场、获取快照、提交/回滚和借用事务的 detach。通用模块没有目录页、集合、Namespace 或 GC 排他分支。
- 既有 compaction 调用 `InstanceMetaStore::min_retained_snapshot()`，该调用转发到通用模块的同一读者登记。新模块的实例必须覆盖其事务生命周期；将来接入其他物理表也必须接入其保留边界。

## 本轮实际接口

`init(tablet, schema)` 绑定一个固定布局的内表。行按主键顺序排列主键列，其后是其余列；调用者无需把键编码为不透明 bytes。

- `get`：完整主键点查，结果深拷贝到调用者分配器。
- `scan`：主键范围正向/反向扫描；MIN/MAX 表示前缀边界；回调可提前停止。回调里的行和变长字段仅在回调期间有效。
- `insert`、`put`：分别是唯一插入和完整行覆盖。
- `get_for_update`、`erase`：锁定读取当前值，再执行读改写/删除；只重试锁获取，遵守原截止时间。
- `begin`、`begin_read`、`begin_weak_read`：固定提交快照，普通读取还可见本事务此前的写入。自定义快照必须是新取得或已有保留者保护的时间戳。
- `attach`：加入已有原生事务，支持两个内表共用一个提交。借用者不能提交/回滚，所有者结束后借用者 detach；detach 不解引用已释放的原生描述符。
- `retain_snapshot`、`min_retained_snapshot`：独立句柄延续 MVCC 保留；登记在获取快照前建立，提交结束后才移除。

当前覆盖整数、二进制字符串、非主键可空列及升序主键。完整行由调用者提供。本轮不实现 SQL 表达式、自动默认值、LOB、索引维护、分区路由、批量语句、部分列更新；初始化拒绝依赖未支持行为的表定义。现有 KV 只需要本轮支持的能力。

## 验证

测试源在现有 `instance_meta_native_probe.ipp` 中扩展，原生执行器要求新 PASS 标记存在：

1. 获取快照回调失败后，事务登记和目录准入均释放；随后能进入 GC。
2. 扫描回调报错、提前停止均释放迭代器，事务仍可继续读写；扫描期间拒绝重入结束事务。
3. 两个 KV tablet 加入同一原生事务，验证共同提交/回滚及所有者释放后安全 detach。

复用原有重复键、二进制边界、连续 put/put/scan、锁等待、读自身写入、固定快照、转储/minor、目录树并发 GC、SQL 共用事务和强制退出恢复用例。

四件套增加显式范围 `--profile native-inner-table`，默认 full 范围保持原有用例集合。该范围覆盖原生 KV、共享事务故障、bootstrap、SQL、direct、TLS、目录 GC、SQL 读视图以及备库 basic、Namespace fork 和目录历史快照。它不是完整 mysqltest、sysbench，也不等同于 full 四件套全部历史用例。

```sh
python3 .scratch/namespace-fork/run_four_gates.py \
  --profile native-inner-table \
  --binary /path/to/seekdb-production \
  --native-probe-binary /path/to/seekdb-native \
  --log-dir /path/to/results
```

最终状态和对应证据见 `gate-results/native-inner-table/results.json`。这是按失败步骤续跑后汇总的证据；不是一次完整串行运行全部成功。

### 最终验证结果

- Release 编译通过；临时钩子已移除，生产二进制没有原生探针标记。
- 原生 KV 的四阶段恢复及三次强制退出、跨两个物理 KV 的共同提交/回滚、扫描和失败清理通过。
- SQL 共用事务故障恢复、目录 GC、SQL 固定视图通过。
- bootstrap、SQL、完整 direct、TLS 用例通过；备库 basic、Namespace 追随和旧目录页删除后的 RR 延迟读取通过。
- 有失败步骤续跑及独立旧缺陷复现，详见下文；不宣称全历史四件套或全量模块检查通过。

生产代码提交：`d89cb45be`。

## 过程中的失败

首次启动在 AccessService 初始化时返回 `OB_NOT_SUPPORTED`。新增类型检查错误地用 `is_varchar()` 检查二进制字符串；该谓词明确排除 VARBINARY。改为 `is_varbinary()`。同一启动路径随后通过原生 KV 和生产 bootstrap 回归。

## 既有 TODO

Storage 头文件归属完整检查在本次变更前已有清单缺项、归属不一致和直接 SQL include。此次增加新头文件的 closure 登记及预期增量；对照 HEAD 的 include 图核实既有差异没有增加。详情：`gate-results/native-inner-table/header-ownership-delta.json`。该 TODO 不阻塞本次 release 构建及 KV 运行验证，不宣称全量模块归属检查已通过。


### 旧测试夹具修复

- direct 的 checksum 故障夹具仍沿用缺少 `namespace_id` 的 SQLite INSERT OR IGNORE，原有 NOT NULL 约束导致实际插入零行。补全夹具归属并改用 INSERT、断言插入数；同一份归档数据库中复现旧夹具零行、新夹具两行。随后完整 direct 用例通过。
- 备库 basic 的框架 `reboot` 在固定等待结束后就创建 admin 用户，一次遇到初始化尚未完成而登录失败。展开该测试的首次启动，并复用既有 Namespace 备库测试的 30 秒启动等待。尝试框架 SQL 就绪轮询时发现其固定使用尚未创建的 admin，故没有采用该轮询。这个测试仍使用有界固定等待；没有修改产品启动流程。

## TODO：目录加锁读取与普通读快照不一致

本轮 direct 续跑出现一次 `CREATE TABLE phase10.blobs ...` 的目录发布 `OB_ENTRY_NOT_EXIST(-4018)`。沿目录发布路径做了确定性复现，并将 KV 两个实现文件换回基线 `194d1454097432539b8dade3d5ecbe89f82062bf` 编译对照，两者都失败：

1. A 开始事务，固定普通读快照 S。
2. B 发布一组新目录页及新目录根并提交。
3. A 加锁读取 Namespace，拿到 B 的新根。
4. A 通过普通读取跟随新根，新页在 S 时尚不存在，返回 -4018；`stage_catalog_delta` 同样失败。

`catalog_locked_root_probe.ipp` 直接调用实际 `InstanceNamespaceMetadata::stage_catalog_delta`，不依赖调度时机。对照分别得到相同页 ID `11733487962978548053`、`page_ret=-4018`、`stage_ret=-4018`。这证明了改动前已有该缺陷；当次线上式 DDL 日志只记录发布失败，不能凭它单独唯一判定内部哪次读取失败。

修复方向：目录写入者应在取得根锁后，使用与该根一致的读取视图访问其页面；须同时审计 DDL、物化及其他目录写入者。不要在通用 `get_for_update` 中暗中改变普通读的固定快照语义。该问题单列后续修复，不阻塞这次保持既有语义的 KV 抽层；不能据本轮通过的 direct 续跑宣称并发目录发布已修复。

复现命令（预期失败，保留真实非零退出码）：

```sh
python3 .scratch/namespace-fork/run_instance_meta_native_probe.py \
  --binary /path/to/seekdb-native --locked-root-regression
```

测试构建须先执行 `gate_probe_injection.py enable`，该复现入口由四件套共用的原生执行器提供。默认正常回归不吞掉这一失败，也不把它计入 PASS；显式选项用于后续修复验证。普通 direct 门禁仍包含触发过失败的完整 DDL 流程。
