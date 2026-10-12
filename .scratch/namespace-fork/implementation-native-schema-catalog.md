# Namespace 原生 schema 目录

状态：实施中；基线 `42d56a8ad`；原生读取和物化进展代码已提交并推送 `908aa69b7`。表/列原生写入代码已提交 `4d5f310a2`。本文记录本轮完整目标和验证证据，未勾选项不表示完成。

最新进度：完整定义装载已提交 `72a71854d`；简单定义全量/增量刷新已提交 `7c976ece1`；版本历史、最新版本、原始创建版本查询已提交 `7e7c2d73f`；核心定义读取已提交 `b9b7f77ee`；当前版本和索引列表已提交 `77d3ae27b`；名称、辅助表及 DDL 发布定义读取已提交 `4c517d04d`。mock FK 父表定义装载也已接入，通过冷读取、生产 DDL/重启和 Namespace 主备验证。部分字段及表/列更新已提交并推送 `9829966a8`。表/列删除已提交并推送 `993133499`；约束完整写入已提交并推送 `bb5b619d7`。外键及占位父表完整写入已通过冷读取、生产 DDL/重启及主备回放/提升验证。分区及其余关联入口仍未完成；下文各阶段的“待接入”描述按时间记录，不代表最新状态。

## 目标与边界

将完整表定义的持久读写接到通用原生内表访问层。现有当前表/history 表仍是权威数据，仍归属 Namespace 并按 fork 快照继承。实例 NAMESPACES/PAGES 只负责目录根及物理定位。合并、checksum 接入和文件持有完整描述属于后续阶段。

调用方显式提供 Namespace、事务及受保护的目录视图。目录操作上下文只活到本次读取或 DDL 结束；不得把归属藏进 TLS、全局指针或默认 Namespace。Namespace 解析在目录层完成，NativeInnerTable/AccessService 只接收物理目标、固定结构和快照。

固定内表结构和逻辑 tablet ID 复用代码生成的 ObInnerTableSchema。实例目录使用自己的代码定义和固定物理地址。因此读取 schema 内表的定义、路由和历史保护不得递归调用 SchemaService，也不得装载完整 Namespace runtime。

## 完成清单

- [x] 原生访问支持 schema 内表实际使用的标量、字符、时间与 LOB 存储格式；复用原有存储实现。
- [x] Namespace 目录读上下文：同一事务视图、显式物理来源与继承快照上限、正确释放目录与物理访问保护；冷读取及借用事务定向验证通过。
- [ ] 写入使用所属 Namespace 物理 tablet；物化、DDL 事务借用和提交保护闭合。
- [ ] 表、列、分区、索引/辅助表关系、约束、外键等完整定义的当前表/history 写入接入。
- [ ] SchemaService 对应装载、历史选择及 DDL 内部回读接入；避免表/列/分区拼接不同视图。
- [ ] SCN 可见性与 schema_version 选择分别表达；旧视图有明确持有者和保留保护。
- [ ] 无 Namespace runtime 的冷读取、父子 DDL 隔离、物化、并发、回滚、重启及备库验证。
- [ ] 失败用例与定向验证加入四件套；编译生产版本；提交并推送分支，不创建 PR。

## 已核对的实现约束

1. NativeInnerTable 原先只接受整数和二进制列，且只扫描绑定的物理 tablet。schema 内表还包含 varchar、timestamp、longtext；不能改内表格式来迁就访问层。
2. ObLSTabletService::put_rows 不支持 LOB。LOB 更新应使用既有 insert/update/delete 链路，不能把正文当作定位器直接写入。
3. TabletAccess::prepare_read 已使用实例目录解析来源；它本身不从 SchemaService 获取表定义。旧 worker_local_table_schema 仍访问运行时 schema cache，不能用作新目录的定义来源。
4. ensure_tablet 的创建描述来自目录。物化已改由原生目录事务托管 CREATE MDS、布局参与者及来源根，移除内部 SQL 连接；冷物化回归通过。DDL 目录写入仍须显式接入其所属事务。
5. history 读取以 schema_version 选择逻辑历史，同时受事务快照约束；schema_version 不是提交 SCN。

## 验证证据

- 原生表/列 history 冷读取通过：`/tmp/native-schema-catalog-cold1.log`。孩子首次登录前读取父空间继承记录，全部 runtime 槽位仍为空。
- 流式游标及完整表定义基本读取通过：`/tmp/native-schema-catalog-full1.log`；测试日志包含 `NATIVE_SCHEMA_CATALOG_FULL ... ret=0`。覆盖中文默认值还原、拒绝同事务重入、拒绝游标未关闭时结束目录操作、关闭后继续读取。
- 上述验证使用同分支测试钩子版本，这些早期验证尚未替换生产调用点；后续生产接入见文末，不能据此宣称完整目标完成。
- `native_schema_catalog_probe.py` 和注入脚本已加入四件套入口。分区/索引/约束/外键/生成列/10KB 默认值扩展用例已通过：`/tmp/native-schema-catalog-cases1.log`。

## 当前实现与后续接入注意事项

- 增加 NativeInnerTable 流式 Cursor；范围和变长 key 由游标持有，行内容仅在下一次 next 前有效。原有 callback 扫描使用同一实现。
- Namespace 原生目录游标持有目录视图、物理访问保护和完整操作上下文；使用现有模板化 `ObSchemaRetrieveUtils` 解码，无须 SQL 结果适配层。
- 完整表读取已实现各组成部分，并接到 TRUNCATE 完整定义回读入口；SchemaService 装载及其他 DDL 回读尚未接入。辅助表关系当前遍历 table history；批量装载需要一次收集整批关系，避免每张表重复全扫描。
- 固定 `__all_table`/history 有 `idx_data_table_id` 等真实索引。写入接入必须一起维护这些现有索引，不能只写主 tablet 导致后续 SQL 查询索引过期。
- 原生 insert/update/erase 已复用存储引擎的 LOB 协议；update/erase 的旧行保留存储定位器，普通读取返回正文。`/tmp/native-schema-catalog-lob2.log` 及对应实例日志 `NATIVE_SCHEMA_CATALOG_LOB_DML ... stage=erase_committed ret=0` 验证大 VARCHAR 的事务内插入/更新可读、回滚、提交、删除回滚及删除提交。复核固定定义后确认 `cur_default_value_v2` 是 VARCHAR，不能把这一证据计为 LOB 验证；测试已改为真正的 LONGTEXT `__all_table_history.view_definition`，90KB/120KB 正文；`/tmp/native-schema-catalog-longtext1.log` 及实例日志 `NATIVE_SCHEMA_CATALOG_LOB_DML namespace=4 stage=erase_committed ret=0` 已通过，代码同时断言了目标列确为 LOB 存储类型。
- LOB 用例曾在“回滚后立即用新事务再次插入”返回标准 `OB_TRY_LOCK_ROW_CONFLICT`，见 `/tmp/native-schema-catalog-lob1.log`。测试按整事务重试处理异步锁清理；保留该用例在四件套中。`put_rows` 仍不支持 LOB，目录写入须使用明确的 insert/update，不能继续调用盲写 put。
- 流式化后的旧原生内表回归第一次只启用 native_probe，漏启 `catalog_gc_boundary_injection.py`，预期 GC 并发回调没有执行，断言失败；已确认测试构建缺少钩子，补齐后 `/tmp/native-schema-catalog-native-regression2.log` 通过全部四个阶段，含三次崩溃重启、snapshot across minor、跨表事务、锁等待、游标释放和重入检查。
- 历史版本读取：`/tmp/native-schema-catalog-history1.log` 的 history 子用例通过；在新增列后按此前 V 仍得到两列定义。dropped 子用例初次把用户可见列数当作全部存储列数，断言失败；SQL 权威记录显示 `id/v/SYS_C00018_...` 共三列，第三列 is_hidden=1。修正为验证完整存储列数且原名称 n 不存在后，`/tmp/native-schema-catalog-longtext1.log` 的 dropped 子用例通过。

