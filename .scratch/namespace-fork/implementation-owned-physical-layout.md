# 原生目录与物理描述所有权实施

基线：`02699902741361dcd937be7b3ed1e17af839e162`。开始日期：2026-10-13。

用户目标为完整完成以下三项。状态：实施中，尚未验收。

## 完成条件

1. 合并准备和 checksum 使用原生目录，在受保护的目标快照读取定义、索引关系及绑定。不得为了这些读取启动 Namespace 完整服务。上层解释 Namespace，物理执行接收对象及完整输出描述。
2. 创建/首次采用新布局写入时安装可恢复的 tablet 完整描述；mini 使用本地描述；SSTable 保存实际文件完整描述；任务固定输出描述。崩溃、回放、备库独立转储仍能取得正确正文，版本与正文一致。
3. 全部消费者切换后删除派生物理布局历史的发布和读路径、其专用物理引用扫描/变化补扫和正文过滤器。历史目标从现有 schema history 构造；不额外建设 checksum 历史副本。

## 必须保持的约束

- schema 仍归 Namespace；原生读取不依赖 SQL/runtime，不引入 Namespace 1 回退。
- SCN 决定提交可见性，schema 版本标识定义；目录各组成部分来自同一视图。
- 未完成 freeze 的目录根、目录页及 schema 数据必须可恢复地保留；仅登记当前进程读者不够。
- tablet 当前描述表示已安装的写入布局，不承诺等于最新 SQL 定义；完成旧合并不得回退它。
- 文件描述随文件保存与释放。普通查询、复制和文件重写不依赖外部历史正文仍然存在。
- 多次加删列、默认值变化、兼容与重建型 DDL、fork 父子分支布局均须保持数据语义。
- 不新增长生命周期全局正文缓存或自行维护的永久版本字典。
- 8000 分区的普通 DDL 不集中向所有 tablet 安装描述；实际首次使用的每个 tablet 承担本地安装成本。
- 暂不处理此前记录的 minor Last 标记异常；如果它阻塞本任务验证，记录实际影响再决定后续动作。

## 实施顺序与当前核对

- 原生目录和访问保护已具备；checksum 仍通过 `prepare_namespace_login` 获取 SQL proxy/SchemaService，历史定义还依赖 `TableStorageLayouts`。
- freeze 历史保留现接到布局 KV tablet，实例目录页和 schema tablet 需要补齐同一保留协议，然后才可按 F 读原生目录。
- checksum 错误记录已使用本机存储，其接口遗留的 SQL 参数未使用，可直接移除。
- 先完成原生历史读取及 checksum；随后接入本地完整描述和文件描述；最后替换合并目标来源并删除旧布局历史。中间可编译提交不代表完成全部目标。

## 验收

- 冷 Namespace 合并与 checksum：不触发完整服务装载；local/global/FTS 索引、缺失结果、错误结果及同数量分区替换均保持检查。
- F 前后 DDL、目录页 GC、schema MVCC 回收、重启及备库回放后仍可选中 F 的一致定义。
- 新旧布局混合 MemTable，安装提交前后崩溃、日志回收后恢复、备库独立转储。
- fork 跨分支布局、父删除、接管和多次重启；major/meta/mini/minor 输出描述和实际数据匹配。
- 宽表、小事务、频繁 DDL 及 8000 分区定向成本检查。
- 已跑用例与失败复现加入四件套，生产编译和静态检查通过；提交代码/测试/文档，推送现有分支，不创建 PR。按用户要求不跑全量 mysqltest/sysbench。

## 实施证据

### 2026-10-13：原生目录与文件/写入描述的第一段实现

- checksum 历史定义和当前删除证据改用 NativeSchemaCatalog；去掉 `prepare_namespace_login`。错误记录接口删除未使用的 SQL 参数。预期对象枚举暂时仍来自 TableStorageLayouts，未声称第 1 项完成。
- unfinished freeze 的 MVCC 边界扩展到实例根/页和 Namespace 内表；边界先取持久要求，再取实例目录活跃读者，覆盖读者接手时的间隙。当前对 Namespace 内表采用保守的整类保留，物理对象生命周期仍需进一步审计。
- `TabletWriteLayout` 使用 MDS 当前值（无需多版本应用字典）：初始值加入 CREATE 的同一 MDS 事务；后续 DML 计划携带从同一 ObTableSchema 构造的完整描述，DML 前用独立原生事务安装新布局。并发安装在既有 tablet 发布锁下检查并 set，繁忙时重试到请求截止时间，已提交高版本不回退。
- mini 使用已提交 MDS 描述，版本/列数尚未被覆盖时等待，删除按 MemTable 统计生成简化正文的分支。meta 从本地描述和同一布局身份的输入文件取得完整输出描述。后一项尚待补齐专门的 meta 测试与跨分支兼容性审计。
- SSTable 将完整描述编码在已有文件元数据中，缓存 deep-copy 大小包含正文；普通生成、DDL、fork、空文件补洞、碎片整理与备库基线传输都交付/复制描述。复制校验同时比较正文。fork 输入和目的描述不再读取 StorageSchemaHistory。
- 旧 `storage_schema_addr_` 尚未清理；旧布局历史的 major/medium 目标选择、发布、预期对象枚举、引用扫描与过滤器尚未删除。这些仍是剩余目标，当前实现不能作为全部完成的验收。

