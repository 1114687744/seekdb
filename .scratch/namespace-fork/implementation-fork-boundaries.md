# Fork 接口与职责整理

基线：`24c2bfa4a963c2dadf2002dd9cfc0ff46c5cd77c`。四项均为本轮目标；下列完成勾选必须有当前代码和对应验证证据。

## 目标

- [x] 原生 schema 目录只依赖明确的 Namespace、快照/受保护视图、原生事务。SQL 客户端转接集中在调用端，保持每次操作的 RC 视图与 DDL 同事务语义。
- [x] Namespace 删除后的物理清理直接使用原生事务完成排他锁和 DELETE MDS；去掉 Namespace 1 SQL 连接依赖，保留引用检查与并发保护。
- [x] DML 直接复用已准备的写入计划和原生行迭代器，消除重新取 schema、重建计划和 Datum/ObObj 往返。保留事务状态合并、保存点、重复键、LOB 与索引行为。
- [x] 物理接管以物理对象身份、源/目标、快照和保护为依据，去掉任务中的 Namespace 编码和当前逻辑归属查询。支持已删除 Namespace 中仍被后代引用的对象推进、重启恢复和并发回收。
- [x] 定向回归加入四件套并记录结果与失败；生产编译、检查最终差异。

交付：代码、测试和本文一起提交，推送现有 `codex/namespace-worker-proxy-v20` 分支，不创建 PR。

## 验证范围

原生目录冷读取、借用事务提交/回滚、父子 DDL 隔离；DML 首次写入、读自己的写入、保存点及重复键、索引和 LOB；物理删除与后代保留、接管/GC 并发、重启和主备。使用现有定向探针与四件套选择范围，不运行全量 mysqltest/sysbench。每项证据记录实际二进制与命令，不能将旧结果当作本轮通过。

## 进度

2026-10-13：四项代码及定向验证完成。Schema 的逻辑归属仍是 Namespace；本轮整理访问边界及物理对象生命周期，没有把 Namespace schema 改成实例共享目录。

### 已实施的边界

1. `sql_schema_catalog` 接受 SQL 调用方的显式目标及事务，完成转接。`NativeSchemaCatalog` 核心仅接受 Namespace、受保护视图或原生事务；已有 RC 操作快照保持不变。
2. 物理清理在一个 `NativeInnerTable::Transaction` 中，通过 `ObAccessService::lock_obj` 取得物理 tablet 排他锁，再登记 DELETE MDS。锁冲突时整笔回滚，由维护任务下一轮重试。
3. 写入复用 SQL 计划中的 `ObDmlTablePlan` 和原生行迭代器；SQL 计划活到执行结束。保存点回滚之前仍先释放写入上下文，向原生事务归还写状态。
4. 原有物理引用计数及发布/回收互斥移至 `PhysicalTabletAccess` 等存储模块。接管任务以物理源、目标、快照启动，短暂排除回收以取得 SSTable 引用；此后靠文件引用及 tablet 保护完成复制，不再检查 Namespace 当前归属。

### 验证中发现的问题

- 首次编译失败：新 SQL 转接函数在两个旧调用文件中缺少 namespace 限定；已修复，后续生产编译通过。
- `native_dml_boundary_probe.py` 原始用例在保存点回滚后的 UPSERT 返回 4109。缩小到单行普通主键表的 UPSERT，已有 `physical-gc-plan/seekdb-production` 也复现，说明该问题早于本轮修改。延迟取快照优化在 DAS 插入阶段单独取 SCN，未取得同快照的目录保护。改为让 UPSERT 使用已有的语句快照路径；没有查询的普通 INSERT 保留原优化。v3 最小用例及完整 98 行用例（唯一索引、LOB、保存点、重启）均通过。
- `retired_baseline_restart_probe.py` 在 v2 超时：本轮物理清理错误地从进程服务取已经归 Namespace 的锁服务，返回 `OB_NOT_INIT`。改用实际的物理锁接口。v3 暴露物理锁参数 `schema_version=-1` 不合法；修为原 LOCK_ALONE_TABLET 路径使用的 0。最终生产产物已通过中间父空间删除、源表删除、重启及源对象回收验证；两次早期失败均保留。
- 全量注入器的共享事务与创建身份 hook 还引用旧 SQL 物化路径。更新为目前原生事务入口；强制提交 redo 的 hook 也覆盖原生 CREATE MDS，避免测试仅运行未注入的成功路径。
- 创建身份探针仍把测试 collection 写入已经拆成结构化表的目录 store，导致测试初始化 `OB_NOT_INIT`。测试辅助 KV 记录改为写入现有 layout KV store，借用同一物化事务的 `layout_tx`；不增加产品兼容分支。
- `obtest` 的 `basic` 和 `namespace_setup_template_local` 在备库仍进行初始恢复时直接查询，报连接拒绝。用例改为检查 SQL 可服务及 STANDBY 角色；重跑 basic 与 namespace_fork_local 均通过。共享部署目录的 runner 加文件锁，避免并发 gate 覆盖二进制或清理另一组的实例。