## 原生事务与生产回读接入进展

- 二级分区模板、删除索引后历史关系读取通过：`/tmp/native-schema-catalog-shapes1.log`，两个孩子的 runtime 槽位均为空。
- `ObTabletCreator` 不再在构造时保存 SQL 事务引用；执行时显式传入 SQL owner，或原生 owner 与布局参与者。共用创建批次与 MDS 实现。
- `ensure_tablet` 由目录原生事务直接托管 CREATE MDS、布局元数据和来源映射；布局参与者保留到 owner 提交或回滚后再释放。`/tmp/native-schema-catalog-cold-materialization1.log` 通过主/LOB 原子创建、冷服务槽位、错误来源代次拒绝以及后续 12KB/14KB LOB 读写。
- 新增 `NativeSchemaCatalog::attach` 借用 DDL 原生事务；独立操作仍通过 begin_read 启动只读事务。借用方持有目录视图直到 owner 结束，close 只 detach。
- `get_full_table_schema_from_inner_table`（TRUNCATE 的完整定义回读）已改为原生目录，Namespace 来自明确的 trans.target_namespace；目录保留到 DDL 事务结束。生产调用链与未提交修改可见性已定向验证通过。其余 SchemaService 批量装载、普通 DDL 回读及写入接入尚未完成。

- 借用原生事务读取：`/tmp/native-schema-catalog-borrowed1.log`；对应实例日志 `NATIVE_SCHEMA_CATALOG_BORROWED namespace=3 stage=read_rollback ret=0`。孩子冷物化 `__all_table_history`，事务内更新表名后借用目录读到新名，独立目录仍看到旧名，回滚后恢复；所有 Namespace 服务槽位仍为空。该回滚测试仅改变 table_name，不改变 idx_data_table_id 的键；完整生产写入与索引维护仍待接入。
- 生产 DDL 完整回读：`/tmp/native-schema-catalog-truncate1.log`。父空间元数据 DDL 后 fork 孩子，再 TRUNCATE 父表，孩子继续读到原行。实例日志明确到达 `new_truncate_table ... truncate cost after get schema ... ret=0` 与 `finish new truncate table ddl(ret=0)`，证明实际走过已替换的完整定义入口。

- 移除所有测试注入后的生产编译通过：`/tmp/native-schema-catalog-production-build1.log`。生产二进制再次通过父空间 TRUNCATE/孩子旧快照回归：`/tmp/native-schema-catalog-production-truncate1.log`。
- 生产备库基本用例通过（118.78 秒）：`.scratch/namespace-fork/gate-results/native-schema-catalog/production-standby/basic.log`，覆盖备库创建、切换及故障提升。
- 生产 Namespace 主备跟随通过（116.17 秒）：同目录 `namespace_fork_local.log`，覆盖首次装载与回放竞争、主/LOB 物化事务回放、孩子 DDL 刷新、父 TRUNCATE/DROP 后历史来源、删除准入、备库提升后的 schema 恢复和写入，以及提升后再次 fork。结果汇总 `results.json` 中两个用例 exit_code 均为 0。

## 继续接入的具体入口

1. **写入行生成**：表/列的字段生成已抽为 `SchemaCatalogFields`，原生行和 SQL splicer 共用字段映射；`typed` 用例已验证 NULL/空串、unsigned、enum、中文默认值及名称索引。分区、约束/FK 等字段生成仍待按同一方式接入。
2. **写入执行**：`batch_add_table_for_create_table/batch_add_columns_for_create_table` 已接同一 DDL owner 的当前表/history 写入；`add_table/add_single_column/add_columns_for_not_core` 也已改造，最新增量已通过下述生产定向回归。保留初始化 bootstrap 与 core 特有表示的既有入口。ALTER 当前行更新、删除、分区、约束/FK 等仍待覆盖；直接 `sql_client.write` 的 UPDATE/INSERT SELECT/DELETE 也必须接入。
3. **真实索引**：固定定义生成器已导出物理索引与归属，原生 writer 同事务维护普通非唯一内表索引；`indexed`、`typed` 和客户端 FORCE INDEX 查询已通过。辅助关系读取仍待使用索引或批量收集，避免每表完整扫描。
4. **剩余读取**：普通 `get_table_schema_from_inner_table` 已借用 DDL owner 接入；`get_not_core_table_schema/get_not_core_table_schemas` 及 SchemaService 其他批量入口仍待迁移。批量读取需要同一受保护 SCN；FK/trigger/aux 关系应使用已有索引或按一批收集，避免对每个表全扫描。
5. **边界**：当前完整 DDL 回读入口明确要求 active strong transaction。弱读与旧 SCN 不能只把 snapshot_timestamp 数字塞进去；必须传入已受保护的目录 view，或在保护期间获取可读 weak SCN。bootstrap/core 元数据入口要显式处理其固定定义及初始化顺序，不以 Namespace 1 回退。

以上五项的剩余部分仍是本轮完整目标的必做项，不能以已通过的部分读写测试替代。

## 当前表与索引写入接入（实施中）

- 固定定义生成器已导出内表的物理索引定义与归属清单；目录 writer 在同一事务维护主表和索引，写前准备全部目标，拒绝未支持的索引类型。
- 原 SQL 字段编码已抽到 `SchemaCatalogFields`，SQL splicer 和原生 `SchemaCatalogRow` 共用字段生成，避免维护两份字段映射。原生 sink 处理固定列顺序、默认值、NULL/空串、整数类型和变长数据所有权。生产 CREATE 和表/列插入入口已接入，详见后续验证；其余更新/删除仍待接入。
- `/tmp/native-schema-catalog-indexed1.log` 暴露普通非唯一索引的删除路径错误：目录在主表行锁之外，再通过 `erase` 给索引加锁，被 `ObMemtable::lock` 返回 `OB_NOT_SUPPORTED`。修正应沿用引擎的主表锁保护规则，直接删除旧主表行投影出的索引项，不修改引擎的锁规则。失败场景保留在 `indexed` 用例：90KB/120KB LONGTEXT、索引键更新、独立读、提交和回滚。修正已通过后述 indexed 回归。
- 新增 `typed` 用例准备验证完整 table/column 编码回读、真实名称索引、中文/NULL/空串/unsigned/enum 默认值及提交回滚，已通过后述 typed 回归。

