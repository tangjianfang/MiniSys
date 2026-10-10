# MiniSys 第四轮评审 · 06 软件工程与可扩展性(架构角色首评)

> 日期:2026-10-11 · 对象:HEAD `5cda187`(v2.14)+ 工作树(MiniSys.vcxproj 有未提交改动,不影响本报告结论)
> 方法:只读静态分析。脚本与原始输出:`C:\Users\tjf\AppData\Local\Temp\minisys-review-r4\06-exp\`(01_loc.py、02_funcs.py、03_includes.py、04/05_vcproj 漂移、06_summary.py);git/wc/grep 佐证。未构建、未运行测试、未启动 exe、未读用户数据。
> 必读文档已通读:ARCHITECTURE.md、DESIGN-v2.md、REVIEW_2026-10-06-3.md、evolve-log.md、需求.txt、DESIGN-v2-实施报告.md。

---

## 0. 摘要

**健康度评分:6 / 10(工程结构与可扩展性维度)。** 核心业务链(GuardRails/PlanBuilder/QuarantineOp/JunkRules)分层清晰、include 图无环、契约集中度高,这是 v2 重构留下的良好底子;但 v2.3→v2.14 的 12 次功能提交让 **MainWindow 回弹至 1975 行(M0 目标 <400 的 4.9 倍)**、SessionService/VolumeIndex 长成双 God object、测试工程手抄源码清单再次成为事故点、新增设置项的成本涨到 5 文件 2 提交。本轮还静态确认了 1 个 v2.12 引入的功能性缺陷(见 Top10 #1)。

### Top10 发现

| # | 级别 | 发现 | 证据 |
|---|---|---|---|
| 1 | **P1 缺陷(新确认)** | `RootsDlgProc` 与 `OnDevCacheRoots` 各有一处 **`flush;` 漏写调用括号(无操作语句)**:多行输入的多个根目录被合并为一个不存在路径后整体丢弃——"额外扫描根"编辑框实际只能保存 1 个根;v2.12 引入至今未被任何测试/编译告警拦截 | MainWindow.cpp:877、:1011;/W4 但 `TreatWarningAsError=false` |
| 2 | P1 结构 | MainWindow.cpp 1975 行,`WndProc` 361 行 / 启发式 CC 127,含 16 处 `dynamic_cast` 下探 presenter;DESIGN-v2 M0 "<400 行" 验收已静默失效 | 01/02 脚本;git 无后续记录 |
| 3 | P1 结构 | 双 God object:`SessionService`(扫描/执行/索引/搜索/空闲维护/缓存/进度/撤销/verify/preview ≈ 10 个职责簇,单例 1065 行)与 `VolumeIndex`(注册表+USN 构建+walk 回退+磁盘缓存+查询+搜索引擎+MAC+测试缝合 ≈ 8 簇,1373 行) | SessionService.h 全文、VolumeIndex.h 全文 |
| 4 | P1 可测性 | MiniSysTests.vcxproj **手抄 25/37 个 app .cpp**;`JunkScanner.cpp` 在 v2.13(12dd8da,2026-10-07)才首次加入测试构建——此前 7 个版本从未被测试编译(`git log -S` 证实)。清单型工程的漂移风险已被历史证实,当前虽无漂移(核验 37/37 一致) | 04/05 脚本 + git |
| 5 | P1 可测性 | **UI 层 0 单测**:MainWindow + ui/*(6 文件 3466 行)不在测试构建;全仓 38.6% 的 .cpp(4272/11076 行)无测试编译覆盖;ui/Presenters.cpp 891 行(勾选态/视图态保持等纯逻辑)全部只能靠活体回归 | 06_summary.py |
| 6 | P2 扩展性 | 新增 Tab 的实测成本 = 16 文件 / +529 行(v2.3 Search);新增带 UI 的设置项 = 5 文件 + 2 次提交 + 102 行手写模态对话框(devCacheRoots,v2.11–2.12);无设置页框架 | `git show 5eae1d2 / 43e5302 / 4c55ca0 --stat` |
| 7 | P2 重复 | 逐字符相同的重复:`FormatCount`(MainWindow.cpp:59)== `SessionService::FormatCountSimple`(SessionService.cpp:388);`NowFiletime` ×2(JunkScanner.cpp:34、DevBuildCache.cpp:78);分割器 5 变体(SplitNames/TsvSplit/SplitQuery/SplitCommandLine/Presenters.cpp:~487 内联);`FindFirstFileExW` 枚举 8 处 | grep 佐证 |
| 8 | P2 横切 | 版本"单一源"机制成立(version.h 生成 + rc/标题消费)但**数据漂移**:incr_build.bat 的 MINOR 冻结在 10,UI 显示 "v2.10 build 43" 而仓库已是 v2.14——4 个 minor 版本没人改脚本 | tools/incr_build.bat + build_number.txt(43) |
| 9 | P2 分层 | 唯一的反向依赖:`core/SessionService.cpp:12` include `res/resource.h`(core 依赖 UI 资源头拿 WM_APP_* 契约);线程契约("worker-only")全部是注释,无任何 debug 断言/机器检查 | 03_includes.py;grep 无 GetCurrentThreadId 断言 |
| 10 | P2 卫生 | 构建产物 7 个输出目录共约 664 MB 未纳管:`build\{Debug,Debug-verify,Release,Release-verify,Release-tour,intermediate}` + 遗留 `MiniSys\build`(106 MB);vcxproj 只定义 Debug/Release 两个配置,-verify/-tour 是 msbuild `/p:Configuration=` 的 ad-hoc 产物;.gitignore 漏 `build/`、`.tmp-review/`、`.tmp-shots/`(git status 长期有 3 条 untracked 噪声);根目录**空文件 `build_log_full.txt` 被入库** | du;git ls-files |

---

## 1. 结构度量表

### 1.1 编译单元行数 Top(01_loc.py;src 合计 13,205 行 = 11,071 cpp + 2,097 h + 37 rc;tests 1,849 行 / 15 文件)

| 文件 | 行数 | 备注 |
|---|---|---|
| MainWindow.cpp | 1975 | M0 后 399 → M1 后 474 → 现 1975(v2.3–v2.14 增量全部落回此文件) |
| core/VolumeIndex.cpp | 1373 | 8 职责簇(见 §2.3) |
| core/SessionService.cpp | 1065 | 10 职责簇(见 §2.2) |
| ui/Presenters.cpp | 891 | 6 个 presenter 共享 ListTabPresenter,结构尚好 |
| core/GuardRails.cpp | 468 | 单一职责,健康 |
| core/JunkRules.cpp / JunkScanner.cpp / OperationLog.cpp | 406 / 405 / 390 | 健康 |
| util/Json.cpp / core/DevBuildCache.cpp | 364 / 352 | 自研 JSON,健康 |

### 1.2 函数长度 / 圈复杂度 Top(02_funcs.py 启发式:括号配对 + 关键词计数;**假阳性声明**:switch-case 风格(WndProc/ParseString)与 TaskDialog 按钮块会抬高 CC,数值看相对排序而非绝对值;共识别 395 个函数,≥100 行 17 个,CC≥20 30 个)

| 函数 | 行 | CC | 备注 |
|---|---|---|---|
| MainWindow::WndProc | 361 | 127 | 8 类消息 + 树下钻业务逻辑内联(L297–333) |
| VolumeIndex::BuildFull | 194 | 41 | USN 双布局解析 |
| JunkScanner::Scan | 185 | 50 | 三种 RuleMode × 索引/文件系统双路径 |
| MainWindow::OnExecute | 184 | 39 | 分险级确认/委派确认/隔离区清空分支 |
| DevBuildCache::Scan | 170 | 47 | |
| LargeFileScanner::Scan | 150 | 36 | |
| VolumeIndex::TryLoadCache | 126 | **53** | 全仓最高 CC(v2.14 加 MAC 后继续膨胀) |
| VolumeIndex::Search | 107 | 45 | 查询语法过滤与匹配混在一起 |

### 1.3 include 图(03_includes.py;基于 project-internal 头文件名解析,96 文件)

- **环:0(SCC 全单点)**;无 ui←core、util←core 之类逆向边。这是全仓最值得肯定的数字。
- 跨层边数:app→core 9、app→ui 7、app→platform 3、app→util 4;ui→core 12、ui→res 2、ui→util 3;**core→util 53、core→core 53**(core 内部全连接,拆库时 core 是一整块);platform→util 6;**core→res 1(违规,SessionService.cpp:12)**。
- fan-in Top:StringUtils.h 25、PathUtils.h 22、Logger.h 19、Scanner.h 10、OperationLog.h 8——util 层是事实上的地基,拆 `minisys_util.lib` 无环障碍。

### 1.4 重复代码清单(本仓已知项逐一核验)

| 重复 | 位置 | 核验结果 |
|---|---|---|
| `FormatCount` vs `FormatCountSimple` | MainWindow.cpp:59 / SessionService.cpp:388 | **逐字符相同**(连注释措辞都同源),确认 |
| `NowFiletime` | JunkScanner.cpp:34 / DevBuildCache.cpp:78 | 逐字符相同,确认 |
| 分割器 ×5 | JunkScanner.cpp:64 SplitNames(;)、OperationLog.cpp:61 TsvSplit(\t)、VolumeIndex.cpp:918 SplitQuery(空白)、DelegateOp.cpp:34 SplitCommandLine(引号感知)、Presenters.cpp:~487 内联 ext 过滤分割(;,) | 确认 5 变体 |
| 目录枚举 ×8 | FindFirstFileExW:DevBuildCache:123、FolderTreeScanner:39、JunkScanner:100/129、VolumeIndex:572、FastWalk:121、PathUtils:150/234 | 确认(其中 PathUtils/FastWalk 是公共层,JunkScanner/DevBuildCache/FolderTreeScanner 各自手写) |
| FormatAge vs FormatTimeShort | MainWindow.cpp:71 / Presenters.cpp:271 | 功能相邻(年龄 vs 短时间),非逐字重复,合并收益中等 |
| TaskDialog 构造 | Dialogs.h ConfirmTask(共享)、MainWindow.cpp ×2、Presenters.cpp ×1 | 已有公共helper,MainWindow 的 About/执行确认未走它,轻度 |

### 1.5 God object 职责簇逐个列

**MainWindow(1975 行,27 个私有成员函数):** ①窗口/控件生命周期(OnCreate/OnSize);②Tab 路由与**每 Tab 控件可见性矩阵 + 每 Tab 说明文案**(OnTabChanged,108 行);③WndProc 巨型消息分发(含 NM_CUSTOMDRAW 风险配色、TreeView 下钻业务);④扫描/执行/撤销入口(OnScan/OnExecute/OnPlanDone);⑤空闲维护定时器状态机(OnIdleCheck/idleCountdown_/idleArmInput_);⑥搜索防抖状态机(TIMER_SEARCH_DEBOUNCE/RETRY);⑦设置 Load/Save 接线;⑧**3 个手写对话框**(RootsDlg、QuickFilter 菜单、命令面板装配);⑨tour 模式(v2.13c);⑩状态栏/执行按钮聚合显示。

**SessionService(1065 行,单例):** ①worker 线程与任务代数(taskGen);②扫描;③索引构建(BuildIndexAsync);④搜索(SearchAsync);⑤空闲维护(IdleMaintenanceAsync);⑥计划执行(RunPlan);⑦隔离区清空;⑧结果缓存持久化(TryLoadCachedResults/SaveResultsCache,含消毒);⑨进度通道;⑩undo/verify/preview 三组 worker 只读操作。公共 API 30+。

**VolumeIndex(1373 行):** ①每卷注册表(For/Registry);②USN 全量构建(BuildFull);③USN 增量(RefreshFromUsn/ApplyChanges);④walk 回退(WalkBuild/RefreshWalk);⑤磁盘缓存 + v2.14 MAC(TryLoadCache/SaveCacheIfWorthwhile);⑥查找结构(Finalize/byFrn_/childrenOf_/dirPaths_/filePaths_/frnToDirPath_);⑦路径查询(HasDir/CollectSubtree/CollectChildren);⑧**搜索引擎 + 查询语言**(Search/ParseFilterTerms/ToggleQueryToken——纯字符串处理函数放在索引类里);⑨测试缝合 4 个。

---

## 2. 分层与可测试性

### 2.1 测试工程源码清单漂移(核码结论)

- MiniSys.vcxproj:37/37 src .cpp 全在清单(脚本核验,**当前无漂移**)。
- MiniSysTests.vcxproj 手抄 25 个 app .cpp。**不在测试构建的 12 个**:MainWindow、main、AppScanner、FolderTreeScanner、LargeFileScanner、CrashDump、ui/×5(CommandPalette/Controls/Icons/Layout/Presenters)、FastWalk。
- 历史事故证实:`git log -S "JunkScanner.cpp" -- MiniSysTests.vcxproj` 只有 12dd8da(v2.13,2026-10-07)一次——JunkScanner 自 v2.0 起就不在测试构建,直到 PerfBenchTests 需要它才补上。**手抄清单的本质:文件存在 ≠ 进测试,漂移不报错,只有链接失败才暴露。**
- 测试计数(gtest 宏计数):**97 = 78 TEST + 19 TEST_F,407 处 EXPECT/ASSERT**;15 个测试文件。有专属测试文件的被测单元 11 个;DeleteOp、MoveJunctionOp、Settings、Hash、Junction、Privilege、RestartManager、SecretStore、SystemRestore、DirSizeCache、Logger、PathUtils、StringUtils、Win32Error 这 14 个进编译但无专属测试(部分被传递覆盖)。
- UI 层:**0 个单元进测试、仅编译覆盖**(且只经主工程编译);MainWindow+main+ui/* = 3466 行 = app .cpp 的 31.3%。

### 2.2 演进方案:按层拆静态库,用链接强制依赖方向

```
minisys_util.lib     (util/ 7 cpp)         ← 无内部依赖,先拆,零风险
minisys_platform.lib (platform/ 7 cpp)     → 链 util
minisys_core.lib     (core/ 16 cpp)        → 链 platform+util
minisys_ui.lib       (ui/ 5 cpp)           → 链 core(+res)
MiniSys.exe          (main + MainWindow)   → 链 ui+core+platform+util
MiniSysTests.exe     → 链 core+platform+util 三库 + gtest
```

增量步骤(S 级每步):①先建 util 库工程,把 7 个 ClCompile 移入,app/tests 改为链库——tests 清单从 25 项变 3 个库引用,**手抄清单问题整类消灭**;②platform、core 依样;③core 入库前先把 WM_APP_* 契约从 res/resource.h 抽到 `core/UiMessages.h`(消除 core→res 反向依赖;resource.h `#include` 它保持 UI 兼容);④MainWindow/ui 最后,可无限期推迟。风险:vcxproj 改动潮(一次性)、链接顺序初期的 LNK 未定义符号(恰是把隐藏依赖显形的过程,是收益不是代价)。与现状对比:现在"分层正确"靠人肉纪律,拆库后 core include ui/res 直接编译失败——**把 §1.3 那个唯一的违规也变成不可能**。

