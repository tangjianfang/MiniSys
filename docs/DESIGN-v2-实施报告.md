# MiniSys v2.0 实施报告 — DESIGN-v2.md 评估与落地

> 日期:2026-09-29 · 基线:efd9e63(v1)· 实施后:54/54 单元测试通过,Release/Debug x64 构建通过
> 配套文档:[ARCHITECTURE.md](ARCHITECTURE.md)(v1 基线架构)· [DESIGN-v2.md](DESIGN-v2.md)(设计)

---

## 1. 评估结论

**总体判定:设计可实施,按推荐路线(M0→M1→M2→M3→M4)全部落地。** 三条不变量(共享索引、PlanBuilder+GuardRails 前置、隔离区默认策略)在代码中成立;`M0→M1 安全优先、M2→M3 性能其后`的顺序被遵循。

### 1.1 代码全量审阅对设计文档的事实纠正

| # | DESIGN-v2 表述 | 实际代码情况 | 处置 |
|---|---|---|---|
| E-1 | §2.1 "AppScanner(Uninstall 注册表 + **UWP 识别**)" | `AppInfo.isUWP` 字段从未赋值(死字段),v1 无任何 UWP 识别 | M3 以 WindowsApps 路径排除实现,并真正赋值 isUWP |
| E-2 | 未提及 | **v1 存在 R-006 缺陷**:`DefaultRoots = {%USERPROFILE%, C:\}` 双根 + Users 不在排除表 → 用户目录每个大文件被枚举两次;去重阶段把同一文件判为"重复对",[DELETE] 行实为唯一副本 | M1 双重修复:默认根改为仅系统盘 + PlanBuilder 拒绝同路径重复项 |
| E-3 | 未提及 | **build.bat 第 9 行 `if "%PLATFORM%"=""` 缺 `=`,cmd 语法错误使脚本整体中止**(实测复现,exit 255)——v1 无法用 build.bat 构建 | 已修复(`==`) |
| E-4 | §12 ADR-007 "TSV→JSONL(启动时一次性迁移)" | 与 R-003(隔离区成孤儿)相关:history 即隔离区 manifest,迁移设计成立 | 按 ADR-007 实现,含 .bak 保留 |

### 1.2 设计待确认项的落定

| DESIGN-v2 §15 待确认项 | 落定 |
|---|---|
| 隔离区清空策略 | 按方案默认:手动按钮 + 状态栏常驻占用提示 |
| 隔离区位置 | 按源文件所在卷(`<盘>:\MiniSys.Quarantine`)——保证 rename 严格同卷、原子且瞬时;C 盘瘦身语义与设计 ADR-005 一致("清空才真释放") |
| $MFT 直读(stretch) | **不做**(设计允许)。USN 枚举 + FastWalk 精扫已满足目标;LargeFiles/FolderTree 全量 size 维持 FastWalk(与 ADR-004 一致) |
| 定时自动清理 | 不列入(超出本轮) |
| 对外发布 | 未定。M4 仅完成本地可行部分(见 §3) |

---

## 2. 里程碑交付与验收

### M0 解耦重构(行为零变化)

| 项 | 结果 |
|---|---|
| SessionService 抽取(扫描线程/进度/结果) | ✅ `core/SessionService`,UI 仅经 StartScan/PostMessage 交互 |
| MainWindow 拆分 | ✅ `ui/Presenters`(Junk/LargeFiles/Apps 共享 ListTabPresenter + FolderTree + History)、`ui/Controls`、`ui/Layout`;MainWindow.cpp **915 → 399 行**(M0 时点,<400 达标;M1 计划执行流程加入后最终 474 行) |
| ScanItem 扩展 | ✅ `ruleId` / `lastWriteFiletime` / `riskLevel`(M1 追加 `strategy`/`command`) |
| GoogleTest 测试工程 | ✅ vendored googletest 1.15.2(third_party/),`MiniSysTests` 工程 |
| 单测 | ✅ SessionService 生命周期(busy 拒绝/取消/异常隔离/分 Tab) |
| 行为零变化 | 代码级保真(v1 逻辑原样迁移);GUI 手工回归待用户执行(见 §4) |

### M1 安全与可逆

| 项 | 结果 |
|---|---|
| GuardRails | ✅ 保护名单(新增 $WinREAgent/Recovery/隔离区根)、reparse/云占位拦截、执行前逐条复验(文件:size+mtime;目录:mtime,规避子树 size 不可比)、InfoOnly/Advanced 拒绝直接文件操作(ADR-008) |
| 注入测试(验收) | ✅ System32/Windows/WinSxS/WindowsApps/SVI 等进 plan **全部被保护名单拒绝**(测试断言拒绝原因为"保护") |
| QuarantineOp | ✅ 同卷 `MoveFileExW(MOVEFILE_WRITE_THROUGH)`、冲突名 -2/-3、Undo(冲突还原为 `*.restored`) |
| PlanBuilder | ✅ planHash = SHA-256 前 8 字节(经 `Hash64OfBuffer`,自研零依赖,替代 xxHash);重扫后整单中止(PlanStale) |
| rules.json | ✅ 外置 + 内置回退(R-005 覆盖);27 条规则:Safe 15(含浏览器**全 Profile**、开发缓存 7 类)、Cautious 7(含微信/QQ 接收文件 age>30d)、Advanced 3(委派)、Info 2 |
| OperationLog JSONL | ✅ 新写 JSONL、旧 TSV 透明读取 + 启动一次性迁移(.bak 保留)、UpdateStatus **原子重写**(temp+rename,修复 v1 R-004) |
| DeleteOp → IFileOperation | ✅ COM 实现,FOFX_RECYCLEONDELETE;DeleteOp 降级为可选策略 |
| 执行线程化 | ✅ 扫描/执行/清空共用 SessionService 串行队列(worker 线程 CoInitializeEx),修复 v1 R-001(UI 阻塞) |
| 一键还原(验收) | 撤销链路代码 + 单测(命名/路径/冲突);端到端需 GUI(§4) |

