# 08 功能扩展机会评审 — 文件安全信任与风险沟通

> 角色:产品经理 + 资深工程师 · 日期:2026-09-30 · 基线:master efd9e63 + v2.1 修改(MainWindow.cpp 564 行)
> 方法:全量阅读必读文档 + `MiniSys\src\core\*.h` + `rules.json` + MainWindow.cpp/Presenters.cpp + 关键 .cpp(JunkScanner/SessionService/GuardRails/QuarantineOp/MoveJunctionOp/DelegateOp/Json/VolumeIndex);**只读**核对用户真实数据 `%LOCALAPPDATA%\MiniSys`(history.jsonl 20 条、minisys.log 97 行)。所有"已核实"均有 grep/阅读依据;推断项均标注依据。

---

## 0. 用户痛点翻译(评审北极星)

用户反馈原文拆成四个可验证的产品缺口:

| 用户的话 | 产品缺口 | 现状证据(已核实) |
|---|---|---|
| "看到按钮和操作,都不知道能不能执行" | 执行前无**可预测性**反馈(哪些会被 GuardRails 拒、哪些是委派) | `SessionService::RunPlan` 执行循环里才 `GuardRails::Validate`,拒绝表现为事后"跳过"弹窗 |
| "被删除的文件是不是系统问题" | 无"这是什么/属于谁"的**身份标注** | ListView 仅 4 列(分类/项目/大小/详情),`ScanItem.riskLevel` 从未渲染(Presenters.cpp `RenderItems` 逐字段核对) |
| "删除是不是风险分析提示,都没有" | 确认弹窗无**风险分组** | `MainWindow::OnExecute` 确认文案 = "N 项 + 总大小 + dangerous 一句警告" |
| "涉及文件安全一定要谨慎优化提示" | 失败无**下一步建议** | 真实记录:npm-cache "MoveFileEx failed (Win32 5)" 失败 2 次(23:51:43、23:52:36 用户重试仍失败),无原因、无建议 |

真实数据佐证(只读核对 `%LOCALAPPDATA%\MiniSys`):
- 2026-09-29 23:51 执行 19 项:17 成功(thumbcache×14 / D3DSCache / cargo / nuget),2 失败(npm-cache 1.6GB,Win32 5);隔离区现持约 1.5GB(仍占 C 盘)。
- 扫描耗时:Junk 30 项 57,371ms;LargeFiles 244 项 69,181ms — **无进度百分比、无取消按钮**。
- 日志 23:48:31 两条静默降级:`rules.json parse failed: bad number at offset 14`(每次启动必现)与 `VolumeIndex: FSCTL_ENUM_USN_DATA failed (Win32 87)` — 索引从未建成,57s 是回退路径的速度。

---

## 1. 现状盘点

### 1.1 现有功能清单表

