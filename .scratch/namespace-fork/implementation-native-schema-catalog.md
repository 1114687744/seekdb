# Namespace 原生 schema 目录

状态：实施中；基线 `42d56a8ad`；原生读取和物化进展代码已提交并推送 `908aa69b7`。表/列原生写入代码已提交 `4d5f310a2`。本文记录本轮完整目标和验证证据，未勾选项不表示完成。

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
4. **剩余读取**：`get_table_schema_from_inner_table/get_not_core_table_schema/get_not_core_table_schemas`。DDL reread 需要原 owner，SchemaService 批量读取需要同一受保护 SCN；批量 FK/trigger/aux 关系应按一批收集，避免对每个表全扫描。
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
- 普通 DDL 回读迁移必须保持 **current 与 history 的区别**：一个 DDL 可能先改 current 行，再回读生成 history；不能把现有 current 回读直接替换为 `get_table(...V)` 的 history 选择。后续原生 current 读取应显式表达，并在同 owner 下复用完整解码。
- 原生 UPDATE 的字段生成也应保留未指定字段（尤其创建时间等）的既有值；不能用整行默认值构造 INSERT 的方式覆盖部分更新。

### 最新生产版本验证

- `/tmp/native-schema-catalog-production-build2.log`：测试注入已移除，生产版本编译通过。
- `/tmp/native-schema-catalog-production-create1.log`：表/列 CREATE 批量写入、`add_table` 的历史插入、`add_single_column` 均已接入后的生产回归通过。客户端强制走内表名称索引，父子分别修改/建表，分区表 TRUNCATE/DROP，进程崩溃重启后再次验证 current/history、默认值和 90KB/120KB LOB 数据。
- `/tmp/native-schema-catalog-production-truncate2.log`：父空间先执行元数据 DDL 再 fork，随后 TRUNCATE；孩子首次数据读取仍取得旧数据，通过。
- 这轮没有完成其余 UPDATE/DELETE/分区与约束写入、普通 current 回读、SchemaService 批量加载；完整目标保持进行中。

- 最新生产 Namespace 主备回归通过（114.86 秒）：`.scratch/namespace-fork/gate-results/native-schema-catalog/native-writes-standby/results.json` 的 `namespace_fork_local.exit_code=0`。覆盖物化与 LOB 事务回放、孩子 DDL 刷新、父 TRUNCATE/DROP 历史来源、Namespace 删除准入、备库提升后的 schema 恢复及写入、提升后再次 fork。
