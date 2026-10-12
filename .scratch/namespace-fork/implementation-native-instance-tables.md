# 实例元数据改为独立原生内表

状态：代码提交 `2031b2ef0`；本轮定向验证已完成。

## 范围

本轮只实施已确定的第一步：Namespace 信息、名称映射、目录页、计数器、快照协调从统一 KV 拆为独立内表，并修复目录新根与旧读快照不一致。SQL schema 目录改造、物理对象完整描述以及删除布局历史留在后续。

## 存储与定义

`share/instance_meta/instance_meta_schema.h` 是固定内表的共同声明，包含表名、物理 tablet ID、主键、字段名、类型和长度。LS 创建 tablet、原生读写计划、调试虚拟表共用它。声明直接编入二进制，不需要 SchemaService，不注册进任何 Namespace 的 SQL 目录，不进入 fork 的表集合。

| 内表 | tablet ID | 主键 | 其他内容 |
|---|---:|---|---|
| NAMESPACES | 49404 | namespace_id | 名称、目录根、SCN cap、schema_version、父空间和状态等明确字段 |
| NAMESPACE_NAMES | 49406 | name | namespace_id |
| PAGES | 49407 | page_id | page_bytes |
| COUNTERS | 49408 | record_id | high_watermark |
| SNAPSHOT_COORDINATION | 49409 | record_id | watermark |

名称继续按字节精确匹配，包括内嵌 NUL，避免字符串排序规则引入大小写或尾空格等价。声明的 TEXT 标识供通用诊断格式化使用，存储采用二进制字符串。其余数值直接使用有符号/无符号整数列。

现有物理布局 KV 保留在 49405，只有 STORAGE_LAYOUTS/TABLE_STORAGE_LAYOUTS 两个 collection。原实例 collection 的 JSON 持久编解码和 key codec 删除；目录页内部的树/对象格式保持不变。

## 事务与保留

`NativeInnerTable` 分开管理访问上下文和绑定的 Table：前者持有事务、快照及读者登记，后者持有物理地址和固定读写描述。同一组的五张表共用一个事务及一个固定快照，未访问的表也从事务取得快照前开始受保护。不能到首次访问某表时才补登记。

InstanceMetaStore 继续管理目录 GC 准入；既有 SQL 原生事务借用、目录/布局跨组共同提交，以及所有者结束后 detach 的规则保留。新增的四张 LS 内表接入普通事务行、mini/minor 与统一读者保留边界。

## 目录读视图缺陷

旧二进制已在本轮再次确定性复现：A 固定快照 S，B 提交新根和新页，A 加锁读到 B 的根，按 S 读取新页返回 -4018。证据目录：`/data/1/nijia.nj/test/namespace_fork_PROTOTYPE_catalog_locked_root_emd3cj2y`。

两种合法目录读取必须明确区分：

- 历史根：页面按固定快照读取，包含已被 GC 逻辑删除但受 MVCC 保护的版本。
- 持有根锁的写入者：页面按当前可见版本读取，目录页不可变，根锁固定被更新的根，事务的 GC 准入保护持续到提交/detach。

`PageRead::LOCKED_ROOT` 在 DDL 发布和物化入口明确指定；`stage_catalog_delta` 在获取根锁后使用相同视图。普通读保持原快照；不在通用 get_for_update 中推进快照，不在读取失败后自动切换视图。save_page 的插入去重使用当前不可变内容。

审查时否决过“全部页面使用当前读”的候选：它会破坏旧目录读视图在页面逻辑删除之后的读取。既有 `catalog_read_view` 与备库页面删除回放用例用于覆盖此约束。

## 调试展示

保留 `__all_virtual_instance_metadata` 的既有列和诊断编号。它从固定字段声明通用生成 JSON，不再解码各 collection 的持久 value。单个 bytes 载荷继续提供 base64 展示，不在展示代码中按 PAGES 名称分支。可见性沿用既有入口规则。

## 验收

结果见 `gate-results/native-instance-tables/results.json`，逐项证据见同目录的 `passed-cases.json` 和 `native-assertions.txt`。四件套的 native-inner-table 范围加入原有失败的 locked-root 确定性用例。原生用例增加跨独立内表固定快照、跨目录与布局事务共同提交/回滚。继续复用 typed records、目录 GC、SQL read view、物化与 DDL 故障恢复、备库回放/切主用例。不运行完整 mysqltest 和 sysbench。

### 已取得的验证结果

- 两种 Release 构建均通过；生产源码已移除临时测试钩子。
- 原生四阶段、三次 SIGKILL 恢复通过；覆盖 Namespace/名称/页面/计数器/快照协调、固定视图和页面 GC、旧布局 KV、跨组事务及 SQL 原生事务借用。
- locked-root 确定性用例通过：新根及页面可读，未加锁的另一条记录保持旧快照，提交后旧映射与两次新增映射均存在。
- SQL 与物化共同事务的故障回归通过，覆盖回滚、崩溃恢复及并发创建。
- bootstrap、SQL、直接连接和 TLS 通过；备库 basic、Namespace 追随/重启/切主，以及主库 GC 删除页面回放后备库旧 RR 查询延迟首次访问通过。
- Share 头文件归属完整检查仍有基线已有的三个归属差异；对照 `ca04c7025`，错误集合没有增加，新声明已登记。见 `gate-results/native-instance-tables/share-header-delta.json`。

### 过程中失败

1. 编译修正了 set_varbinary 参数、allocator 标签长度、set_data_length 的有符号重载及 ObRowkey 的可变指针接口。首轮边编译边编辑还产生过编译器读取旧文件长度的 NUL 诊断；检查源码无 NUL，后续在源码固定后重编译通过。
2. locked-root 新增的首次断言错误地要求“加锁后的同一行仍读到旧版本”。锁操作属于本事务，不能用该行证明全局普通读快照是否变化。修正为并发更新另一条未加锁记录，在 A 中仍读到旧值；随后同一确定性场景通过。首次失败及修复前旧版本的 -4018 证据均保留。

本轮 native-inner-table 定向范围由多个命令完成，包含失败后修正和续跑，不声称一次完整串行运行无失败。生产代码共 18 个文件，净减少 204 行；实例元数据由 1 个 tablet 拆为 5 个，实例固定增加 4 个 tablet，并未按 Namespace 增加这些表。未加入持久缓存、后台任务或新事务协议。测试结束后已将 obtest 的默认二进制恢复为生产版本。