### 已验证进展

- 索引修正已通过：`/tmp/native-schema-catalog-indexed3.log`，`indexed` 测试最终 `erase_committed ret=0`。普通索引删除使用主行保护下的 `erase_index_row`，不再给非唯一索引单独加锁。
- `/tmp/native-schema-catalog-indexed2.log` 的 `typed` 初次失败在名称索引读取：测试传入的 varchar 查询键没有采用固定定义的 collation，严格原生接口返回 `OB_INVALID_ARGUMENT`。补齐测试键类型后 `/tmp/native-schema-catalog-typed1.log` 通过，包含同事务完整定义回读、独立读不可见、回滚和提交后可见，Namespace 服务槽位仍为 0。
- 生产 CREATE TABLE 表/列 current/history 接入后，`/tmp/native-schema-catalog-create2.log` 通过客户端 FORCE INDEX 读取、父子 DDL 隔离、分区/索引/约束/LOB 建表、TRUNCATE/DROP 与崩溃重启。首次 `/tmp/native-schema-catalog-create1.log` 的断言把 `cur_default_value_v2` 当作客户端字符串；该内表列实际返回 bytes，修正断言为 UTF-8 字节后通过。用例已加入四件套 direct gate。
- 已重新核对 **current 与 history 的区别**：`get_table_schema_from_inner_table` 明确传 `INT64_MAX-1`，现有普通 DDL 回读读取最新 history，原生替换应保留这个语义；批量 `get_not_core_table_schemas` 的调用传 `INT64_MAX` 时才读取 current。前一版说明将普通 DDL 回读当作 current，判断有误。后续批量原生读取仍必须区分两者。
- 原生 UPDATE 的字段生成也应保留未指定字段（尤其创建时间等）的既有值；不能用整行默认值构造 INSERT 的方式覆盖部分更新。

### 最新生产版本验证

- `/tmp/native-schema-catalog-production-build2.log`：测试注入已移除，生产版本编译通过。
- `/tmp/native-schema-catalog-production-create1.log`：表/列 CREATE 批量写入、`add_table` 的历史插入、`add_single_column` 均已接入后的生产回归通过。客户端强制走内表名称索引，父子分别修改/建表，分区表 TRUNCATE/DROP，进程崩溃重启后再次验证 current/history、默认值和 90KB/120KB LOB 数据。
- `/tmp/native-schema-catalog-production-truncate2.log`：父空间先执行元数据 DDL 再 fork，随后 TRUNCATE；孩子首次数据读取仍取得旧数据，通过。
- 这轮没有完成其余 UPDATE/DELETE/分区与约束写入、SchemaService 批量加载与其 current 读取语义；完整目标保持进行中。

- 最新生产 Namespace 主备回归通过（114.86 秒）：`.scratch/namespace-fork/gate-results/native-schema-catalog/native-writes-standby/results.json` 的 `namespace_fork_local.exit_code=0`。覆盖物化与 LOB 事务回放、孩子 DDL 刷新、父 TRUNCATE/DROP 历史来源、Namespace 删除准入、备库提升后的 schema 恢复及写入、提升后再次 fork。

### 普通 DDL 回读接入

- `get_table_schema_from_inner_table` 的非 core 分支已使用原生目录，仍读取 owner 可见的最新 history（`INT64_MAX-1`），保持不存在时 `OB_TABLE_NOT_EXIST` 的调用约定。只接收 active DDL 事务；任意旧 SCN 仍须上层提供受保护 view。
- `/tmp/native-schema-catalog-ddl-read-build1.log` 编译通过；`/tmp/native-schema-catalog-ddl-read1.log` 通过包含 LOB/分区/索引建表、ALTER、TRUNCATE、父子隔离和崩溃重启的生产回归。
- 后续批量加载前要消除 `get_table` 对 FK/trigger/aux history 的逐表全扫描。固定定义已有 `idx_fk_his_child_tid/idx_fk_his_parent_tid/idx_trigger_his_base_obj_id/idx_data_table_id`；可先按关系索引收集候选对象 ID，再在同视图按主键读取目标 V 的最新 history 并重新检查归属与删除标记。索引的 schema_version 不一定是 key 的最后一列，不能直接沿用主 history 的逐键去重规则。
- 更正前一轮的解码器判断：`src/oblib/lib/utility/ob_macro_utils.h` 中 `OB_SUCC(statement)` 展开包含 `ret = (statement)`，故 `OB_SUCC(result.next())` 已传播迭代错误；不能按其他项目的同名宏推断存在缺陷。该项已核实排除，无须改动解码器。


### 原生单表/批量完整定义装载接入

- `get_table` 增加显式 CURRENT/HISTORY 来源，当前记录沿用同一套解码，历史记录保留版本筛选与删除语义。批量 `INT64_MAX` 读取 current；单表入口继续 history。
- FK、trigger、aux 关系改用已有物理索引先收集候选，再按主键读取目标版本并重新检查归属/删除；FK 的父、子索引候选去重，避免自引用外键重复。没有新增常驻缓存。
- `get_not_core_table_schema/get_not_core_table_schemas` 已接入一个操作持有的原生目录视图，借用事务时保留自身写入可见性；历史 SCN 必须对应仍被持有的目录视图。Namespace 装载入口开始持有目录视图及其 schema_version，直到刷新结束。
- 新增 `self_fk/drop_fk/old_fk` 测试，并在既有冷读取用例中验证 current 来源。默认四件套 bootstrap-native-kv 的 `native_schema_catalog_probe.py` 自动包含这些用例。
- 首次 `/tmp/native-schema-catalog-load-build1.log` 编译通过；`/tmp/native-schema-catalog-load-probe1.log` 的全新实例启动暴露初始化阶段边界错误：GCTX.in_bootstrap_ 已结束、后续内表 DDL 正在刷新，但 Namespace 目录行尚未创建。日志稳定反复出现 get_batch_table_schema ret=-4018，测试实例已终止。改用既有 Namespace lifecycle 的 is_bootstrapping 明确判断目录启用阶段，保留目录建立之前的初始化读取；不按 Namespace ID 或运行错误回退。修正后的验证待完成。
- 完整刷新所用简单表定义入口 `fetch_tables` 仍待接入；以上不能计为全部 SchemaService 读取闭环完成。