| # | 功能 | 入口 | 成熟度 | 备注 |
|---|---|---|---|---|
| 1 | 垃圾清理扫描(27 规则:riskLevel/minAgeDays/strategy/detailHint/多 Profile 浏览器缓存) | 垃圾清理 Tab「扫描」 | **成熟\*** | \*外部 rules.json 因 §1.2-D1 的 Json bug **从未加载成功**,实际跑内置回退表(内容与 rules.json 一致,行为无差异,但"可热更新"是假的) |
| 2 | USN 共享索引 + 增量(VolumeIndex) | 内部,Junk 扫描自动 | **骨架** | 代码完整(550 行 + 单测),但真实环境从未生效(Win32 87,见 §1.2-D2) |
| 3 | 大文件扫描 + 三级去重(size→头64KB→全量 SHA-256,[KEEP]/[DELETE] 预选) | 大文件 Tab | 成熟 | riskLevel 恒为默认 Cautious |
| 4 | 应用迁移(复制+永久删源+Junction;四步预检含 RmGetList;迁移后自检) | 应用迁移 Tab | 成熟 | |
| 5 | UWP/WindowsApps 排除迁移 | 自动 | 可用 | `AppScanner.cpp:91` 真正赋值 isUWP(E-1 已修复) |
| 6 | 文件夹分析 + 右键"移入隔离区" | 文件夹分析 Tab | 可用 | 仅顶层目录,无下钻 |
| 7 | GuardRails 安全闸(9 条保护名单/reparse/云占位/TOCTOU 复验/InfoOnly+Advanced 拒直接操作) | 内部 | 成熟 | deny reason 已是中文人话,但只在事后出现 |
| 8 | PlanBuilder + planHash(PlanStale 整单中止) | 内部 | 成熟 | |
| 9 | QuarantineOp 同卷隔离(冲突 -2/-3、Undo→*.restored) | 执行/历史页撤销 | 成熟 | |
| 10 | 清空隔离区(遍历各盘 MiniSys.Quarantine,标记 [已释放]) | 历史页按钮 | 可用 | 全有或全无,无逐条删除 |
| 11 | DelegateOp 委派(DISM/powercfg/cleanmgr,管道捕获) | 垃圾 Tab Advanced 项 | 可用 | 端到端在实施报告手工清单中未勾选 |
| 12 | 回收站清空(Junk 特殊项,EmptyRecycleOp,不可逆) | 垃圾 Tab | 可用 | |
| 13 | DeleteOp(回收站删除,IFileOperation) | **无(孤儿)** | 孤儿 | 全仓 grep:仅 include,无构造点 |
| 14 | OperationLog JSONL + TSV 迁移 + 原子重写 | 内部 | 成熟 | |
| 15 | 历史页一键还原(Quarantine/MoveAndJunction) | 历史页「撤销选中」 | 成熟 | |
| 16 | 状态栏三栏(系统盘空间/迁移目标或隔离区占用/进度文本) | 全局 | 成熟 | migrateTargetRoot_ 会挤掉隔离区占用显示 |
| 17 | Info label 分 Tab 使用说明(非模态 hint) | 全局 | 可用 | v2.1 对话框降噪 |
| 18 | 进度条 | 扫描/执行期间 | 骨架 | 仅 marquee;ProgressFn 的 (cur,total) 被 `RunScan` 丢弃 |
| 19 | 扫描取消 | **无 UI(孤儿方法)** | 孤儿 | `SessionService::CancelScan()` 存在,grep 无调用方 |
| 20 | 崩溃 minidump + 日志 | 内部 | 成熟 | 日志无轮转 |
| 21 | 设置持久化 | **无** | 缺失 | grep 无 WritePrivateProfile/RegSetValue;migrateTargetRoot_/useSymlink_/LargeFiles 参数重启即失 |
| 22 | 单实例保护 | **无** | 缺失 | main.cpp 无 CreateMutex;双开将并发操作隔离区/历史 |
| 23 | 打开所在位置 / 排序 / 关于(TaskDialog 使用说明) | 列表/按钮 | 成熟/可用 | About 文案已建议"先创建系统还原点"但无功能 |

### 1.2 会误导提议的「文档 vs 代码」差异(提议前必读)