已取得的动态证据：

| 范围 | 二进制/日志 | 结果 |
| --- | --- | --- |
| 原生 checksum，父子独立 DDL，local/global/FTS | `/data/1/nijia.nj/test/owned-layout-artifacts/seekdb-native-catalog`；`/tmp/owned-layout-native-checksum-fts.log` | PASS；此二进制尚未加入 MDS/SSTable 改动 |
| 启动、fork、孩子首次写入 | `seekdb-write-layout-v1`；`/tmp/owned-layout-write-startup1.log` | PASS |
| 多次默认值修改、加列、崩溃回放、转储、再次重启 | `seekdb-write-layout-v1`；`/tmp/owned-layout-probe1.log` | PASS，保留既有缺列默认值语义 |
| 父子同位置新增 INT/VARCHAR，fork 文件接管后重启 | 同上，实例 `namespace_fork_PROTOTYPE_owned_layout_41yiyfux` | PASS |

构建：`owned-layout-native-checksum-build2`、`owned-layout-write-mds-build2` 通过；初次构建分别暴露 SCN 头文件和 MDS replay 访问权限问题，已修正。meta 切换构建又发现 const 指针释放接口不匹配，已修正，后续构建通过。以上均为真实编译问题，未记录成动态功能失败。

`owned_layout_probe.py` 已加入四件套；补充并发首次安装和数据事务回滚情形，新增 `owned-layout` 定向 profile。整个 profile、备库复制、故障注入、8000 分区成本及完整去历史表后的验证仍未完成。

### 2026-10-13：回放与全文索引失败闭环

- `seekdb-write-layout-v2` 的 `owned_layout_probe.py` 在第一次强杀重启时 SIGSEGV。现场为 `namespace_fork_PROTOTYPE_owned_layout_xkd3oc1k/data.tar.gz`；单独解包回放到 `/data/1/nijia.nj/test/owned-layout-replay-gdb-xxd0qr21` 后 GDB 再现，调用栈记录于 `/tmp/owned-layout-gdb.log`。栈顶为 `TabletWriteLayoutHelper::initialize -> ObStorageSchema::init`；纯数据/纯辅助 tablet 回放已存在对象时跳过了创建 schema 的解析，随后新增初始化解引用空指针。修正为始终解析日志内描述，再决定是否需要创建物理对象。
- 同版 `namespace_checksum_probe.py --fts` 创建全文索引超时。日志明确为 PX 计划序列化空 `ObTableDMLParam` 时 `ObStorageSchema::serialize` 返回 `OB_NOT_INIT`；空参数不拥有布局，序列化/反序列化/大小计算统一按已编码的表身份决定是否携带布局。
- 修复版 `seekdb-write-layout-v3`，SHA256 `2f02d27fb21ad9bff9724a2c66d427624bfaa1242c3ebd84cc93cb51a613a890`，生产编译 `/tmp/owned-layout-write-mds-build6.log` 通过。
- `/tmp/owned-layout-probe3.log` 全部通过：强杀回放、转储后再重启、8 个并发首次写入、数据事务回滚、父子 INT/VARCHAR 分叉、文件接管与第三次重启。实例 `namespace_fork_PROTOTYPE_owned_layout_w4ne8r9g`。
- `/tmp/owned-layout-native-checksum-fts3.log` 通过：父子独立 DDL、本地/全局/全文索引及历史 checksum，F=`1791885812118993018`。实例 `namespace_fork_PROTOTYPE_namespace_checksum_w1a86s5u`。以上两个真实失败场景均已在四件套 `owned-layout` profile 中，修复后的证据不替代最终切换后的重跑。

### 原生合并目标读取（进行中）