### 2.3 核心服务无接口怎么 mock

- `VolumeIndex::For(drive)`:**不可测**。Registry() 是 `static std::map<wchar_t, unique_ptr<VolumeIndex>>` 按真实盘符懒建,无注入点;`ResetForTesting()` 只重置成员不清注册表;多卷合并(ValidVolumes/合并搜索预算,v2.11 引入)因此无单测,仅有活体确认(evolve-log:2,831,802 项)。演进:抽 `IVolumeSource` 或允许 `For()` 注册假卷(测试专用盘符映射),与 2.2 的 core.lib 一并做。
- `SessionService::Instance()`:单例 + `SetWindow(nullptr)` + `SetCacheDirForTesting`,可测但**全局共享状态跨用例**(tests 顺序敏感风险);无构造注入。观感:够用但每加一类任务都要动单例头文件。
- AppScanner:注册表枚举无接口包装,是它进不了测试构建的直接原因(观点:包一层 `IUninstallSource` 即可,它只有 158 行)。

---

## 3. 扩展性走查(逐项:改动文件 + 行数,以历史提交实测为准)

### (a) 新增清理规则字段(如 minSizeMB)— 🟡

以 `minAgeDays`(同型先例)为尺:①JunkRules.h 结构体 1 行;②JunkRules.cpp:145 解析 1 行;③JunkScanner.cpp 过滤 2 处(IsOldEnough 逻辑 + detail 文案 370-371);④rules.json 字段;⑤JunkRulesTests 验证用例。**= 4 个源文件 / 约 30 行,半小时级**。好的一面:规则外置 + LoadValidated 校验 + 内置回退框架都在,新字段自动获得安全校验位。坏的一面:字段语义(过滤发生在哪个阶段:展开前/尺寸计算后)散在 JunkScanner::Scan 的 185 行里,新人要读懂双路径(索引/文件系统)才敢加。