- **D1|rules.json 热更新实为从未生效**:`Json::Parser::ParseValue`(util/Json.cpp)在 `ParseObject` 的 `:` 之后**不跳过空白**即判值类型;rules.json 全文 `": value"` 带空格 → 首键 `"version": 2` 即在 offset 14 报 "bad number"(日志逐字吻合)。`JunkRules::Load()` 静默回退内置表。JsonTests 54/54 通过说明无"冒号后空格"用例。**任何基于"改 rules.json 即可调整风险文案/新增规则"的提议都建在沙滩上。**
- **D2|USN 索引(DESIGN-v2 M2 卖点)真实环境从未建成**:`VolumeIndex.cpp:231` `MFT_ENUM_DATA med{}` 零初始化,未设 `MinMajorVersion=2/MaxMajorVersion=3`,Win10 1607+ 返回 ERROR_INVALID_PARAMETER(87)——与日志吻合(推断:零初始化是该错误的典型成因,依据 MSDN 字段要求;需修复实测)。57s 扫描 = 纯 FastWalk 回退。**"共享索引已完成"的文档表述会让人误以为性能问题只剩优化空间,实际是先修 bug。**
- **D3|"detailHint 已有只差 UI 呈现"需要修正表述**:detailHint 已被拼进 `it.detail`(JunkScanner.cpp:324)并写入 ListView 第 4 列——但 `\n` 在 ListView 单行渲染中被截断,**实际不可读**。所以缺口是"呈现退化",不是"数据未到达"。
- **D4|ARCHITECTURE.md 是 v1 基线**:其 R-001(UI 阻塞)/R-004(非原子重写)/R-006(双根重复)在 v2 已修复(执行线程化/原子重写/默认根修复,代码核实);按它提议这些会重复。以 DESIGN-v2-实施报告.md 为准。
- **D5|行数漂移**:MainWindow.cpp 现 564 行(实施报告称 M1 后 474;v2.1 又加了 About/ShowHint 等)。不影响功能结论。
- **D6|riskLevel 覆盖面**:仅 JunkScanner 赋值;LargeFiles/Apps/FolderTree 项恒为默认 `Cautious`、`dangerous=false`(grep 核实)。风险可视化在非 Junk Tab 需派生或诚实留空。

---

## 2. 候选功能卡片总表

成本口径:S=纯 UI/纯函数复用现成零件(≈1-2 天);M=新增小模块或改消息协议(≈3-5 天);L=新子系统(>1 周)。

| ID | 候选功能 | 价值 | 频次 | 成本 | 主要复用 | 风险 | 优先级 |
|---|---|---|---|---|---|---|---|
| F1 | 执行前风险分析摘要(确认弹窗按 riskLevel 分组 + 后果说明) | 5 | 每次执行 | S | ScanItem.riskLevel/strategy/isReversible | 低 | **P0** |
| F2 | 列表风险可视化(a 风险列+徽标 S / b 行染色 M / c 系统组件标注) | 5 | 每次浏览 | S→M | ScanItem、RenderItems、NM_CUSTOMDRAW | 染色过度反而噪声 | **P0**(a)/P1(b) |
| F3 | "为什么安全"解释面板(这是什么/删了会怎样/能否还原) | 5 | 逐项决策 | S | detailHint、ruleId、About 的 TaskDialog 模板 | 低 | **P0** |
| F4 | 错误码→人话映射 + 失败详情面板(含"复制详情") | 5 | 有失败时(实测 2/19) | S-M | LastErr 已带 Win32 号、LastReport | 需把 details 由 wstring 结构化 | **P0** |
| F5 | 占用预检/诊断(Restart Manager 接入隔离区失败路径,列占用进程) | 5 | 失败诊断时 | S | `RestartManagerCheck`(MoveJunctionOp.cpp:33,已实现) | RM 会话开销,须按需触发 | **P0** |
| F12 | 扫描真实百分比 + 取消按钮 | 4 | 每次扫描 | S(取消)/M(百分比) | `CancelScan()` 已有;JunkScanner 尺寸阶段已有 (d,total) | 消息协议扩展 | **P0**(取消)/P1(百分比) |
| F15 | 静默降级可见化("兼容模式"状态提示) | 4 | 异常时 | S | JunkScanner 已知索引/规则来源 | 低 | **P0**(与 H1/H2 绑定) |
| F6 | 空间仪表盘(可释放 X GB,安全 Y/谨慎 Z;隔离区+回收站占用) | 4 | 每次扫描后 | S | QuarantineUsageText/QueryRecycleBin/Results 聚合 | 与状态栏数字一致性 | P1 |
| F9 | 排除清单("永不动这个文件夹",持久化 + GuardRails 双保险) | 4 | 一次设置长期受益 | M | GuardRails、Json::Dump、右键菜单(OnContextMenu 模板) | Json bug 是前置 | P1 |
| F11 | 设置持久化(migrateTargetRoot_/useSymlink_/LargeFiles 参数) | 3 | 每次启动 | S-M | Json::Dump、PathUtils AppDataDir | 低 | P1 |
| F13 | 单实例保护(命名互斥体,二次启动激活已有窗口) | 3 | 低频但后果重 | S | — | 低 | P1 |
| F8 | 系统还原点创建入口(迁移确认弹窗可选勾选) | 4 | 迁移时(低频) | M | OpRecord note、确认弹窗 | SR 服务禁用需降级;占磁盘 | P1 |
| F7 | 清理预览(dry-run:GuardRails.Validate 批量跑,显示将通过/将拒+原因) | 4 | 执行前 | M | `GuardRails::Validate` 纯函数、planHash | 预览≠执行时状态(PlanStale 已兜底) | P1/P2 |
| F17 | 隔离区专属视图(按时间/大小浏览、逐条还原/永久删除) | 3 | 清理确认期 | M | OperationLog、HistoryPresenter | 低 | P2 |
| F18 | "上次清理时间/已隔离过同规则项"提示 | 2 | 低 | S | OperationLog 历史匹配 ruleId | 低 | Backlog |
| F10 | 定时自动清理(仅 Safe 级) | 2 | — | L | — | **违反确认制边界** | **不做**(折中"提醒"进 Backlog) |
| F14 | 托盘常驻/开机任务 | 2 | — | M | — | 与信任定位冲突 | 不做 |
| — | $MFT 直读 / EV 签名(DESIGN-v2 列"不做"的重新评估) | — | — | L | — | 见 §6 | 维持不做(EV=发布前再评) |