- major/medium 不再读取 `StorageSchemaHistory` 正文；组合根为现有 schema 解析模块安装原生目录读取函数，存储仅传物理对象和目标 SCN。
- `NativeSchemaCatalog::begin_read_at` 集中执行登记读者、检查持久保留边界、检查本地可读 SCN。checksum 和合并准备使用相同入口。
- 布局投影只读取表/列记录；不为一个分区构造整张表的分区、约束和触发器列表。
- 当前仅代码已接入，生产编译及动态验证中；预期对象枚举、旧历史发布/回收和旧 tablet 描述仍未移除。

补充证据：`seekdb-native-merge-v4`（SHA256 `d36e3786962d271976c4019ee1e658c8148350654027499c44457837670a0df3`）已通过 `namespace_checksum_probe.py --fts`，日志 `/tmp/owned-layout-native-checksum-fts4.log`，F=`1791886203588881018`。分区 major 的输出描述由原生目录读取，三类索引完成校验。提交 `910955e24` 已推送当前分支，是实施检查点，不代表三项完成。

### 删除派生历史正文（进行中）

- checksum 预期集合由原生目录枚举，布局绑定只提供 G；缺失应有绑定会阻止校验，不会悄悄遗漏对象。版本来自同一 F 的表定义，不再读取布局历史头。
- 已删除生产 `StorageSchemaHistory`、`StorageSchemaHistoryFilter` 和对应构建项；删除为其服务的 `scan_tablet_references`、发布计数和变化对象补扫。布局绑定的创建/DDL 不再发布正文。
- `owned-layout-remove-history-build2` 生产编译通过；`seekdb-no-history-v5` SHA256 为 `d42c0226ce0e1297468f0ac663d52fcffd818945e771e4f95d5b0f11367bde3a`。`owned_layout_probe.py` 全部通过（`/tmp/owned-layout-probe5.log`，实例 `namespace_fork_PROTOTYPE_owned_layout_yund3h8l`）。该版 checksum 与三个备库用例仍在跑。
- 四件套正在迁移旧断言：mini 应保有完整描述；meta 读取本地/文件描述；SSTable 审计读取文件正文。`gate_probe_injection.py --profile owned-layout` 排除已删除历史模块的专用 hook。旧全量 profile 中依赖旧历史 API 的测试仍需改写或按新协议替代，不能宣称全量入口已通过。
- 剩余工作仍包括：旧 `storage_schema_addr_` 的独立正文收敛、恢复/备库/DDL 兼容性覆盖、目录物理对象回收保护的完整审计、旧保留测试替代及四件套/8000 分区成本验收。

补充证据：去历史版的 `/tmp/owned-layout-native-checksum-fts5.log` 已通过，F=`1791886686902667070`；不再有历史正文发布或读取的情况下，本地/全局/全文索引全部完成 checksum。备库 `basic` 通过（116.45 秒），其余两项继续执行。测试版 `seekdb-no-history-probe-v6` SHA256=`9be6dc184a3841ef37c6d1c1c145fd430060ed7a4bfe870459be7a74c0b1dc1c` 已编译，四件套 `bootstrap-native-kv` 正在运行；mini 与普通 fork 的完整描述检查已通过。测试 hook 已全部从生产源码撤除。测试版首轮编译发现审计脚本调用 restore 构造函数的参数顺序未更新，已修正后编译通过。

第二个实施检查点 `a7304f90f` 已推送。撤除 hook 后的生产编译 `owned-layout-no-history-production-build1` 通过；`seekdb-no-history-v7` SHA256=`64af28323e099d1ece17b6b44ab0675d8483524b8d6487a83d1e3c9d89e4fdcf`。四件套生产部分正在 `/tmp/owned-layout-four-gates-v7` 运行，bootstrap 已通过；测试部分在 `/tmp/owned-layout-four-gates-v6` 运行，mini/fork/fork_wide、父空间 meta、孩子 meta 和冷父基线 meta 均通过。三项备库定向测试 runner 已成功退出，结果见 `/tmp/owned-layout-standby-v5/results.json`。当前仍不可判定三项总目标完成。

### 2026-10-13：删除 tablet 独立正文地址