### (b) 新增右键菜单项 — 🟢

①resource.h 加 `IDM_LIST_*`;②OnListContextMenu 一行 `AppendMenuW`;③WndProc 的 WM_COMMAND switch 一个 case(MainWindow.cpp:418-419 为例,IDM_LIST_VERIFY v2.7 实测仅 3 处)。**= 2 文件 / 5-10 行**。唯一摩擦:第三处落在 CC 127 的 WndProc 里,每加一项都在恶化 Top1 函数。

### (c) 新增 Tab 页 — 🔴(对照 Search tab,v2.3 实测:16 文件,+529/-22)

TabId.h(enum+Count)、MainWindow.cpp(+110:创建/切页/消息)、SessionService(+30)、VolumeIndex(+192,属功能本体)、resource.h(+4)、Controls.cpp(+12)、Layout.cpp/h(+28)、Presenters.cpp/h(+100)、UiHandles.h(+3)、ScanItemTests/VolumeIndexTests(+62)。结构性原因:**Tab 是散射轴**——控件创建(Controls)、可见性矩阵(OnTabChanged 1093-1119 逐控件 ShowWindow)、说明文案(OnTabChanged 1129-1163 六路 switch)、布局(Layout)、presenter 注册(OnCreate)、消息分发(WndProc)、扫描按钮语义(1125)。数组按 `TabId::Count` 自动扩容(SessionService::results_ 等)是唯一的自动化部分。观点:不引入"Tab 描述符"结构(控件集/文案/按钮集数据化),第 7 个 Tab 的成本仍是 13+ 文件。

