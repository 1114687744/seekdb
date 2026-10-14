# Namespace 状态与后台任务的所有权约束

日期：2026-10-14。状态：本轮讨论确认的设计原则，尚未实施。范围包括业务 Namespace 和实例管理空间；不把以下目标记为当前代码已经具备的行为。

## 所有权与执行资源

- Namespace 持有自己的服务对象、可变状态及其生命周期。保留服务对状态和操作的封装，不为改名而拆散接口。
- 工作线程和定时执行设施由进程持有。Namespace 可以持有任务状态和定时登记，不创建随自身启停的专属后台线程。
- 固定且不可变的内表定义共享一份；动态定义、版本及权限按所属空间隔离。缓存可以共用容量和底层结构，对外使用明确绑定所属空间的访问对象。
- 实例管理连接与业务连接使用统一的 SQL 访问模型，登录时确定身份和目录。实例管理空间不参与业务 Namespace 的 fork；NS 1 不作为默认管理上下文。复用现有 SQL 的具体依赖改造仍需落实。

## Namespace 上下文必须显式传递

本条是整个 Namespace 架构的约束，不限于后台线程或句柄的选择。不引入 Namespace 版 MTL：线程不承载“当前 Namespace”，任何模块都不能依靠全局入口、线程局部变量或环境状态取得所属空间。即使通过 RAII 作用域临时设置并在返回时恢复 TLS，也不满足要求。

约束覆盖客户端 SQL、内部 SQL、任务投递与重试、RPC/异步回调、回放恢复、异常清理和日志中的 Namespace 身份。显式传入 session/context 后读取其成员属于显式依赖；从 `THIS_WORKER` 或其他 TLS 找到 session 再推导 Namespace 属于隐式依赖。

1. Namespace 身份、服务对象、访问目标和使用保护，必须来自函数参数、构造注入，或由这些显式输入建立的会话/任务成员。
2. 不通过全局变量、全局服务查找或全局 Registry 取得当前 Namespace；不从线程局部变量、`THIS_WORKER` 的隐式 session 或当前线程作用域推导 Namespace、权限或数据路由。
3. 登录、任务恢复等负责解析外部身份的入口，可以通过显式注入的目录/注册接口取得目标句柄；绑定后将句柄传递下去。后台线程不能仅携带 `ns_id`，执行时再到全局 Registry 寻找对象。
4. 创建异步、延迟和重试任务时，显式传递所属 Namespace 句柄和任务参数。普通 `ns_id` 不能代替有效对象引用、权限或生命周期保护；捕获服务裸指针也不构成保护。
5. 排队和延迟任务可持有弱引用或可失效句柄。任务的存储由共享调度设施保持有效，不能依赖已经可能销毁的 Namespace 内嵌回调对象。统一执行入口先取得 Namespace 使用保护，再访问其服务状态。
6. 统一执行入口持有保护直到本次工作及其必要收尾结束；同步子调用借用有效对象引用。后续异步工作必须显式传递自己的所属句柄，不能继承线程上遗留的上下文。
7. 缺少所属对象、句柄失效或 Namespace 已关闭时，按接口返回错误或终止任务；不得回退 NS 1。bootstrap 的访问目标和权限也须明确提供。
8. Namespace 解释和继承解析留在上层。共享物理存储接收已绑定的物理目标、事务和快照，不反查 Namespace。

注册表由实例对象持有，并作为明确依赖传给负责登录、恢复和管理的入口；不以 `namespace_registry()`、`get_instance()`、宏或“当前 NS”辅助函数作为模块访问 Namespace 的捷径。共享物理设施同样以明确依赖提供，不能借它们隐式恢复 Namespace 上下文。缺少显式参数时，应修改调用链补齐，而不是增加一个临时 TLS 兼容通道。

示意接口，不表示当前已有这些类型：

```cpp
struct RefreshTask {
  NamespaceWeakHandle owner;
  int64_t schema_version;
};

void execute(const RefreshTask &task)
{
  auto use = task.owner.try_acquire(); // 同时检查准入状态并建立使用保护
  if (!use) { return; }
  refresh_schema(use.schema(), task.schema_version);
} // 本次执行结束，归还保护；共享线程继续处理其他任务
```

`owner` 由任务创建者显式提供。执行入口不查全局 Registry、不设置“当前 Namespace”线程变量；具体操作取得的服务引用由外层使用保护覆盖。

## 删除规则

- 使用保护统一针对 Namespace 状态容器，不为每个随其整体生灭的服务重复建立删除计数。持有保护期间，这些对象不能被单独销毁。
- 初期采用正在使用就返回忙的删除语义。检查使用情况与关闭准入必须原子完成；删除失败恢复准入。正在运行的会话、任务和必要异步收尾均计入使用范围。
- 关闭后撤销该 Namespace 的定时和待执行登记；已派发任务无法再取得使用保护，并且不能继续自我调度。排队任务不能长期持有强引用，使一个未来任务一直阻止删除。
- 对象存活保护不替代并发锁，也不替代 schema 版本、缓存项和物理文件现有的独立引用机制。后代数据引用由物理保留规则处理，不要求被删 Namespace 的服务对象永久驻留。

## 已核对的实现差距

- `ObTimer` 已委托给共享 `ObTimerService` 和工作线程池。计划/PS 缓存、锁 GC、DDL 恢复分别持有 timer 对象，不等于各自拥有线程。
- `ObMultiVersionSchemaService::init` 内部启动 `ObDDLTransController` 线程；需将等待刷新循环改为提交有界工作，保留所属 Namespace 的待刷新版本状态。
- `ObDBMSSchedService` 启动独立调度线程；需把长期循环改为按到期时间/事件触发的一轮调度，不能把原循环直接放入共享池长期占用 worker。
- `namespace_worker_protocol_prototype.h` 中的 `in_process_serving_ns`、`in_process_storage`、`worker_global_storage_scope_depth` 以及基于它们的 `serving_namespace`/权限/存储范围推导，属于需要替换的隐式入口。经 `THIS_WORKER.get_session()` 取得 Namespace 服务的路径也在审计范围内。
- `ObDBMSSchedJobMaster::run_job` 异步捕获 `sql_proxy` 和 `schema_service` 裸指针，尚未具有上述任务保护协议。
- `NamespaceRegistry::remove` 只取消注册，服务状态保留到进程退出。统一使用保护、任务撤销和对象释放均未完成。

验收应覆盖：任务创建和执行处于不同线程、同一 worker 连续处理不同 Namespace、任务延迟到删除后才执行、执行中删除返回忙、删除失败后恢复准入、任务重试不再唤醒已删除 Namespace。验证改造的路径不能靠设置 TLS 或在缺少上下文时查询全局 Registry 才通过。