### 2.1 重点卡片详评(P0 项)

**F1 执行前风险分析摘要**
- 方案:`OnExecute` 确认弹窗从"N 项/总大小"升级为按 riskLevel × strategy 分组:`安全·可还原 12 项 / 3.1 GB(移入隔离区,历史页可一键还原)`,`谨慎 3 项 / 900 MB(WU 缓存等,删除后系统需重新下载)`,`委派 1 项(WinSxS — 调用系统 DISM,不可自动撤销)`,`不可逆 1 项(清空回收站)`。每组附一句后果话术(静态表,riskLevel→模板)。
- 验收:勾选混合级别项,弹窗出现 4 组计数;`hasDangerous` 警告逻辑保留。
- 理由 P0:直接命中"删除是不是风险分析提示,都没有";数据 100% 在内存,零新增 I/O。

**F4 错误码→人话映射 + 失败详情面板**
- 方案:`QuarantineOp/MoveJunctionOp/DelegateOp` 的错误出口统一过 `Win32ErrorText(DWORD)`(5→"拒绝访问:文件被占用或权限不足,可关闭相关程序(如 node.exe、VSCode)后重试";32→共享冲突;183→目标已存在;3→路径不存在;87→参数错误…)。`ExecuteReport.details` 从 wstring 升级为 `vector<ItemResult>{title, path, err, advice}`,`OnPlanDone` 弹窗逐条展示,并加"复制详情"按钮(写剪贴板,便于发issue/求助)。
- 验收:重放 npm-cache 失败场景,消息含人话原因 + 下一步建议;单测覆盖映射表纯函数。

**F5 占用诊断(Restart Manager 复用)**
- 方案:把 `RestartManagerCheck` 从 MoveJunctionOp.cpp 匿名命名空间提取为 `platform/RestartManager.h`(公开 API:`std::vector<std::wstring> LockingProcesses(path)`);接入 `QuarantineOp::Execute` 失败分支:MoveFileEx 失败且 GetLastError∈{5,32} 时,调 RM 查占用,`errOut` 追加 `被占用: node.exe, Code.exe — 关闭后重试`;RM 查无占用则提示"疑似权限/ACL 问题,可查看属性→安全"。
- 关键取舍:**失败后诊断,不做执行前全量预检**(19 项×RM 会话≈数秒,反噬执行速度;MoveJunctionOp 的预检保留,因为迁移本来就慢且代价高)。
- 验收:npm-cache 场景错误消息含进程名或 ACL 提示;`RestartManagerTests`(可注入假会话或标记 manual)。