### 当前证据

构建日志：`/tmp/seekdb-fork-boundaries-build1.log` 到 `build6.log`；v1 包含前三项，v2 包含四项，v3 包含 UPSERT 和物理锁入口修复。二进制位于 `/data/1/nijia.nj/test/fork-boundaries-artifacts/`。

- v1 启动/fork/写入：`/tmp/seekdb-fork-boundaries-startup1.log`，通过。
- v1 原生 schema DDL：`/tmp/seekdb-fork-boundaries-ddl1.log`，通过。
- v1 DML 失败：`/tmp/seekdb-fork-boundaries-dml1.log`；旧产物最小复现：`/tmp/seekdb-fork-boundaries-upsert-old.log`。
- v3 DML 复测：`/tmp/seekdb-fork-boundaries-upsert3.log`、`/tmp/seekdb-fork-boundaries-dml3.log`，通过。
- v2/v3 中间父空间删除、重启后回收失败：`/tmp/seekdb-fork-boundaries-retired2.log`、`/tmp/seekdb-fork-boundaries-retired3.log`。

四件套新增 `fork-boundaries` 定向 profile，包含上述两个失败场景及其最小复现。结果及复现命令见同目录 `validation-fork-boundaries.json`。这是定向 profile，不代表全量 mysqltest、sysbench 或 `full` profile 已通过。

生产代码的最终编译已完成：`/tmp/seekdb-fork-boundaries-final-build2.log`。重新去掉全部注入后编译的 SHA256 与回归产物相同：`33b175ecd56c8cbd613765ae3f74de11cf23f40e683576ae6ca74bdbb90b47bd`。工作目录的生产二进制也已恢复为该版本。

已有最终产物证据（目录 `.scratch/namespace-fork/gate-results/fork-boundaries/`）：

- `frontdoors/{bootstrap,sql,tls}.log`：三组入口通过。
- `direct/direct.log`：DML、schema DDL/索引/触发器/FK/重启、访问并发、索引、FORK TABLE、物理删除、两种来源的接管后重启、中间父空间删除后接管/回收、direct full 均通过；末尾两个 standby fixture 首次失败。
- `direct-standby-rerun/production-standby/results.json`：修复就绪等待后 basic 与 namespace_fork_local 通过。
- `native/bootstrap-native-kv.log`：主备接管进度错开及两边重启/升主、物理保留边界、过期 GC 计划、零 Namespace 服务装载的冷物化及原生 schema 读取、共享事务回滚/崩溃/并发通过；创建身份探针因上述测试存储入口过期而失败。
- `native-rerun/bootstrap-native-kv.log`：修复探针后，共享事务故障集合、创建身份提交/回滚、强制 redo 后创建及发布阶段崩溃恢复均通过；重启验证以崩溃前原生事务提交记录为权威比对物理创建版本。
- `native-rerun/catalog-standby/results.json`：备库旧目录页删除回放后，原 RR 事务晚打开表仍可读取，`catalog_read_view` 通过。

最终探针二进制 SHA256 为 `d3da086f6d320a11381f154c7de93ef806cf80a5038210ebfbc3b5a48532323a`。探针只用于上述故障验证，其注入代码已从生产源码移除。最后 `git diff --check` 通过。