- 初始化修正后的 `/tmp/native-schema-catalog-load-build2.log` 编译通过。`load-probe2.log` 已通过 part/part2/drop_index/fk；其后 self_fk 建表失败，根因是测试将同一列同时作为自引用外键父列、子列，现有 resolver 明确不支持（1215/-5317）。改为独立 parent_id 列及其索引后，`/tmp/native-schema-catalog-load-probe3.log` 通过 self_fk/drop_fk/old_fk/history，检查父侧外键关系、历史删除标记及 current/history 不同列数；冷 runtime 槽位保持 0。
- `/tmp/native-schema-catalog-load-probe4.log` 通过 typed/indexed/plain/dropped/generated/lob。既有借用事务 own-write/独立读/回滚，以及索引修改与 90KB/120KB LONGTEXT 事务验证继续通过；这些用例也验证 current 定义读取。
- `/tmp/native-schema-catalog-load-ddl1.log` 通过生产调用链的 CREATE/ALTER/TRUNCATE、内表强制索引、父子隔离和崩溃重启。该轮使用含测试钩子的二进制（测试环境未启用控制文件）；移除钩子的生产编译和主备验证另记。

- `/tmp/native-schema-catalog-load-production-build1.log`：移除测试注入后的生产编译通过。`/tmp/native-schema-catalog-load-production-ddl1.log`：生产二进制的 DDL、父子隔离、强制内表索引及崩溃重启回归再次通过。主备装载回归通过（120.39 秒），结果见 `gate-results/native-schema-catalog/native-load-standby/results.json`；覆盖刷新/回放竞争、物化/LOB 回放、孩子 DDL、父 TRUNCATE/DROP 历史来源、删除准入、提升后的装载/写入及再次 fork。

### 简单表定义刷新接入

- `NativeSchemaCatalog::get_simple_tables` 在同一受保护视图读取简单表定义、分区、外键名称与约束名称。全量读取对关系 history 各扫描一次；增量读取按已有外键索引收集候选，再检查目标版本的最新归属和删除标记。显式空集合、重复 ID、不存在的 ID 与历史版本选择均有验证。
- SchemaService 的 `fetch_tables` 在 Namespace 初始化完成后走此入口。目录建立之前仍走既有 bootstrap 路径。原生全量读取不再为估算超时进入 SQL 执行 `COUNT(*)`；使用配置的默认时限，同时遵守调用方更早的 worker/ObTimeoutCtx 截止时间。
- 完整与简单定义复用分区解码入口；没有增加常驻缓存。四件套既有 `native_schema_catalog_probe.py` 会自动执行新增的冷读取和事务内简单定义验证。
- `/tmp/native-schema-catalog-simple-build1.log` 因对 ObIArray 使用 range-for 编译失败，改为 count/at 后 build2/build3/build4 均通过。
- `/tmp/native-schema-catalog-simple-probe1.log` 通过 part/part2/fk/self_fk/drop_fk/old_fk/history，冷 Namespace 的服务槽位保持 0。`/tmp/native-schema-catalog-simple-borrowed1.log` 通过 plain：借用 DDL owner 读取未提交改名，独立事务不可见，回滚后恢复原值；完整与简单定义均检查。
- `/tmp/native-schema-catalog-simple-ddl1.log` 通过 DDL、索引、父子隔离和崩溃重启。实例日志确认生产 `fetch_tables` 已使用原生全量与增量入口；约 645 张表的全量刷新观察值为几十毫秒，仅代表该测试数据量。
- `/tmp/native-schema-catalog-simple-production-build1.log`：移除测试钩子后生产编译通过。`/tmp/native-schema-catalog-simple-production-ddl1.log` 的生产 DDL、父子隔离及崩溃重启通过。主备回归通过，结果见 `gate-results/native-schema-catalog/native-simple-standby/results.json`；包括备库刷新、物化/LOB 回放、子空间 DDL、父 TRUNCATE/DROP、提升后装载与写入、再次 fork。

### 表版本查询接入

- 简单定义刷新已提交并推送 `7c976ece1`。
- `construct_schema_version_history` 的 TABLE_SCHEMA 分支、`get_table_latest_schema_versions`、`get_ori_schema_version` 在目录初始化完成后改走原生目录。前者的 snapshot_version 参数实际是逻辑 schema 版本，SCN 仍由受保护读取视图决定。
- 最近版本缓存仍保留最新 16 条及最新删除标记；min_version 取完整可见历史的最小值。原生读取使用同一视图下的逆序有限扫描和正序首行，避免为 MIN 遍历该表全部历史。批量最新版本去重并保留删除记录；原始版本读取 `__all_ori_schema_version`，不把它推算成最早 history 版本。
- `/tmp/native-schema-catalog-versions-build1.log` 编译通过。首次测试命令 `versions-probe1.log` 因 argparse 的 case 选项漏加 versions 退出，尚未启动实例；补齐选项后 `versions-probe2.log` 通过 plain/versions/history/old_fk。
- versions 用例实际产生 41 条表历史：与 SQL 聚合结果核对最小值、最新值和原始值，验证最近 16 条的完整顺序。plain 新增未提交的 history 删除记录，验证借用事务可见、独立读取不可见、回滚恢复。冷服务槽位保持 0。四件套默认原生目录 probe 自动包含新增用例。
- `/tmp/native-schema-catalog-versions-production-build1.log` 移除测试钩子后生产编译通过；`/tmp/native-schema-catalog-versions-production-ddl1.log` 的 DDL、父子隔离及崩溃重启通过。备库历史 schema 用例退出 0（348.90 秒），结果见 `gate-results/native-schema-catalog/native-versions-standby/results.json`；覆盖 12 个用户物理 tablet、主备 major、备库崩溃恢复、提升后 DDL 与 major，以及 Namespace 1/4 的历史 checksum。
- 仍需接入表/索引/约束/外键的名称查询、当前表版本查询、索引信息查询，以及既有清单中的完整 DDL 写入；本轮目标仍未完成。
- 核心表定义还有独立读取：`get_core_table_priorities/get_core_table_columns` 使用 `ObCoreTableProxy::load`，后者仍对 `__all_core_table` 执行 SQL。正常装载不能漏掉这条路径；后续应复用固定内表定义和现有 core 行解码，避免通过虚拟表查询核心定义。初始化阶段与目录已经建立后的访问必须分清。

#### 待定位：核心 tablet 的 minor 行版本结束标记

- 上述备库历史 schema 场景中，主库子空间 4 的逻辑 tablet 1（物理 ID `4611686568183201793`）在 minor 合并多份 MINI 时反复报 `-4016`。报错位置 `ObMacroBlockWriter::check_order`：切换到下一行时，上一行未见 Last 标记；涉及 `__all_global_stat/change_stream_min_dep_lsn` 等 core 行。
- major 准备等待期间，后台物化/接管持续推进，最终主库与备库 major、恢复、提升后 major 和用户数据/索引校验均完成。这个日志异常没有阻止本次用例完成，但不能据此认定 minor 正确，也尚未证明它由版本查询改动引入或此前已存在。
- 已保留同一失败 DAG 的证据 `gate-results/native-schema-catalog/native-versions-standby/minor-core-error.txt`。后续需缩小到父子核心 tablet 的继承 MINI 与本地 MINI 合并，核对截取边界及多版本结束标记，并做基线对照。`schema_history` 场景已在四件套 direct 的 standby suite 中；当前断言覆盖合并完成和数据正确性，尚不能替代这条 minor 异常的定向断言。