**F15 降级可见化**
- 方案:JunkScanner 结束时把数据源状态(规则来源 external/builtin、索引 used/fallback+原因)随 `WM_APP_SCAN_DONE` 透传;UI 在 info label 追加一行:"⚠ 快速索引未启用(原因),已使用兼容模式扫描(较慢)" / "⚠ 规则文件解析失败,已使用内置规则"。修复 D1/D2 后正常机器不再出现,机制留给用户改坏 rules.json 时兜底。
- 理由:工具对自己降级不诚实,就无法要求用户信任它对文件的判断——信任沟通的第一性原则。

### 2.2 前置卫生项(不是功能,是信任链地基,必须先做)

- **H1 修 Json 解析器**:`ParseValue` 入口加 `SkipWs()`(或 `ParseObject` 在 `:` 后跳过空白)+ 回归测试(`{ "a": 1 }`、`[ 1, 2 ]`)。不修则 F9/F11 的持久化、以及"规则热更新"全部失效。
- **H2 修 VolumeIndex**:`MFT_ENUM_DATA med{}` → 设 `MinMajorVersion=2, MaxMajorVersion=3`;修复后实测 Junk 首扫是否达 ≤30s 目标。57s→达标是所有性能护栏讨论的前提。

---

## 3. Top 10 推荐功能(排序 + 实现草案)

> 排序原则:信任主线直接命中 > 真实使用证据 > 复用密度;H1/H2 为第 0 顺位前置。

**#0(前置)H1 Json 修复 + H2 USN 修复 + F15 降级可见化** — 见 §2.2。这三件事一起交付"工具先对自己诚实"。

**#1 F1 执行前风险分析摘要(P0)**
- 数据流:`CollectChecked()` → 内存聚合 `GroupByRisk(items, indices)`(新纯函数,core 或 util)→ 确认文案 → MessageBox(保持模态)。
- UI 入口:`MainWindow::OnExecute`,弹窗本身即入口,零新控件。
- 复用:ScanItem.riskLevel/strategy/dangerous/isReversible 语义、FormatW/FormatSize。
- 测试:`RiskSummaryTests`(GroupByRisk 纯函数:混合级别分组计数、空组不出现);手工:勾选 Safe+Cautious+Advanced+回收站四类,弹窗四段文案正确。

**#2 F2a 列表风险列 + 徽标(P0)**
- 数据流:RenderItems 增加第 5 列「风险」:`● 安全`(绿)/`⚠ 谨慎`(黄)/`⚙ 委派`(灰)/`ℹ 提示`(蓝)/`— 未分类`(LargeFiles/Apps/FolderTree 诚实留空,不做猜测式评分)。
- UI 入口:`Controls.cpp AddCol` + `Layout.cpp` 列宽 + `RenderItems`;颜色用 `NM_CUSTOMDRAW`(MainWindow WndProc 已有 WM_NOTIFY 分支)。系统组件标注:category=="System Component" 的行在项目名后加 "(系统)" 后缀。
- 复用:ListTabPresenter 渲染管线;`Icons::SetStockButtonIcon` 证明 SIID_* 体系可用(如需图标)。
- 测试:27 规则逐一断言列文本映射;手工:LargeFiles Tab 显示"—"。
- 分期:第一版纯文本(半天),行染色(NM_CUSTOMDRAW)第二版——避免一次改太大。

**#3 F3 "为什么安全"解释面板(P0)**
- 数据流:ListView 双击(LVN_ITEMACTIVATE)或右键菜单「为什么安全?」→ 按 `ruleId` 反查规则表(JunkRules::Load 已有,修复 H1 后才是真外置)→ TaskDialog 三段式:**这是什么**(title + category + 路径)、**处理后会怎样**(detailHint + 按 riskLevel 的话术:Safe="缓存,程序下次运行自动重建";Cautious="删除后系统/程序需重新下载,期间可能变慢";Advanced="仅调用系统官方命令,不会直接删文件")、**能否还原**(strategy==Quarantine→"可,历史页一键还原";Delegate→"不可自动撤销";InfoOnly→"仅提示项,不可执行")。
- UI 入口:右键菜单(OnContextMenu 模板在 FolderTreePresenter 已有)+ 双击。
- 复用:About 的 TaskDialogIndirect 调用模板、JunkRule.detailHint、ruleId。
- 测试:27 规则每条弹出非空三段;无 ruleId 的项(LargeFiles)显示通用说明("非规则项,请自行判断,操作仍受安全闸保护")。