### (d) 新增索引来源 — 核心侧 🟢 / 提交粒度 🟡(对照 walk fallback,v2.6:10 文件,+296/-15)

索引源本身只动 VolumeIndex.cpp/h(+131:WalkBuild/RefreshWalk/walkBuilt_,策略藏在 EnsureBuilt 后面,调用方无感)——这是 EnsureBuilt 门面设计的直接收益,给 🟢。但同一提交还打包了 quick-filter 预置(UI 4 文件 + resource.h 18 行),即**一次提交 = 一个核心特性 + 一个 UI 特性**,回溯改动集时两者不可分。教训入库:功能提交按层拆。

### (e) 新增设置项 — 🔴(对照 devCacheRoots,v2.11+2.12:两次提交;当前落点 5 文件)

Settings.h(字段)+ Settings.cpp(Load/Save 各 1 行,手写序列化)+ DevBuildCache.cpp(消费 ×2)+ **MainWindow.cpp ×5 处**(声明、菜单入口、OnDevCacheRoots 102 行手写模态对话框、保存接线)+ CommandPalette 一行。v2.11 引入(43e5302)与 v2.12 补 UI(4c55ca0,MainWindow +178)合计约 250 行。结构性原因:**没有设置页框架**——每个设置项自带一份 UI(LargeFiles 的三个设置控件又是另一套硬编码),Settings 序列化也是逐字段手写(新增字段要同步 Load/Save 两处,漏一处 = 静默丢设置)。devCacheRoots 的 102 行对话框本应是 ui/Dialogs 的可复用"多行文本+浏览"件,且其解析逻辑(正是 #1 缺陷所在)埋在 UI 里不可单测。

---

## 4. 横切

| 关注点 | 现状 | 评价 |
|---|---|---|
| 任务代数/DONE 消息映射 | **单一事实源成立**:`DoneMessageFor(TaskKind)` 一个 switch(SessionService.cpp:36-44);`Post()` 对 5 种 DONE + TASK_STARTED 统一盖 taskGen 戳(67-78),UI 按 gen 丢弃过期消息(R-4 修复核码属实) | 👍 生产侧闭环;消费侧 MainWindow 仍需 6 个独立 handler 手工配对,映射正确性无机器检查(观点:可加 `static_assert` 表或单测遍历 TaskKind→msg 的双射) |
| UI 线程契约 | VolumeIndex.h:60-62、SessionService 注释声明 "worker-only"/"UI 只碰两个原子"——**全部是注释**;全仓无 GetCurrentThreadId/owner 线程断言(grep 证实,仅 CrashDump 用它写 dump) | 建议:DEBUG 版 `AssertOwnerThread()` 一行宏进 VolumeIndex/SessionService 容器入口,把三类评审反复出现的竞态(R-4、L-1、V3)变成断言可捕获 |
| 魔数集中度 | 控件/定时器/自定义消息/IDM 全在 res/resource.h(89 行,含出处注释)👍;**例外**:RootsDlg 用裸控件 ID(编辑框=1 即 IDOK、浏览按钮=100,MainWindow.cpp:965-980 / RootsDlgProc `GetDlgItem(h,1)`、`case 100`)——evolve-log V1 已记载 IDOK=1 踩过坑,同类魔数又新增了一份 | RootsDlg 的 1/100 应入 resource.h 或 ui/Dialogs 统一 |
| 版本号单一源 | 机制:incr_build.bat(PreBuildEvent)→ build_number.txt(git 跟踪)→ version.h(gitignore 生成)→ rc + 标题双消费,**机制是好的**;**数据漂移**:脚本硬编码 MINOR=10,HEAD 是 v2.14,UI 显示 "v2.10 build 43"(build_number=43) | 把 MINOR 也进 build_number.txt(或从 git tag 派生),否则机制形同虚设 |
| 委派白名单 | `DelegateCommandAllowedImpl` 单点(JunkRules.cpp:168),加载消毒(SessionService.cpp:276)+ 执行复验(696)两层核码属实(review-03 T-B1 修复存活) | 👍 |
| shlwapi.lib | 主工程链接但**全仓无任何 shlwapi 函数引用**(v1 遗留);srclient.lib 与 SystemRestore.cpp 的 `#pragma comment(lib)` 双重声明 | 清理 |

---

## 5. 构建与仓库卫生

- 跟踪文件 165 个、0 二进制/截图入库(评审截图均被 .gitignore 的 png 规则拦住)👍;.git 4.4 MB、third_party 1.1 MB(vendored gtest,合理)。
- **构建产物失控(量化)**:`build\` 558 MB(intermediate 286 + Debug-verify 58 + Release-verify 43 + Release 43 + Release-tour 39 + Debug 37)+ 遗留 `MiniSys\build` 106 MB(IntDir 早已改到 $(SolutionDir)build\intermediate,此目录是旧布局残骸)。vcxproj 只定义 Debug|x64 / Release|x64;`*-verify`/`*-tour` 是评审/巡页时 `/p:Configuration=` 的 ad-hoc 配置名——**没有任何工程定义它们,却各留了一份 40 MB 的 exe+pdb**。
- **规范化方案(S)**:①.gitignore 增加 `build/`、`MiniSys/build/`、`.tmp-review/`、`.tmp-shots/`,删除已入库的空文件 build_log_full.txt;②verify/tour 一律用 `/p:OutDir=build\verify\` 覆盖输出而非新 Configuration 名(同一套编译设置,产物可整目录删);③build.bat 增加 `clean` 目标清 intermediate;④迁移期一次性删除现有 6 个陈旧目录(约回收 660 MB)。
- 测试工程链接库(8)vs 主工程(12)漂移:差集 = comctl32/shlwapi/gdi32(UI 专用,合理)、srclient(靠 pragma 兜底才链上——SystemRestore.cpp 在测试构建里,**靠源码里的 pragma 而非工程配置**,属隐性约定)、dbghelp(CrashDump 不在测试构建,合理)。结论:tests 清单基本自洽,但 srclient 这类"pragma 补丁"掩盖了清单不完整,拆静态库后自然消失。

---

## 6. 文档漂移清单(逐条核对代码;≥10 处)

| # | 文档陈述 | 代码现实 | 判定 |
|---|---|---|---|
| 1 | ARCHITECTURE.md §1 "5 个功能页" | TabId.h 6 页(v2.3 Search) | 漂移 |
| 2 | ARCHITECTURE.md §3 "MainWindow 915 行" | 1975 行 | 漂移 |
| 3 | ARCHITECTURE.md §5 链接库清单 10 项 | 现 12 项(+srclient、dbghelp),且 shlwapi 已无引用 | 漂移 |
| 4 | ARCHITECTURE.md §10 history.tsv 9 列 TSV | history.jsonl + TSV 一次性迁移 + v2.14 记录签名 `sig` 字段(Operation.h:31) | 漂移 |
| 5 | ARCHITECTURE.md §8 "WM_APP_SCAN_* 消息不带参数" | DONE/TASK_STARTED 带 wParam=taskGen;消息从 2 种增到 8 种(resource.h:51-68) | 漂移 |
| 6 | ARCHITECTURE.md §2 持久化 = logs+history | 另有 cache\(results-*.json、index-cache.bin)、settings.json、dumps\、machine secret | 漂移 |
| 7 | DESIGN-v2 §13 M0 验收 "MainWindow <400 行" | M0 时点达标(实施报告 399/474 有如实记录),此后无任何文档承认 1975 的回弹 | 漂移(静默) |
| 8 | DESIGN-v2 §11 性能目标 "待基准" | v2.13 实测 41ms/76ms 暖扫描(commit 12dd8da),文档未回填 | 漂移 |
| 9 | DESIGN-v2 §12 ADR-006 "零第三方依赖" | third_party/googletest(仅测试链路;运行时仍零依赖) | 半漂移 |
| 10 | DESIGN-v2 §3 不变量 "所有 Scanner 消费同一份 VolumeIndex" | 仅 JunkScanner 消费(且走 Instance()=系统卷单卷);LargeFiles/FolderTree/DevBuildCache 仍 FastWalk(实施报告以 ADR-004 记录,算已声明偏差);Search 经 SessionService 消费 | 半漂移(已声明的偏差,但"For 多卷注册表 vs Instance 旧入口并存"无文档) |
| 11 | 需求.txt 通篇 "C 盘" | v2.11 身份改为全固定磁盘"磁盘瘦身助手"(MainWindow.cpp:44 标题、VolumeIndex.h:25 注释) | 漂移(需求侧) |
| 12 | ARCHITECTURE.md §13 R-009 死代码清单 | IDI_APPICON 已用(MainWindow.cpp:117);**IDC_BTN_REFRESH/IDC_BTN_DEDUP/OpType::MoveFilePath 仍是死定义**(grep 无使用) | 部分存活 |
| 13 | 实施报告 §2 M1 "27 条规则 15/7/3/2" | rules.json 27 条 = 15 safe + 7 cautious + 3 advanced + 2 info | **一致**(无漂移,抽检通过) |
| 14 | REVIEW-3 §0 T-B1 "双层修复" | SessionService.cpp:276(加载消毒)+696(执行复验)核码属实 | **一致**(修复存活) |
| 15 | evolve-log Round1 V3 "取消路径显式失效 valid_=false" | VolumeIndex.cpp:492 一带存在该逻辑 | 一致 |

结论:实施报告与 review-3 的"已修复"声明抽检全部存活;**身份类/结构类文档(ARCHITECTURE、需求)整体滞后一个时代**——ARCHITECTURE.md 自我声明"保留为 v1 基线"是有意的,但代价是**当前版本没有任何一份与代码同构的架构文档**,新人只能读 DESIGN-v2 的"目标态"再自己减去偏差。

---

## 7. 被推翻假设

1. **"测试清单当前有漂移"——被推翻**:核验 MiniSys.vcxproj 37/37、MiniSysTests 25 项均与磁盘一致;漂移是历史事实(JunkScanner,v2.13 才补)而非现状。
2. **"Instance() 与 For() 双入口会造成 C: 卷双索引双内存"——被推翻**:读码证实 `Instance()` 委托 `For(systemDrive)`(VolumeIndex.cpp),单一注册表,无重复构建。
3. **"MainWindow 拆分后保持瘦身"(DESIGN-v2 M0 验收的隐含持续性)——被推翻**:1975 行,M0 目标已在 12 次提交中静默失效。
4. **"/W4 能拦住无操作 lambda 语句"——被推翻**:MainWindow.cpp:877/:1011 两处 `flush;` 自 v2.12 起通过了 Release+Debug 构建(commit 自述 "build clean");MSVC 对 lambda 对象表达式语句不产生 C4551,该缺陷只能靠 /WX+静态分析或单测拦截。
5. **"REVIEW-3 的 Top10 之 #1(活体验证索引)未闭环"——被推翻**:evolve-log Round 2 记录 2,831,802 项活体确认,已闭环。

---

## 8. 值得保留的实践(清单)

1. rules.json 外置 + LoadValidated 逐条安全校验 + 内置回退 + 委派白名单单点(JunkRules.cpp:168)——扩展规则的安全底座完整。
2. DoneMessageFor 单 switch + taskGen 戳:任务代数的单一事实源,生产/消费两端语义清晰。
3. include 图零环、util 地基稳定(fan-in 25/22/19)——拆库的前提已天然成立。
4. res/resource.h 集中全部控件/定时器/消息/菜单 ID 并带版本出处注释(仅 RootsDlg 魔数一处例外)。
5. 原子写纪律:settings.json 与 OperationLog 均 temp+rename(MoveFileExW REPLACE_EXISTING)。
6. *ForTesting 缝合 + 测试 main 统一重定向日志与缓存目录(tests/main.cpp,V-R5 修复)——"测试不碰用户数据"已成显式工程约束。
7. Presenter 快照契约(UI 永不持 worker 容器引用,P1-1)+ ApplyDeadPaths 的 render 参数(04-R1 教训参数化)。
8. evolve-log 的"发现-修复-证伪-活体确认"闭环记录格式;每轮提交信息带测试计数。
9. EnsureBuilt 门面:索引来源(USN/walk/cache)切换对调用方透明——(d) 走查 🟢 的直接原因。
10. incr_build.bat 的"生成头文件作单一版本源"机制(机制本身,数据漂移另计)。

---

## 9. 演进路线图(ROI 排序;S=≤半天 M=1-3 天 L=≥1 周)

| # | 项 | 规模 | 增量步骤 | 风险 |
|---|---|---|---|---|
| 1 | **修 flush; 缺陷 ×2** + devCacheRoots 解析抽成可测函数 + 回归单测(多根、含空行、含无效根) | S | 2 行修复 + 1 个纯函数迁移进 core/Settings 或 util;单测 3 用例 | 极低;注意修后保存路径语义(验证现存目录) |
| 2 | **仓库/构建卫生**:gitignore 补全、删 build_log_full.txt、verify/tour 改用 /p:OutDir、清 660 MB 陈旧产物 | S | 纯仓库操作,无代码 | 无 |
| 3 | **build.bat 开 /WX + CI 化自检**(每次提交跑 build+test 并比对测试计数) | S | /WX 一次清零存量告警;计数比对脚本化 | 存量告警清理工作量未知(先 /W4 清单) |
| 4 | **按层拆静态库**(§2.2 方案) | M | util→platform→core 三步,tests 清单换库引用,顺手消灭手抄漂移与 srclient pragma | 链接错误潮(显形隐藏依赖,属收益);建议单独提交逐步走 |
| 5 | core/UiMessages.h 抽 WM_APP_* 契约(消 core→res)+ DoneMessageFor 映射单测 | S-M | 契约头迁移 + resource.h 转发;单测断言 TaskKind→msg 双射 | 低 |
| 6 | **MainWindow 二次瘦身**:WndProc 拆分(命令表/NOTIFY 路由表)、OnTabChanged 可见性矩阵与文案数据化(Tab 描述符)、RootsDlg 等对话框移 ui/Dialogs.cpp;目标 <900 行 | M | 每拆一块跑 97 测试 + 手工回归;不追求一次到位 | 中:消息路由表漏分支=静默失效,需 #5 的映射检查先行 |
| 7 | 重复代码合并:util/Format.h(FormatCount/NowFiletime/FormatAge)、分割器统一 util/Split.h、JunkScanner/DevBuildCache/FolderTreeScanner 的 FindFirstFileExW 收敛 util/DirIter | M | 纯搬家 + 单测;6 处文件逐个替换 | 低 |
| 8 | SessionService 拆 ResultsCache / IdleMaintenance / 进度通道;VolumeIndex 拆 IndexPersistence 与 SearchQuery(查询语法解析独立) | M-L | 先抽 ResultsCache(边界最清晰,已有 SetCacheDirForTesting);VolumeIndex 拆缓存层(TryLoadCache CC53 最痛) | 中:单例改组合期间注意 97 测试全绿 |
| 9 | 可测性注入:VolumeIndex For 注册表假卷接口、AppScanner IUninstallSource、ui/Presenters 进测试构建(勾选态/视图态逻辑是纯逻辑) | L | 随 4/8 顺路;目标:测试构建覆盖 61.4%→85% | 中 |
| 10 | **文档再生**:按当前代码重写 ARCHITECTURE.md(六页/多卷/缓存/签名),DESIGN-v2 加"现状校准"附录,需求.txt 增补 v2.11 身份变更;版本 MINOR 进自增机制 | M | 以本报告 §6 清单为核对底稿 | 无 |

---

## 10. 局限

1. 全部结论基于静态分析:未构建、未运行 97 测试、未启动 GUI;#1 缺陷的行为推演(多根合并→全部丢弃)来自逐行读码 + 分支推演,未活体复现(按约束不启动提权 exe),建议用户在 devCacheRoots 对话框输入两行路径验证"忽略了 N 个无效路径"提示。
2. 函数长度/CC 为脚本启发式:函数签名识别对多行声明、类内定义、Win32 回调有 ±10% 误差;CC 对 switch/TaskDialog 按钮块的抬升已在 §1.2 声明假阳性。
3. 测试计数以 gtest 宏计数(97)为准,与 commit 自述(94/94 等)存在口径差(参数化/禁用用例未计)。
4. include 图按"头文件名→模块"解析,同名头或宏拼接 include(本仓未见)会漏边;`#pragma comment` 类链接依赖不进图。
5. 工作树 MiniSys.vcxproj 有未提交修改(gitStatus M),本报告以工作树为准;若该修改是实验性内容,§5 的链接库结论需复核。
6. UI 观感/交互不在本角色范围(见 07 号 UX 分报告);安全细节(签名/MAC 语义)仅核调用点存在性,密码学评审归 05 号。