### 核心表定义读取接入

- 核心表的定义实际保存为 `__all_core_table` 中的单元格记录。原生读取按 `(table_name, row_id)` 聚合一条逻辑记录，复用原来的 `ObCoreTableProxy::Row` 解码器；解码器仅需调用方分配器，不再要求 SQL proxy。每条逻辑记录处理后释放临时单元格，不保留整张 core 表的原始行。
- 表和列分别选择目标 schema_version 以内的最大可见版本，并处理删除标记。物理 row_id 只表示记录身份，不能当作版本顺序。`__all_core_table` 自身仍由固定定义提供。
- 正常 `get_core_table_schemas_at_version` 接入同一原生目录视图；目录建立前的 bootstrap 保留原入口，依据既有 Namespace lifecycle 判断，不按 Namespace ID 或失败结果回退。
- `/tmp/native-schema-catalog-core-probe1.log` 在核心物理 tablet 的版本 0 上返回 -4002：原生 bind 错误地要求版本大于 0，已与既有 schema/DML 参数规则统一为允许 0、拒绝负数。
- `/tmp/native-schema-catalog-core-probe2.log` 发现空 comment 被当作 SQL NULL（-4152）。普通字符串复制在长度 0 时清空指针，而 core 单元格以空指针表示 NULL；已复用保留空串/NULL 区别的单元格复制逻辑。
- `core-probe3.log`、`core-probe4.log` 已读出 22 张核心表，但在与固定定义比对时失败。固定定义生成器的范围漏掉了 LOB 辅助表，已补充生成这一类定义。最终通过证据待补充，不能把这两次失败记为通过。
- `plain` 用例新增核心表/列的逐项比对，数量与客户端虚拟核心表查询核对；另新增同事务插入乱序历史、删除标记、独立事务不可见及回滚测试。它们由四件套既有原生目录 probe 自动执行。
- `/tmp/native-schema-catalog-core-build5.log` 编译通过。`/tmp/native-schema-catalog-core-probe5.log` 的 plain/part 通过；核心目录为 22 张表、284 列，逐项核对名称、列 ID/类型、主键位置、逻辑 tablet ID；`CORE_HISTORY ... stage=read_rollback ret=0` 验证物理 row_id 与逻辑版本逆序、旧版本读取、删除标记、外部隔离及回滚，两个孩子的服务槽位均为 0。
- `/tmp/native-schema-catalog-core-production-build1.log` 移除测试钩子后的生产编译通过；`/tmp/native-schema-catalog-core-production-ddl1.log` 通过实际 DDL、索引、父子隔离和崩溃重启。Namespace 主备回归通过，结果见 `gate-results/native-schema-catalog/native-core-standby/results.json`；覆盖刷新与回放竞争、物化/LOB 事务回放、孩子 DDL、父 TRUNCATE/DROP 历史来源、Namespace 删除准入、提升后的装载/写入和再次 fork。

### 当前版本与索引列表查询接入

- 核心定义读取已提交并推送 `b9b7f77ee`。
- `get_table_schema_versions` 已接入原生目录：当前记录不存在时不输出，重复请求只输出一次，保留原接口向输出数组追加的约定。它与返回 history 最新版本及删除标记的 `get_table_latest_schema_versions` 分开。核心表从 core 单元格中选择最新可见定义，不再经过虚拟表 SQL。
- `get_table_index_infos` 使用现有 `__all_table.idx_data_table_id` 收集候选，在同一受保护视图中回读当前记录，并检查 database_id、data_table_id 和 USER_INDEX 类型；核心表索引从同视图的 core 定义读取。返回名称复制到调用方分配器。
- 两个生产入口沿用已建立的 Namespace lifecycle 初始化边界，普通 DDL 借用所属事务；原生方法自身不要求装载 Namespace 服务。
- `/tmp/native-schema-catalog-current-metadata-build1.log` 编译通过；`/tmp/native-schema-catalog-current-metadata-probe1.log` 通过 typed/plain/part/drop_index/old_fk/history。版本和索引信息与客户端 SQL 核对，包括三个核心内表索引、输出数组追加、重复/不存在 ID、错误 database_id 过滤、删除索引后的空集合及历史/当前区别。
- part 用例在未提交事务内改变索引的 data_table_id、名称和版本，验证旧归属消失、新归属可见，独立事务保持旧值，回滚后恢复。typed 用例验证新建当前表版本在自身事务、独立事务、回滚及提交后的可见性。六个冷 Namespace 的服务槽位均为 0；新断言已随既有 probe 进入四件套。
- `/tmp/native-schema-catalog-current-metadata-production-build1.log` 移除测试钩子后的生产编译通过；`/tmp/native-schema-catalog-current-metadata-production-ddl1.log` 通过 DDL、强制内表索引、父子隔离及崩溃重启。主备回归通过，结果见 `gate-results/native-schema-catalog/native-current-metadata-standby/results.json`；包括刷新与回放竞争、物化/LOB 回放、孩子 DDL、父 TRUNCATE/DROP、删除准入、提升后装载/写入与再次 fork。
- 本轮仍须完成表/索引/约束/外键的名称查询、mock FK 父表查询，以及完整 DDL 更新、删除、分区与关系写入。核心 tablet minor 行版本结束标记异常仍按前述 TODO 单独跟踪。

### 名称查询接入