**#4 F4 错误人话 + 失败详情面板(P0)**
- 数据流:新建 `util/Win32Error.h: Win32ErrorText(DWORD, context)` 纯函数映射表(~15 个高频码);`ExecuteReport::details` → `std::vector<ItemResult>`;`OnPlanDone` 弹窗分组渲染 + `复制详情`(Clipboard API)。
- UI 入口:执行完成弹窗(已有)+ 历史页 note 字段同步人话。
- 复用:QuarantineOp::LastErr 已携带错误号;OpRecord.note 落盘管道现成。
- 测试:映射表单测;集成:模拟占用(测试中打开文件句柄再 quarantine)断言消息含建议文案。

**#5 F12 取消按钮 + 真实百分比(P0 取消 / P1 百分比)**
- 取消(S):扫描/执行期间把「扫描」按钮变为「取消」(或并列小按钮)→ `svc.CancelScan()`。引擎已存在,纯接线。注意:JunkScanner 取消时 `return` 丢弃部分结果,行为=放弃本次扫描,UI 提示"已取消"即可,不改扫描器。
- 百分比(M):`ProgressFn` 的 (cur,total) 在 `RunScan` 透传:progress lambda 存原子 pair → `WM_APP_SCAN_PROGRESS` 的 wParam 带 percent → `PBM_SETPOS`(range 0-100 已设,Controls.cpp:67)。Junk 尺寸阶段立即有真实数据(d/dirs.size(),JunkScanner.cpp:285);索引构建/遍历阶段退回 marquee。
- 测试:SessionServiceTests 增:进度回调值被正确暂存;取消后 IsBusy 归位、不崩溃。

**#6 F5 占用诊断接入隔离区(P0)** — 见 §2.1。

**#7 F6 空间仪表盘(P1)**
- 数据流:Junk Tab 扫描完成后,info label 上方或其首行输出聚合:`可释放 4.2 GB(安全 3.1 / 谨慎 1.1);隔离区已占 1.5 GB 待确认释放;回收站 2.0 GB`。执行/清空后事件驱动重算(仅内存聚合 + QueryRecycleBin 一次轻量调用,缓存至下次事件失效)。
- 复用:`QuarantineUsageText`(已有)、`QueryRecycleBin`(已有)、Results 聚合。
- 测试:聚合纯函数单测;手工:执行成功后数字扣减一致。

**#8 F9 排除清单 + F11 设置持久化(P1,一并交付)**
- 数据流:列表右键「永不清理此文件夹」→ `%LOCALAPPDATA%\MiniSys\settings.json`(Json::Dump 序列化:`exclusions[]`, `migrateTargetRoot`, `useSymlink`, `largeFiles{minMB,ext,drives}`)→ 启动加载;JunkScanner 展开规则时跳过命中前缀;GuardRails::Validate 对排除路径 deny(reason="用户排除清单")。双保险:扫描层不出现 + plan 注入被拒。
- UI 入口:右键菜单 + 历史页「管理排除项…」(简单 ListBox 对话框)。
- 前置:H1(否则 json 读不回来);GuardRails 单例需支持运行期追加名单(现 ProtectedPaths 是 static,加一个 mutable vector + mutex)。
- 测试:排除后重扫该项消失;注入测试:plan 中带排除路径被拒;settings 往返(写→读)相等。

