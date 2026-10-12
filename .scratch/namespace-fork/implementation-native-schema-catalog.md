# Namespace 原生 schema 目录

状态：实施中；基线 `42d56a8ad`；原生读取和物化进展代码已提交并推送 `908aa69b7`。表/列原生写入代码已提交 `4d5f310a2`。本文记录本轮完整目标和验证证据，未勾选项不表示完成。

最新进度：完整定义装载已提交 `72a71854d`；简单定义全量/增量刷新已提交 `7c976ece1`；版本历史、最新版本、原始创建版本查询已提交 `7e7c2d73f`；核心定义读取已提交 `b9b7f77ee`。当前版本和索引列表查询也已接入，通过冷读取、生产 DDL/重启和 Namespace 主备验证，记录见文末。名称查询和 DDL 更新、删除、分区及关系写入仍未完成；下文各阶段的“待接入”描述按时间记录，不代表最新状态。

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