- 表、索引、约束和外键名称查询，以及 mock FK 父表名称/当前版本查询已接入原生目录，测试验证中。普通名称使用既有物理索引收集候选，在同一视图回读当前记录与所属表；mock FK 父表没有名称索引，保留既有扫描及字节精确比较规则。
- 表名称策略由调用方显式传入。按指定 session 的临时表、普通表、内表名称匹配的顺序选择；隐藏表、索引与 LOB 辅助表不作为普通表名返回。旧 SQL 表名查询误用输出 table_type 检查候选类型，新路径按候选行类型过滤。
- `/tmp/native-schema-catalog-names-build1.log` 编译通过；`names-probe1.log` 中 typed 通过，plain 的客户端对照暴露核心表自身记录遗漏：`__all_virtual_core_all_table` 确实包含 table_id=1。完整定义装载跳过它并使用编译定义，但名称/当前版本查询必须读取其持久记录。已修正 metadata-only 核心读取，增加名称与版本的客户端对照，待重跑。
- 继续核对调用点发现 `fetch_aux_tables` 仍被 `namespace_schema_publication.cpp` 和 `ob_server_schema_service.cpp` 使用，不能当作死代码。前者的 `publication_schema` 还用 SQL 查询存在/删除状态。这些属于本轮必须接入的表目录读取。
- mock FK 父表的简单/完整定义、列及外键关系仍有独立 SQL 装载入口（`fetch_mock_fk_parent_tables`、`fetch_all_mock_fk_parent_table_info` 等）；名称与版本查询接入不代表这些装载入口已完成。
- `/tmp/native-schema-catalog-names-build2.log` 编译通过；`names-probe2.log` 的 typed/plain/part/drop_index/fk 通过，核心表自身名称/当前版本与 SQL 对照一致（ID=1，版本=1048584）。随后 mock_fk 在 SQL 解析阶段失败：未选择默认库时，`ObDDLResolver` 从 session 取得无效 database_id，尚未进入原生目录。单独 `names-mock-repro1.log` 重现相同 1210。仅在用例中增加 `USE` 后，`names-mock-probe1.log` 的 mock_fk/drop_fk/old_fk 全部通过，冷服务槽位仍为 0。
- TODO（解析器）：缺失 FK 父表的预检查使用 session database_id 和 foreign_key_name；应核对显式限定数据库及父表名。现有代码路径可解释上述失败，但尚未做基线版本的运行对照，不宣称是本轮引入或已修复。原生目录回归先使用明确选择数据库的会话。

### 辅助表与 DDL 发布读取接入

- 将完整定义已有的辅助表读取提取为 `get_aux_tables`，正常 `fetch_aux_tables` 使用同一原生入口。历史索引只提供候选，按目标版本读取最新可见主记录并重新检查删除/归属，结果按 ID 排序。
- `NamespaceSchemaPublication` 在 DDL owner 中持有一个原生目录操作，读取已发布版本的 history 和本次事务的 current；不存在/已删除的表不加入对应定义集合。完整定义已包含 LOB 辅助关系，移除发布过程原有的 SQL 存在性检查和重复辅助表读取。
- 四件套既有冷读取用例增加 SQL 历史查询对照辅助表 ID/类型/索引类型，增加外键/约束名称大小写及已删除外键名称断言。`/tmp/native-schema-catalog-names-aux-build1.log` 编译通过；`names-aux-probe1.log` 的 typed/plain/part/drop_index/fk/mock_fk/drop_fk/old_fk 全部通过，对应实例 `namespace_fork_PROTOTYPE_native_schema_catalog_nfdf4hie`。八个孩子服务槽位均保持 0，包含自身事务与独立读隔离、回滚以及临时表/隐藏表/索引/LOB 名称选择。
- `/tmp/native-schema-catalog-names-aux-production-build1.log` 移除测试钩子后生产编译通过；`names-aux-production-ddl1.log` 的实际 DDL、LOB、父子隔离、SQL 索引读取及崩溃重启通过。Namespace 主备回归通过（121.63 秒），结果见 `gate-results/native-schema-catalog/native-names-aux-standby/results.json`；涵盖刷新与回放竞争、孩子 DDL、父 TRUNCATE/DROP 后历史来源、删除准入以及提升后的恢复、写入和再次 fork。

### 缺失外键父表定义装载

- 名称、辅助表和发布定义读取已提交并推送 `4c517d04d`。
- `get_mock_fk_parent_tables/get_simple_mock_fk_parent_tables` 从 Namespace 的固定 history 内表读取定义，支持全量、指定 ID、重复/不存在 ID 和显式空集合。完整定义在同一操作中读取列及外键关系；普通表与 mock 父表共用外键读取实现。
- 完整批量装载和简单定义刷新接入原生目录；目录建立前仍使用既有初始化读取。列历史选择最新可见版本后恢复既有列 ID 升序。
- 冷读取用例增加多列 mock FK、删除后读取历史定义及最新视图排除已删除定义；列 ID/名称逐项与客户端 SQL 对照。实际 DDL 用例增加父子独立解析缺失父表、同一父表两条外键及重启后创建真实父表。主备用例增加占位父表回放和提升后创建真实父表，均随既有脚本进入四件套。
- `/tmp/native-schema-catalog-mock-build1.log` 编译通过；`mock-probe1.log` 的 part/fk/mock_fk/mock_fk_multi/old_mock_fk/self_fk/drop_fk/old_fk 八个冷用例通过，对应实例 `namespace_fork_PROTOTYPE_native_schema_catalog_5an7zo59`。历史 mock 用例以删除前已提交的 DDL 版本为上限，包含同一 DDL 创建的父表占位记录；不能假定子表那一行的 schema_version 就是整个 DDL 的最终版本。生产编译及运行验证继续进行。
- `/tmp/native-schema-catalog-mock-production-build1.log` 移除测试钩子后编译通过；`mock-production-ddl1.log` 的实际 DDL、父子独立解析同一缺失父表、同一父表两条外键、崩溃恢复和恢复后创建真实父表/写入数据通过。Namespace 主备回归通过（125.69 秒），结果见 `gate-results/native-schema-catalog/native-mock-standby/results.json`，日志包含 `mock_parent_schema_replay` 和 `mock_parent_resolution_after_promotion`，验证了新增读取的备库刷新及提升后 DDL 回读。

### 下一阶段：完整 DDL 写入

- 现有 `SchemaCatalogRow::finish` 用固定默认值补齐插入行；不能直接拿它替换 SQL 的部分字段 UPDATE，否则未指定字段会被覆盖。需在同一 owner 的行锁下保留旧值，只覆盖提供的字段，并据新旧行维护索引。LOB 更新复用存储协议，核对未修改 LOB 的处理。
- 首先接表/列更新与删除，再接分区、约束、外键和 mock 父表等关系写入，包括直接 `sql_client.write` 的批量/范围操作、history 和发布。现有 SQL 写入继续存在，不能把读取接入完成当作整个目标完成。

### 部分字段更新与表/列更新（实施中）