**#9 F8 系统还原点入口(P1)**
- 数据流:Apps Tab 确认弹窗加复选框「执行前创建系统还原点(推荐)」→ `platform/SystemRestore.h` 封装 `SRSetRestorePointW`(BEGIN_SYSTEM_CHANGE+MODIFY_SETTINGS,序列化要点:两次调用间需隔 10s 系统 FIFO 限制需处理)→ 结果写 OpRecord.note("已创建还原点: xxx")或失败降级提示(不阻断迁移)。
- 测试:服务启用机手工验收(系统还原 UI 可见新点);服务禁用机降级文案。
- 顺带删除 About 文案与功能不一致的尴尬(现状文案建议用户手动做,产品却没做)。

**#10 F13 单实例(P1,半天)**
- 数据流:main.cpp 入口 `CreateMutexW(L"Local\\MiniSys.SingleInstance")`,ERROR_ALREADY_EXISTS → `FindWindowW(kWindowClass)` + `SetForegroundWindow` → 退出。防管理员双开并发踩隔离区/历史(OperationLog 有 mutex 但 UpdateStatus 全文件重写,双开仍有交错窗口)。

---

## 4. 分阶段路线图

| 阶段 | 内容 | 主题 |
|---|---|---|
| **Now(信任补齐,约 1.5 周)** | H1 Json 修复+回归测试;H2 USN 修复(实测 57s→?);F15 降级可见化;F1 风险摘要弹窗;F2a 风险列(文本版);F3 解释面板;F4 错误人话+失败详情;F5 RM 占用诊断;F12a 取消按钮 | "每个按钮都说得清后果,每个失败都给得出下一步" |
| **Next(体验完善,约 2 周)** | F12b 真实百分比;F2b 行染色;F6 仪表盘;F9+F11 排除清单+设置持久化;F13 单实例;F8 还原点入口;历史页 note 人话化 | "可预测、可个性化、可回退" |
| **Later(约 2 周)** | F7 清理预览(dry-run);F17 隔离区专属视图+逐条永久删除;D3 修复(detail 列 \n 截断→tooltip 或详情面板);FolderTree 下钻分析 | "从解释到演练" |
| **Backlog** | F18 上次清理提示;定时清理的**提醒形态**(开机检查隔离区积压→通知,不自动执行);日志轮转;托盘最小化;发布前 EV 签名评估 | |

---

## 5. "不该做"清单及理由

1. **定时自动清理(自动执行版)** — 需求.txt 第 3 条"所有的迁移和删除都需要我确认才能执行"是用户亲手写下的边界;自动执行哪怕只限 Safe 级也动摇"隔离区可逆 + 确认制"的产品根基。折中:提醒形态进 Backlog。
2. **$MFT 直读** — 维持 DESIGN-v2 决策。理由:H2 修复后性能问题先实测(57s 大头是回退模式;USN 命中后 size 精扫只剩命中子树);MFT 直读省的是枚举不是 size 查询;2-3 周投入 + 解析崩溃风险,与本轮"信任"主线零贡献。
3. **EV 签名(现阶段)** — 需组织身份与外部流程,自用阶段零收益。触发条件重新打开:决定对外发布时(杀软误报确实伤害信任,但那是发布期问题)。
4. **托盘常驻/开机任务** — 后台常驻与"每次操作亲自确认"的信任叙事直接冲突;一个清理工具常驻后台会让用户更怀疑它在暗地做什么。
5. **删除类(回收站)自动撤销** — DeleteOp 已是孤儿,QuarantineOp 是默认且更优策略。不值得为逆向 SHFileOperation 回收站路径(不稳定)投资;正确动作是维持现状或删除死代码。
6. **执行前全量 RM 预检** — 每项一次 RM 会话,批量执行反白数秒;失败后诊断(F5)以 1/10 成本覆盖 90% 场景。
7. **LargeFiles/Apps 的启发式"重要性评分"** — 猜测式风险标注("这个 exe 可能重要")一旦错一次,信任归零。诚实呈现"未分类,需人工判断 + 安全闸兜底"是更好的答案。

---

## 6. 性能护栏(不得加重 57 秒扫描)