### M2 共享索引

| 项 | 结果 |
|---|---|
| VolumeIndex | ✅ `FSCTL_ENUM_USN_DATA` 全卷枚举(V2/V3 记录双布局解析)、名字池 + FRN 映射 + childrenOf + 小写 dirPaths、懒拼路径、`THREAD_MODE_BACKGROUND` I/O 礼让 |
| 失效检测 | ✅ 卷序列号比对(每次 EnsureBuilt 廉价验证);journal ID 支撑增量 |
| JunkScanner 消费索引 | ✅ 三种规则模式均有索引路径;未知路径/其他盘/无索引 → 回退 v1 文件系统遍历 |
| 回退验证 | ✅ 非提权集成测试:USN 枚举失败 → EnsureBuilt=false → FastWalk 行为 = v1 |
| 性能目标(≤30s/≤5s) | **待实测**(设计目标;日志已埋 `Scan tab=%d done: %zu items in %lld ms` 基准点) |

### M3 增量与补全

| 项 | 结果 |
|---|---|
| USN 增量 | ✅ `FSCTL_READ_USN_JOURNAL` 自游标应用 create/rename/delete(墓碑标记 + 重新 Finalize);变更 >20000 条自动回退全量重建;journal ID 不符回退 |
| DelegateOp | ✅ 管道捕获输出、退出码映射、命令行解析;WinSxS→DISM、hiberfil→`powercfg /h off`、Windows.old→cleanmgr;执行路径先于 GuardRails 文件校验路由(仅规则表白名单命令) |
| 迁移后 junction 自检 | ✅ `ReadReparseTarget` 回读比对,失配记 Interrupted |
| UWP 排除迁移 | ✅ WindowsApps 路径识别并跳过 |
| $MFT 直读 | 按设计决策不做 |

### M4 发布质量(本地可行部分)

| 项 | 结果 |
|---|---|
| 崩溃 minidump | ✅ `SetUnhandledExceptionFilter` + `MiniDumpWriteDump` → `%LOCALAPPDATA%\MiniSys\dumps\` |
| 性能基准记录 | ✅ 扫描耗时入日志(数据源即基准报告输入) |
| EV 签名 / 杀软白名单申报 / 图标 / 安装包 | ❌ 不可本地实施(EV 证书需组织身份与外部流程);建议发布前再评 |

---

## 3. 偏差与实现决策记录

| 偏差 | 理由 |
|---|---|
| planHash 用 SHA-256 前 8 字节而非 xxHash64 | 复用既有 BCrypt 封装,零新依赖(ADR-006 精神优先) |
| "持久化 USN 光标"未落盘 | 游标仅在配合索引持久化时才有意义;当前索引为会话内内存态,M3 增量在同一会话内消费游标 |
| LargeFiles/FolderTree 仍 FastWalk 全量 | 设计 ADR-004 明确 M3 前 size 全量维持 FastWalk |
| Presenter 为 5 个但 Junk/LargeFiles/Apps 共享 ListTabPresenter 基类 | 消除三份相同渲染代码;每 Tab 仍独立实例(扫描器构建/排序状态隔离) |
| M0 时点 MainWindow 399 行,M1 后 474 行 | M1 加入计划执行/隔离区清空的 UI 流程属新增功能,非重构回退 |
| v1"扫描完成落当前 Tab"的隐患(切 Tab 后结果错位) | SessionService 改为落**发起扫描的 Tab**(修正性偏差,已记录) |

---

## 4. 手工验证清单(需 GUI + 管理员,自动化无法覆盖)

- [ ] 5 Tab 手工回归(M0 验收项):扫描/排序/勾选/确认弹窗/打开位置/迁移目标选择
- [ ] Junk 扫描 → 勾选 Safe 项执行 → 状态栏出现"隔离区: N 项 / X" → 历史 Tab 一键还原成功
- [ ] "清空隔离区"→ C:\MiniSys.Quarantine 消失、历史记录标记"已释放"、系统盘可用空间增加
- [ ] 勾选 WinSxS(Advanced)→ 确认后 DISM 组件清理端到端执行(耗时数分钟属正常)
- [ ] 历史页确认旧 v1 history.tsv 已迁移为 history.jsonl(原文件保留 .bak)
- [ ] 二次 Junk 扫描:日志确认索引未重建(增量路径)、耗时对比首次(目标 ≤5s)
- [ ] 迁移一个应用 → 原位置 Junction 可用(应用可启动)→ 历史撤销 → 应用还原

---

## 5. 变更集概览

- 修改 18 文件(+892 / −956):MainWindow 瘦身、4 个 Scanner/3 个 Op 重构、OperationLog 重写、main(崩溃钩子)、resource/build.bat 修复
- 新增:`core/`(SessionService、PlanBuilder、GuardRails、QuarantineOp、JunkRules、VolumeIndex、DelegateOp、TabId)、`ui/`(Presenters、Controls、Layout、UiHandles)、`util/Json`、`platform/CrashDump`、`MiniSys/rules.json`、`MiniSysTests/`(12 个测试文件,54 用例)、`third_party/googletest`
- 源码总量:v1 约 3,000 行 → 约 7,700 行(含测试,不含 third_party)
- 测试:`build\Release\MiniSysTests.exe` — **54/54 通过**;构建:`build.bat Release|Debug x64` 均通过