- `SchemaCatalogRow::finish_update` 要求完整主键，只输出明确提供的非主键列；不补插入默认值。`NativeSchemaCatalog::update_fields` 在 owner 的行锁下保留旧字段，根据合并后的新旧行维护内表索引；不存在的行返回成功且 matched=false。
- `NativeInnerTable` 增加带更新列清单的更新，内部读取当前存储行并保留未指定的值；原有整行更新入口保留。存储形式读取保留 LOB locator，未指定的 LOB 列不读取正文、不列入 LOB 更新；没有增加常驻缓存或跨操作状态。
- 生产 `update_single_column/update_table_attribute/inner_update_table_options_` 的正常非核心表路径接入原生更新，复用原有字段映射及同一个 DDL owner，history 和日志继续走各自既有入口。完整表字段映射明确提供的 LOB 仍按更新字段处理；不能把“未指定 LOB 不重写”扩大成自动判定所有 LOB 正文是否改变。
- 四件套增加部分字段更新时的索引旧键移除/新键可见、未修改 90KB LOB 保留、120KB LOB 单列更新、不存在记录，以及旧快照事务保留其他事务新提交字段的断言；生产脚本增加继承表列改名/默认值修改、表改名、内表 FORCE INDEX 对照与重启校验。`/tmp/native-schema-catalog-partial-build1.log` 编译中，尚未计为通过。
- `/tmp/native-schema-catalog-partial-build1.log` 编译通过；`partial-probe1.log` 的 typed/indexed/part/history/dropped/lob 六个冷用例通过，对应实例 `namespace_fork_PROTOTYPE_native_schema_catalog_8be2hza0`。`PARTIAL`、`LOB_RETAIN`、`PARTIAL_LATEST` 标记均为 ret=0，直接验证了 locator 字节未变和旧快照事务没有覆盖其他事务新提交的字段。
- 继续接入 `update_table_schema_version/update_data_table_schema_version/update_index_status/update_index_type`。通过局部模板编码器共用初始化 SQL 与正常原生字段映射；更新字段清单不包含 LOB。显式维护原先由 SQL `ON UPDATE` 提供的表 gmt_modified，并增加实际 DDL 后时间戳前进的断言。`partial-production-build1.log` 生产编译中，后接入入口尚未完成运行验证。

- `/tmp/native-schema-catalog-partial-production-build1.log` 移除测试钩子后编译通过；`partial-production-ddl1.log` 退出 0，覆盖实际列改名、默认值更新、表改名、gmt_modified 前进、强制内表索引、父子隔离、崩溃重启与重启后解析缺失外键父表。Namespace 主备回归正在运行，尚未计为通过。

- 部分更新的 Namespace 主备回归退出 0（127.25 秒），见 `gate-results/native-schema-catalog/native-partial-standby/results.json`。生产编译、DDL/重启和主备验证完成；代码已提交并推送 `9829966a8`。

### 表、列和约束删除接入（实施中）

- 原生目录增加按字段插入和非空主键前缀删除。删除只接受连续主键前缀，拒绝缺失前导键、非主键条件或空条件；完整主键直接点删，范围删除在本次受保护视图收集主键后关闭游标，再逐行加锁并维护索引。主键清单只活到本次调用结束；对象并发串行化沿用 DDL owner。
- 普通表/列删除、表/列 history 删除标记、单个约束及其列关系删除接入同一个 DDL 事务。bootstrap 与 core 的既有表示仍显式处理；统计信息等其他内表写入未据此宣称完成。
- 四件套冷用例增加前缀删除自身未提交写入、相邻对象保护、空条件/缺失键拒绝、索引一致性及提交/回滚。生产用例增加继承表删除约束、删除列后重用同名列、LOB 列删除及 PURGE 后的当前表/history/索引校验。尚未编译和运行验证。

- `/tmp/native-schema-catalog-delete-build1.log` 编译通过。`delete-probe1.log` 的 typed 通过，indexed 新增前缀删除用例在合成行插入时返回 -4152；固定定义明确要求非空 `is_deleted`，用例漏填，已补充为 0，原生接口的非空约束保持不变。`delete-ddl1.log` 的 DDL 均执行成功，但删除列后重用名称的 FORCE INDEX 断言为空，正在通过逐步查询表 ID、主表与索引缩小原因；尚不能计为通过。

- 最小复现 `/tmp/native-schema-catalog-delete-repro1.log` 证明删除 LOB 列走离线 DDL：表 ID 从 500003 换为 500007，主表与名称索引都正确返回新表的 v 列。修正生产用例在同名列复用之后才删除 LOB，并分别检查原对象的 history 与新对象的 current；新对象列 ID 可重新从 16 起分配，不能跨表比较 column_id。两项失败均为测试输入/断言错误，未据此改动存储逻辑。

- `/tmp/native-schema-catalog-delete-build2.log` 编译通过；`delete-probe2.log` 的 typed/indexed/part/history/dropped/lob 六个冷用例退出 0，实例 `namespace_fork_PROTOTYPE_native_schema_catalog_egqbr6ne` 的 `PREFIX_DELETE stage=prefix_deleted ret=0` 覆盖了新增范围操作。`delete-ddl2.log` 的实际 DDL、父子隔离、删除标记、同名列复用、LOB 离线 DDL、PURGE 和崩溃重启通过。已去除探针钩子，继续生产版本和新增删除主备回放验证。

- `/tmp/native-schema-catalog-delete-production-build1.log` 去除测试钩子后编译通过；`delete-production-ddl1.log` 退出 0。用例显式启用 recyclebin，确认 DROP 后原表目录行仍存在、PURGE 后删除，验证了最终删除入口。删除主备回放仍运行中。

### 下一批仍须接入的写入入口

- 约束：`add_constraints_for_not_core`、`rename_csts_in_inner_table`、`delete_constraint`（整表批量删除），以及约束属性更新。当前只接入了单约束删除。
- 分区：`ObPartSqlHelper` 的批量 current/history 写入、增删/重命名 helper，`ObTableSqlService` 的分区选项更新、整表/指定分区删除。复用已有边界值编码，按一个 DDL owner 批量执行，避免每个分区保留一个目录操作上下文。
- 外键与 mock 父表：对应 current/history 插入、更新、删除，以及直接 SQL 的批量操作。现有读取接入并不证明这些写入完成。
- 以上之后继续审计完整表定义写入、关联内表及发布路径；生产操作日志和 core 单元格式入口尚有 SQL，不能按本阶段通过的用例宣称全部 SQL 依赖消除。

- 删除阶段生产 Namespace 主备回归退出 0（132.65 秒），见 `gate-results/native-schema-catalog/native-delete-standby/results.json`。`native_catalog_delete_replay` 验证删列、删约束、LOB 离线 DDL、当前行清理与 history 删除记录回放；提升后再次校验数据和 PURGE 对象不存在，原有物化/LOB 回放、父 TRUNCATE/DROP、Namespace 删除及提升后 fork 也通过。
- 本阶段修改及过程失败的用例已纳入四件套共用冷探针、生产 DDL 和 Namespace 主备脚本。未运行全量 mysqltest 或 sysbench；完整目标仍在进行。

### 约束完整写入接入（实施中）

- `NativeSchemaCatalog` 统一维护约束及其列关系的 current/history：插入、状态或表达式更新、改名、删除均借用 DDL owner。单条和批量入口复用相同操作；批量建表/删除约束只建立一个目录操作上下文。原有内表不允许用户约束的入口约束保持不变，没有新增 bootstrap 或 Namespace ID 写入分支。
- `ObTableSqlService` 的建表约束、ALTER 添加/删除约束、状态更新、列关系修订、回收站改名和整表批量清理均改用原生目录，移除对应 SQL 字段生成与批量 SQL 拼接。权限检查和操作日志仍在上层。
- 旧回收站改名的 history 字段生成漏写 rely/enable/validate，可能把非默认状态恢复成默认值。新的改名历史使用同一份完整约束字段映射，保留这些属性；生产用例比较 current 与最新 history 的全部状态字段。
- 四件套新增冷 Namespace 的约束写入、状态更新、改名、关系行、未提交不可见及提交/回滚验证；实际 DDL 用例覆盖 NOT ENFORCED / ENFORCED、改列名后的表达式、添加双列 CHECK、回收站改名与 PURGE、直接 DROP、重启及主备回放/提升。首次编译及旧二进制对照验证运行中。