1. **零扫描期新增 I/O**:F1/F2/F3/F4/F6 全部只消费内存中的 ScanItem/OpRecord/静态话术表;风险文案、后果说明一律静态查表,禁止扫描时读文件。
2. **按需触发**:F5 RM 诊断仅在失败分支;F7 预览仅在用户点击时批量跑 `GuardRails::Validate`(每项 1-2 次 `GetFileAttributesExW`,30 项 <100ms)。
3. **事件失效缓存**:F6 仪表盘聚合结果缓存,仅在执行完成/清空隔离区/重扫事件后重算;`OperationLog::LoadAll` 是全文件读,现阶段 5KB 无碍,日志增长后应内存缓存 + 事件失效(列入 Later 注意项)。
4. **UI 线程纪律**:所有新计算保持 O(n) 内存遍历;百分比更新走既有 `WM_APP_SCAN_PROGRESS` PostMessage 节流(避免每文件 PostMessage 风暴,JunkScanner 尺寸阶段按目录粒度已天然节流)。
5. **先修再优**:H2(USN)落地前,任何"再优化扫描"的新功能提案一律冻结——先验证修复后首扫是否达 ≤30s 设计目标,避免在错误基线上叠加复杂度。

---

## 7. 特别加分:用现有零件即可拼出的功能(复用密度清单)

| 零件(已存在) | 只差什么 | 拼出 |
|---|---|---|
| `ScanItem.riskLevel/strategy/dangerous`(JunkScanner 已赋值) | Presenters/MainWindow 渲染一行 | F1、F2a |
| `JunkRule.detailHint`(已拼进 detail 但被 \n 截断) | TaskDialog 呈现(About 模板现成) | F3 |
| `SessionService::CancelScan()`(完整实现,无调用方) | 一个按钮 + 一行调用 | F12a |
| `RestartManagerCheck`(MoveJunctionOp.cpp:33,已实现) | 提取到 platform/ + 失败分支接入 | F5 |
| `QuarantineUsageText()`/`QueryRecycleBin()`/`QueryDiskSpace()` | 聚合 + 一行 UI | F6 |
| `GuardRails::Validate`(无副作用纯校验) | 批量入口 + 结果面板 | F7 |
| `TaskDialogIndirect`(About 已用) | 内容参数化 | F3/F4 面板 |
| `Json::Dump/Parse`(序列化现成;Parse 有 H1 bug) | 落盘读写 + 字段定义 | F9/F11 |
| `LastErr` 错误号已进 errOut | DWORD→文案映射表(纯函数) | F4 |
| 右键菜单模板(FolderTreePresenter::OnContextMenu) | Junk 列表复用 | F3 入口、F9 入口 |

结论:Top 10 中 7 项(F1/F2a/F3/F4/F5/F6/F12a)的核心是"接线"而非"造零件",这正是数据已到 UI 层、只差最后一公里的典型状态。

---

## 8. 未核实项

1. **H2 根因**:`MFT_ENUM_DATA` 零初始化 → Win32 87 为代码推演(依据:MSDN 要求 Win10 1607+ 设 Min/MaxMajorVersion,零值非法;与日志错误码吻合),未实际运行验证;修复后需实测首扫耗时。
2. **npm-cache Win32 5 确切成因**(进程占用 vs ACL):F5 上线后由 RM 诊断结果确认;当前只能确定"重试一次仍失败"。
3. JsonTests 用例内容未逐条审阅;"无冒号后空格用例"是从 54/54 通过 + 线上必现失败反推(推断)。
4. LargeFiles 69s 的耗时分布(FastWalk vs 三级哈希)无分阶段埋点数据;F12 百分比设计前建议先加分段计时日志。
5. thumbcache 被 Explorer 使用却隔离成功的机制(rename 对 share-delete 打开的文件生效?)未验证,不影响功能结论。
6. 实施报告"手工验证清单"7 项均未勾选(含 DISM 端到端、二扫 ≤5s),这些验收状态以文档自述为准,未复测。
7. `MiniSysTests` 12 文件 54 用例的具体断言未逐条阅读,本报告测试策略以模块名为依据。