- 删除 `ObTablet::storage_schema_addr_`，以及它的持久化、反序列化、宏块引用和 persister 写入。所有原 `load_storage_schema` 消费者切换为明确的 `load_write_layout`；普通 tablet 从 MDS 当前提交值取得正文，LS 固定表使用与创建相同的代码定义。没有 Namespace 回退。
- 创建和物理恢复直接用显式输入构造 rowkey read info；状态更新、MDS 转储、碎片整理复制已有 rowkey read info，不读取另一个完整正文。合并和接管只安装新文件及读信息，不更新写入布局。删除已无调用的 `update_tablet_storage_schema`。
- SSTable 列描述检查使用各文件自带正文；碎片重写也使用源文件的正文。DDL 完成记录不再清空原始默认值。tablet 尺寸断言（含 rowkey read info）从 1392 更新为实际 1344 字节。
- 首次编译暴露 rowkey 接口静态类型和尺寸断言两个编译错误，修正后通过。最终生产构建 `/tmp/owned-layout-no-tablet-address-build4.log`；产物 `seekdb-no-tablet-address-v8`，SHA256=`7afbc55ad460984d53e121eb2c0e34c738097366c6c3108c781ee97181b64e92`。测试构建 `/tmp/owned-layout-no-tablet-address-native-build1.log` 通过；产物 `seekdb-no-tablet-address-probe-v9`，SHA256=`d282021eb6c51d0d654afc638f38aaed7a000ebf3266266943ee868b8c34743e`。测试 hook 已从生产源码撤除。
- 新生产版 `/tmp/owned-layout-probe8.log` 全部通过：默认值、混合布局、首次安装并发及回滚、三次强杀恢复、父子不同类型及文件接管。`/tmp/owned-layout-native-checksum-fts8.log` 通过，F=`1791888665885354058`，覆盖 local/global/FTS。三项备库在 `/tmp/owned-layout-standby-v8` 运行，已见 basic、namespace_fork_local 通过，最后一项尚待结果。
- 新测试版四件套在 `/tmp/owned-layout-four-gates-v9` 运行，已通过 mini/fork/fork_wide、父子 meta；不可用已通过的前缀代替整套结果。

前一检查点补充结果：

- `/tmp/owned-layout-four-gates-v7.log` 生产 bootstrap/sql/direct/tls 全部通过。direct 内三项备库均通过；8000 分区 5 次采样见 `fork-service-latency-results.json`：fork 中位 3.87 ms，fork 至首次点读中位 0.401 s，首次写提交 10.68 ms，全分区聚合 3.60 s。这是删除旧地址之前的测量，新版本仍需成本验收。
- v6 原生门禁中的 SSTable 混合重写、行日志崩溃恢复通过；随后 `physical_merge_layout_probe` 失败于遗留的 `simplified == 1` 断言。日志实际为完整描述且 before/observed/result 相同；修改为检查完整正文、正确版本与列数，定向重跑 `/tmp/owned-layout-physical-merge-v6-rerun.log` 通过 F 前后父子 DDL、后续 mini、旧 F major 和晚创建 tablet 排除。原失败日志保留。
- v6 剩余原生门禁从 checksum 继续运行于 `/tmp/owned-layout-four-gates-v6-resume`，不重新跑已完成前缀。

仍需闭合：原生目录 F 的 MVCC 转储/合并/重启保留回归及原有故障用例替代；Namespace 删除与目录物理对象回收的并发审计；最终四件套及宽表/8000 分区成本。旧 full profile 的已删除历史模块测试尚未全部迁移，总目标仍未完成。

检查点 `7871d4d17` 已推送。新生产版三项备库最终全部通过（115.54/186.92/139.40 秒），`native_schema_catalog_ddl_probe.py` 也通过，日志 `/tmp/owned-layout-native-ddl8.log`。新版本 8000 分区 5 次采样结果备份为 `owned-layout-v8-latency.json`：fork 中位 3.84 ms，首次写提交 10.80 ms，fork 到首次点读 0.426 s，全分区聚合 3.585 s；同期机器负载不同，不能据此宣称细小耗时差异有统计意义。8000 分区 DDL 发布成本用例加入 owned-layout profile。

v6 剩余故障回归发现真实失败：`shared_transaction_probe.py --faults --case rollback_create --case rollback_owned --case crash_create --case crash_owned --flush-redo --creation-identities` 在 crash_owned 重启后的首次写入超时（4012）。持久现场 `namespace_fork_PROTOTYPE_shared_transaction_6fsywn_r/data.tar.gz`；客户端请求反复在 `ObTabletCreateMdsHelper::check_new_tablet_existence` 返回 EAGAIN，等待 aborted CREATE 的持久清理，不能按“全量故障测试已通过”处理。新 v9 不强制 redo 的单独 crash_owned 通过；强制 redo 的最小场景正在 `/tmp/owned-layout-shared-crash-owned-flush9.log` 验证，尚未确认根因或关闭。完整新测试版原生门禁也仍在运行。

补充：v9 强制 redo 的单独 crash_owned 已通过（`/tmp/owned-layout-shared-crash-owned-flush9.log`），正在按原四个连续故障场景重跑 `/tmp/owned-layout-shared-flush9.log`；单例通过不能证明原失败闭环。8000 分区父子两空间、四条 DDL 的发布成本用例通过（`/tmp/owned-layout-ddl-cost9.log`）。