- 上一阶段主备验证保留的二进制（`tools/obtest/bin/observer`）对照 `/tmp/native-schema-catalog-constraint-prior-repro1.log` 在回收站改名断言失败：同一 constraint 的 current enable/validate 为 0/0，最新 history 为 1/1，证实字段遗漏造成状态不一致。该回归已加入共用生产 DDL helper，并被主备脚本复用。`constraint-build1.log` 编译退出 0，开始当前版本冷探针及实际 DDL 验证。

- `constraint-probe1.log` 的 typed/part/constraint 三个冷 Namespace 用例退出 0，实例 `namespace_fork_PROTOTYPE_native_schema_catalog_nyqb2oxj` 的 `NATIVE_SCHEMA_CATALOG_CONSTRAINT stage=deleted ret=0` 验证了非默认 rely/enable/validate、完整 history、关系行以及提交/回滚。`constraint-ddl1.log` 的真实 CREATE/ALTER/DROP/PURGE、改列表达式、父子隔离和崩溃重启通过；旧二进制失败的同一回收站状态断言已通过。测试钩子已移除，生产编译及主备验证继续。

- `/tmp/native-schema-catalog-constraint-production-build1.log` 去除测试钩子后编译通过；`constraint-production-ddl1.log` 退出 0，再次覆盖完整约束 DDL、回收站状态一致性和重启。对源码按约束四张表的表名/ID 检索，`ObTableSqlService` 已不再直接拼接或写入约束 SQL；剩余表名引用是 SchemaService 初始化读取 helper，正常装载入口已走原生目录。主备回放验证仍在运行。

- 首次 `native-constraint-standby` 回放检查失败在新增 helper 的首次 `SELECT native_constraints.state_t`（1146）。该检查漏掉了其他主备用例已有的 schema 可见性等待：日志回放水位达到目标不等于已装载 Namespace 的 schema cache 已刷新。新增同样的有界表结构/数据等待，再执行全部持久 current/history 断言；不把该首次失败记为通过。

- 约束阶段生产 Namespace 主备回归退出 0（140.27 秒），见 `gate-results/native-schema-catalog/native-constraint-standby2/results.json`。新增 `native_catalog_constraint_replay` 及提升后复查通过，包括 current/history 状态、关系行删除标记、回收站改名和 PURGE；生产编译、冷 Namespace、DDL 与重启验证均已完成。完整目标继续推进分区和外键关系写入。

### 外键和占位父表完整写入（实施中）

- 外键、列关系以及 mock 父表/列的 current/history 写入改用原生目录。父表删除后的外键重建保留原有 upsert 语义；同一 DDL owner 中更新当前行、历史和物理索引。真实父表替换占位父表时，先发布旧列关系删除版本，再以新版本写入真实列关系。
- 删除 ObTableSqlService 的相应 SQL 字段生成、拼接和执行 helper；字段映射归入 SchemaCatalogFields，批量外键/列操作共用一个目录操作。操作日志、对象版本生成继续由上层负责。
- 四件套冷探针增加多列外键与占位父表的 own-write、独立读不可见、状态更新、提交/回滚、删除回滚；真实 DDL/主备共享用例增加占位父表增删列、父表删除后重建、真实列 ID 变化、无效数据的外键校验失败回滚，以及 current/history 对照。
- `fk-build1.log` 首次编译发现测试探针对 ObIArray 使用 range-for；待本次编译终止后改为 count/at。尚未计为通过。

- `fk-build2.log` 编译通过。`fk-probe1.log` 的冷多列外键首次 upsert 报重复主键：原实现先调用锁定读取，底层 lock_rows 假定行存在，缺失键上的锁节点使随后的 INSERT 判为重复。新增 upsert 改为先读当前行，缺失时直接 INSERT，存在时锁定 UPDATE；对象串行化由原有 DDL owner 提供，未修改存储锁协议。
- `fk-ddl1.log` 的父表删除/重建及列映射替换已执行成功，随后新增断言错误地要求校验失败的外键完全消失。旧生产二进制最小对照 `fk-validation-prior1.log` 证实同样返回 5785，仍保留 enable=1、validate=0 的 current/history 记录。测试改为核对该既有持久状态并显式 DROP；原生事务回滚单独由冷探针验证，不能将这条 DDL 测试宣称为整个异步 DDL 回滚成功。
- TODO（既有 DDL 异常清理）：`ObConstraintTask::rollback_failed_foregin_key` 构造回滚 alter_table_arg 后没有实际调用 alter_table，仅记录成功。需要单独修复并验证失败后清理语义；这不是本轮原生外键写入引入，旧二进制已有运行证据，不阻塞当前访问层替换。

- `fk-build3.log` 编译通过；`fk-probe2.log` 的 fk/mock_fk_multi/old_mock_fk/self_fk/old_fk 五个冷 Namespace 用例退出 0，实例 `namespace_fork_PROTOTYPE_native_schema_catalog_k3vgi6qx` 的 FOREIGN_KEY stage=deleted ret=0。首次 upsert 重复键回归已通过，所有孩子 runtime 槽位保持 0。
- `fk-ddl2.log` 的真实 DDL、父表删除和多次重建、占位列增删、列 ID 替换、完整 current/history 对照、显式/整表外键删除、父子隔离、崩溃重启及重启后父表解析均通过。钩子已移除，生产编译和主备验证继续。

- `/tmp/native-schema-catalog-fk-production-build1.log` 移除测试钩子后编译通过；`fk-production-ddl1.log` 退出 0，真实 DDL 与崩溃重启复查全部通过。`ObTableSqlService` 已无外键/占位父表八张内表的 SQL 表名引用；主备结果待确认。
- 剩余写入清单补充：`ObPartitionSqlHelper` 批量与增量分区路径、`ObTableSqlService` 分区选项/删除、`update_view_columns`、`batch_insert_ori_schema_version`，以及完整定义关联入口（包括 trigger 服务）和操作日志/core 表示的边界审计。本阶段通过不能替代这些入口接入。

- 外键阶段生产 Namespace 主备回归退出 0（139.30 秒），见 `gate-results/native-schema-catalog/native-fk-standby/results.json`。`native_catalog_foreign_key_replay` 和提升后的复查均通过：外键完整属性、真实父列 ID、旧列关系删除记录、占位表/列删除 history 一致；原有父 TRUNCATE/DROP、Namespace 删除、提升后写入及再次 fork 继续通过。过程失败用例保留在四件套共享脚本中；未运行全量 mysqltest 或 sysbench。
