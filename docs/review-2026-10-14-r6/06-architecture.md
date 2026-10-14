# MiniSys 第六轮评审 · 06 软件工程与高可扩展性(架构角色第三评)

> 日期:2026-10-14 · 对象:HEAD `2a9d653`(v2.19a)· 只读静态分析
> 方法:脚本量化(`.tmp-review/r6/exp/`:02_funcs.py 函数度量、03_includes.py include 图+Tarjan、04_tests.py 测试清单比对)+ git/wc/grep 逐条核验。未构建、未运行测试、未启动 exe、未读用户数据;对 build/ 仅做文件名/体量清点(git/du 级)。禁跑脚本清单未触碰。
> 前序:round-4 06 首评(docs/review-2026-10-11/06-architecture.md)、round-5 美学双路。本轮避免重复,聚焦增量与遗留执行情况。

---

## 0. 摘要

**架构健康度:5.5 / 10(round-4 首评 6/10,小幅下滑)。** 底子仍是好的:include 图零环(95 节点 Tarjan 复验)、任务代数单一源、规则外置+校验、原子写纪律全部存活;v2.15 虚拟列表与 v2.19 全行取大小让主列表路径的可扩展性实质变好。但**round-4 路线图 Top10 一项都没执行**(逐条核验,见 §1.5),同期 MainWindow 再涨 254 行、WndProc 膨胀到 486 行/启发式 CC 151、测试编译覆盖率从 68.7% 降到 59.1%,手写线程池从 ~5 处涨到 7 处。另发现 **1 个 round-4 修复不完整造成的新回归**(浏览按钮静默失效,§2 Top1)与 **1 个未评审的权限模型翻转**(v2.17 asInvoker,§2 Top3)。

### 分维度评分

| 维度 | r4 | 本轮 | 依据 |
|---|---|---|---|
| 分层与依赖 | — | **7/10** | 0 环、util 地基稳(fan-in 25/22/19);core→res 违规仍在(SessionService.cpp);双 God object 继续长 |
| 可测试性 | 6 | **4/10** | 40.9% .cpp 行无测试编译覆盖(12 文件/4745 行,UI 层 0 单测);手抄清单 25/37 未变 |
| 扩展性 | — | **5/10** | 右键菜单🟢/规则数据化🟢;列数魔数、Tab 散射、设置项 6 落点、信息文案 26 处 ShowHint |
| 横切一致性 | — | **6/10** | taskGen/DoneMessageFor 单点👍;线程契约仍纯注释;魔数已实际咬人一次(RootsDlg id) |
| 构建与质量基建 | 6 | **4/10** | 无 /WX、无 CI、build/ 896MB(10 个输出目录)、build_number.txt 跟踪致每次构建脏树、Latest 部署不带 PDB |
| 文档 | — | **4/10** | ARCHITECTURE 停在 v1 时代;asInvoker 翻转全文档失载;evolve-log 停更 7 天/20+ 提交 |

### Top 10 结构性问题

| # | 级别 | 问题 | 证据 |
|---|---|---|---|
| 1 | **P2 缺陷(新)** | RootsDlgProc 浏览按钮**静默失效**:round-4 R-1 修复把编辑框 id 1→1001 时只改了 IDOK 分支,**浏览分支漏改**——`GetDlgItem(h, 1)` 现取回"确定"按钮(IDOK==1),EM_REPLACESEL 发给按钮无效果;v2.14 修复至今"浏览添加…"选完目录什么也不发生 | MainWindow.cpp:1134(漏改)vs :1099(已改)vs :1201(创建 id=1001);git blame:1134 行源自 4c55ca0(v2.12) |
| 2 | P1 结构 | MainWindow 回弹加速:1975→**2229 行**(+254/3 天);WndProc 361→**486 行、CC 127→151**(启发式);新增第⑪职责簇:**-probe 诊断状态机**(probeStage_ 三段轮询+Debug* 调用,~70 行)与**owner-draw 信息条排版**(DrawInfoText);dynamic_cast 16→**21 处** | 02_funcs.py;MainWindow.cpp:616-690、296-333 |
| 3 | P1 架构 | **v2.17 asInvoker 翻转未与 v2.14 安全链对账**:SecretStore 的机器密钥 SDDL 为"Administrators+SYSTEM 专有"(SecretStore.cpp:14,51-58),非提权运行时读不到 → 签名/MAC 信任链整体休眠;全仓**没有任何应用内提权路径**(grep runas/ShellExecuteW 仅 explorer 打开),rotate.ps1 注释"需要管理员的功能才在应用内自行请求提权"**无代码支撑**;main.cpp:20"both are elevated"、DESIGN-v2 ADR-010"requireAdministrator 维持不变"、ARCHITECTURE ADR-010 全部与 manifest 矛盾 | app.manifest:8;SecretStore.cpp:14-58;main.cpp:20;rotate.ps1:6-8;DESIGN-v2.md:311 |
| 4 | P1 流程 | **round-4 路线图 Top10 执行率 0/10**(watchdog/内存紧凑/FolderTree 并行/启动异步 JSON/关闭 join 尾/隔离区浏览器/扩展名报表/排除 UI/拆库/孤儿清理——逐条 grep 核验全部未做);evolve-log.md 止于 2026-10-07 Round 2,之后 20+ 提交无演进记录;无 CI 兜底 → 结构债只记账不清账 | §1.5 表;git log docs/evolve-log.md |
| 5 | P1 重复 | **手写线程池 7 处、3 种习语、池上限两种策略(6/8)混用**:游标式 fetch_add ×4(JunkScanner:277-310、LargeFileScanner:87-111、DevBuildCache:300-337、**v2.19 新增 SessionService:540-568**);mutex+cv+deque 队列式 ×3(PathUtils SizePool:174-270 约 97 行、VolumeIndex WalkBuild:535-653 约 119 行、FastWalk);`hw==0→4; clamp` 样板在 **7 个文件**逐字出现 | 见 §3.4;grep std::vector<std::thread> |
| 6 | P1 可测性 | 测试编译覆盖率降至 **59.1%**(无测试编译 .cpp 12 个/4745 行,占 40.9%;r4 为 31.3%);MiniSysTests 仍手抄 25/37;**Presenters.cpp 986 行(勾选态/快照/虚拟单元格=纯逻辑)继续 0 单测** | 04_tests.py |
| 7 | P2 扩展 | 列模型魔数:列数 **6**(scan tabs)/ **5**(history)以字面量散布在 4 个函数(Presenters.cpp:158-168 `i*6+k`、:195-196 `col>5`/`row*6`、:884-893 `i*5+k`、:905 `col>4`);WM_APP 8 个值分居 resource.h 两个不连续块,Post() 再硬编码 6 个消息名(SessionService.cpp:75-78) | 读码;resource.h:51-68 |
| 8 | P2 基建 | build/ **896MB**(r4 664MB):ad-hoc 配置目录从 4 个涨到 **10 个**(p5/p6/p7/p8/p9/probe/Debug-verify/Release-verify/Release-tour/tour2);sym.py **硬编码 build\p7**;build.bat 的 Latest 部署只拷 exe+rules.json **不拷 PDB** → 重建后旧 dump 无法符号化(Release 有 PDB 无归档);build_number.txt(=95)被 git 跟踪,**每次构建弄脏工作树** | du;tools/sym.py:8,24;build.bat 尾部;git ls-files |
| 9 | P2 卫生 | .gitignore 仍漏 `build/`、`.tmp-review/`、`.tmp-shots/`(只挡了 `.tmp-review2/`);IDC_BTN_REFRESH/IDC_BTN_DEDUP 死定义仍在 resource.h:16,20(r4 #10 孤儿清理未做);OpType::MoveFilePath 仍是只有序列化没有生产者的死枚举值 | .gitignore;grep 核验 |
| 10 | P2 横切 | 日志覆盖极不均:**MS_LOG 仅 53 处、集中在 10 个文件**,19/29 个 .cpp 零日志(全部 Scanner、全部 ui/、Junction/DelegateOp/DeleteOp/QuarantineOp)——错误范式=bool+errOut(进 UI 不进日志),事后排障只剩 UI 文案;UI/worker 线程契约仍纯注释(全仓无 GetCurrentThreadId 断言;static_assert 全仓仅 1 处) | grep MS_LOG;-c 按文件统计 |

---

## 1. 结构度量

### 1.1 编译单元行数(src 合计 13,781 = 11,602 cpp + 2,142 h + 37 rc;tests 1,903)

| 文件 | 现在 | r4 | Δ | 备注 |
|---|---|---|---|---|
| MainWindow.cpp | **2229** | 1975 | +254 | v2.16-2.19a 增量继续全部落回此文件 |
| core/VolumeIndex.cpp | 1388 | 1373 | +15 | +Debug* 诊断簇(db3c5da) |
| core/SessionService.cpp | 1094 | 1065 | +29 | +v2.19 取大小池 |
| ui/Presenters.cpp | 986 | 891 | +95 | +多级 FolderTree 渲染 |
| core/GuardRails.cpp | 468 | 468 | 0 | 健康 |
| core/JunkRules.cpp / JunkScanner.cpp / OperationLog.cpp | 411 / 405 / 390 | 406/405/390 | ≈0 | 健康 |

### 1.2 函数长度 / 圈复杂度 Top(02_funcs.py;启发式:括号配对+关键词计数,switch/TaskDialog 抬高 CC,#pragma/续行已清洗;≥15 行函数 181 个,**与 r4 的"395 个"分母不同——r4 计入短函数**)

| 函数 | 行 | CC | r4(行/CC) | 备注 |
|---|---|---|---|---|
| MainWindow::WndProc | **486** | **151** | 361/127 | 8 类消息+NM_CUSTOMDRAW×3(tab 加粗/风险配色)+GETDISPINFO 路由 |
| VolumeIndex::BuildFull | 205 | 36 | 194/41 | |
| JunkScanner::Scan | 185 | 49 | 185/50 | 三态×双路径,稳定 |
| MainWindow::OnExecute | 184 | 36 | 184/39 | |
| DevBuildCache::Scan | 174 | 46 | 170/47 | |
| WalkBuild | 154 | 29 | 154/29 | |
| SessionService::RunPlan | 148 | 26 | 148/26 | |
| FolderTreeScanner::Scan | 140 | 39 | — | v2.18 重写,+39 行 |
| **SearchAsync(lambda 体)** | 103 | 18 | — | v2.19 取大小池内嵌其中 |
| VolumeIndex::Search | 107 | 44 | 107/45 | |

≥100 行 20 个;≥50 行 56 个;CC≥20 24 个、CC≥30 11 个。结论:**长函数榜单 r4 原班人马+恶化一个 WndProc**;唯一新进入者是 v2.18/v2.19 的两处改动本身。

### 1.3 include 图(03_includes.py,95 节点)

- **环:0**(Tarjan SCC 全单点)——r4 结论存活,本轮复验成立。
- fan-in Top:StringUtils.h 25、PathUtils.h 22、Logger.h 19、Scanner.h 10、OperationLog.h 8——util 地位不变,拆库前提仍成立。
- 跨层边:core→util 53、core→core 53、ui→core 12、ui→ui 11、app→core 9、app→ui 7、core→platform 7、platform→util 6、**core→res 1(SessionService.cpp→resource.h,违规仍在)**。
- fan-out Top:MainWindow.cpp 19、Presenters.cpp 15、SessionService.cpp 15。

### 1.4 God object 职责簇(对照 r4,增量标注)

- **MainWindow(2229 行,35 个私有方法 + 17 个数据成员)**:r4 十簇全在,新增 ⑪ **-probe 自测状态机**(probeQuery_/probeStage_,OnCreate 解析+定时器轮询+诊断输出,MainWindow.cpp:616-690、828-843)⑫ **owner-draw 信息条排版与擦除**(DrawInfoText,v2.19/v2.19a)。
- **SessionService(1094 行,单例,~30 公有方法)**:r4 十簇全在;v2.19 把"搜索结果取大小"塞进 SearchAsync 的 lambda 体(540-568)——扫描/搜索/索引/执行/维护/缓存/进度/撤销/verify/preview 仍是单类。
- **VolumeIndex(1388 行)**:r4 八簇 + **⑨ 调试诊断 API**(DebugNameMatchCount/DebugNameSample/DebugSearchPass1/DebugFirstPath,db3c5da)——搜索引擎/查询语言仍在索引类里。

### 1.5 round-4 Top10 遗留执行核验(0/10)

| r4 # | 事项 | 核验结果 |
|---|---|---|
| 1 | UI stall watchdog | **未做**(grep watchdog/Heartbeat 0 命中) |
| 2 | 索引内存紧凑化 | **未做**(Node 已 40B 紧凑,但 5 个 unordered_map 容器未动;工作集 ~905MB@3M 项为静态推断) |
| 3 | FolderTree 并行定大小 | **未做**(仍逐目录串行 `DirectorySizeParallel(kid, 2)`,FolderTreeScanner.cpp:76,134) |
| 4 | 启动异步 JSON | **未做**(OnCreate 同步 `TryLoadCachedResults ×6`,MainWindow.cpp:816-818) |
| 5 | 关闭 join 尾 | **未做**(消息循环尾 `Shutdown()` 直接 join,MainWindow.cpp:270;takeover 路径靠 10s 轮询兜底) |
| 6 | 隔离区浏览器 | **未做** |
| 7 | 扩展名空间报表 | **未做** |
| 8 | 排除清单 UI | **未做**(仍只有"加入排除"单点,无管理界面,MainWindow.cpp:2157-2172) |
| 9 | 静态库拆分 | **未做**(仍 2 个 Application 工程,0 个 StaticLibrary) |
| 10 | 孤儿清理+卫生 | **未做**(死 ID/死枚举在;仅 MINOR 10→19 手工顺带修正) |

---

## 2. 分层与可测试性

- **测试怎么拼源码**:MiniSysTests.vcxproj **仍是手抄清单**(25 个 app .cpp 逐一 `<ClCompile Include="..\MiniSys\src\...">`);两份清单(主工程 37 / 测试 25)靠人肉同步,r4 论证过的"漂移不报错、链接失败才暴露"风险原样存在。本轮核验:25/25 与磁盘一致,**当前无漂移**。
- **框架核实**:third_party/googletest **vendored 真体 gtest**(src/gtest-all.cc),**不是自实现**;无 gmock。tests/main.cpp(29 行)自定义 main 重定向日志与缓存目录(V-R5 修复存活)。
- **测试计数真值**:**98 = 79 TEST + 19 TEST_F,EXPECT/ASSERT 412 处,15 个测试文件**——与 v2.18/v2.19 提交自述"98/98 Debug+Release"口径一致。
- **UI 编译单元进测试**:0 个(MainWindow/main/ui×5/FastWalk/LargeFileScanner/AppScanner/FolderTreeScanner/CrashDump 共 12 个 .cpp、4745 行、**40.9%** 无测试编译覆盖;r4 31.3%,因 MainWindow/Presenters 增长而恶化)。
- **核心服务无接口如何 mock**:与 r4 相同——VolumeIndex::For 静态注册表无注入点;SessionService 单例全局共享;AppScanner 注册表裸枚举。唯一注入缝是 *ForTesting 静态(4 个)。
- **按层拆静态库增量路径**:r4 §2.2 方案(util→platform→core 三步,顺手消灭手抄清单与 core→res 违规)完整仍适用,本轮不赘述;唯一新增论据:core→res 违规与手抄清单又原样存活了一个版本周期,说明**不动它的成本在复利**。

## 3. 扩展性走查(a–h)

### (a) 新增主列表列 — v2.18 为实测样本:🟢(改格式)/ 🟡(加新列)

v2.18"年份入时间戳"实测:**1 个文件、~10 行**(Presenters.cpp:316-331 格式函数 + :142 列宽 110→130)——展示层改动成本优秀。但**加一个全新列**的实测路径(v2.10 加"修改时间"列 + v2.13c 补可见 + v2.15 虚拟化改造,历史共 3 次才真正可用)暴露的是另一回事,当前需改:①kScanCols(Presenters.cpp:140)②cells_ 预渲染循环(159-168,**`i*6+k` 六个字面量**)③SupplyDispInfo 界限(195-196 **`col>5`/`row*6`**)④排序映射(sortCol_ 注释 106-108)⑤若历史页也要:另一套 `i*5+k`/`col>4`(884-893、905)。**≈8-10 处魔数,全在 2 个文件**。列数没有常量、没有 per-tab 列描述符。

### (b) 新增扫描器/清理规则源 — FolderTree depth 参数化(v2.18)为样本:🟢(参数)/ 🟡(新字段)/ 🟡(新扫描器)

- depth 参数化实测:**FolderTreeScanner.h +5 行(Config struct)+ Scan +59 行**,消费方(Presenters 建树)另 +80 行——参数化本身干净(Config 构造注入,无全局)。
- 但 v2.18 的 Scan 内部把"顶层目录→ScanItem 构造+mtime 提取"(78-95)与"嵌套层→同构代码"(128-147)**整块复制**(≈18 行 ×2),第三个深度层级会在第三次复制——参数化了深度,没参数化循环体。
- 新增规则(数据):rules.json 27 条 + LoadValidated 校验 + 内置回退,🟢。新增规则**字段**:仍是 JunkRules.h/JunkRules.cpp 解析/JunkScanner 过滤×2/rules.json/测试 ≈5 文件(r4 结论存活)。新增**扫描器类**:实现 Scanner 接口 + presenter BuildScanner + OnScan 分派,🟢-🟡(接口稳定,但扫描器进不了测试编译,新扫描器=又一份零覆盖代码)。

### (c) 新增右键菜单项 — 🟢

resource.h 加 IDM + OnListContextMenu 一行 AppendMenuW(MainWindow.cpp:2184-2199)+ WM_COMMAND 一个 case;**菜单选中后 `SendMessageW(hwnd_, WM_COMMAND, ...)` 重派发回命令路径**(2200-2202),handler 单点。= 2 文件 / 5-8 行。唯一代价仍是落进 CC 151 的 WndProc。

### (d) 新增设置项 — 🔴(与 r4 相同)

实测落点 6 处:Settings.h 字段 → Settings.cpp Load 一行 + Save 一行(手写序列化)→ MainWindow LoadSettings 一段 + SaveSettings 一段(2071-2110)→ 消费者 → 若带 UI:resource.h ID + Controls 创建 + Layout + OnTabChanged 可见性矩阵 + CommandPalette 一行。默认值双份(struct 初始化 + `AsInt(100)` 兜底)。无 schema/迁移/往返校验;漏一处=静默丢设置。devCacheRoots 型(带对话框)再加 100+ 行手写模态框。

### (e) 新增自定义消息 WM_APP_* — 🟡

现存 **8 个值(WM_APP+1..+8),编号连续无空洞**,下一枚 +9;但定义分居 resource.h 两个不连续块(51-54"v2.4/v2.8 增量"与 64-68"Custom messages"), newcomers 易重号。契约链=resource.h define + DoneMessageFor case(若新任务类,SessionService.cpp:38-46)+ TaskKind 枚举 + Post() 的**硬编码 6 消息名列表**(75-78)+ MainWindow WndProc case + handler ≈6 处。生产端单一源(👍),**消费端无机器校验**(r4 建议的 TaskKind→msg 双射单测仍不存在)。

### (f) 新增 Tab 页 — 🔴(与 r4 相同,未引入 Tab 描述符)

散射轴原样:TabId.h + TabName(MainWindow.cpp:46-56)+ OnCreate presenter 注册(787-792)+ **OnTabChanged 可见性矩阵(20+ 个 ShowWindow,1320-1366)+ 六路文案 switch(1368-1403)+ 扫描按钮语义(1367)**+ Controls + Layout + resource.h + Presenters 类。SessionService 的 per-tab 数组按 TabId::Count 自动扩容仍是唯一自动化部分。v2.18 没加 tab,没检验改进。

### (g) 信息条/文案集中管理 — 🔴

**ShowHint 调用 26 处,全部在 MainWindow.cpp**(grep 精确计数,含定义 27 行命中);**中文字面量行数:MainWindow.cpp 202、Presenters.cpp 74、SessionService.cpp 58(进度文案)、Controls.cpp 23**;tabInfoText_ 6 块多行文案 switch。无 copy 表/资源化;文案改版(如 v2.19a 排版)只能全文检索。风险文案(⚠/⊘ 前缀)约定散在字符串前缀里,无枚举。

### (h) 版本号机制 — 🟡(机制好,数据靠手)

机制存活且自 v2.19 起 MINOR 同步了(incr_build.bat 硬编码 `MINOR 19`,version.h/build 95/标题一致)。但 r4 建议的"MINOR 进自增数据"未做——**每次 minor 升版仍依赖手工改 bat**(v2.19 提交自述"incr_build.bat was still emitting v2.10 — bumped to v2.19",恰好证明 9 个版本没人改)。另:build_number.txt 被 git 跟踪 → **每次构建产生脏文件**,与"构建可重复"相悖。

## 4. 横切一致性

| 关注点 | 现状 | 评价 |
|---|---|---|
| 错误处理范式 | bool + `std::wstring& errOut` 全仓统一(QuarantineOp/Junction/…);**但 MS_LOG 仅 53 处/10 文件**,19 个 .cpp 零日志(含全部 ui/ 与除 SessionService/VolumeIndex 外的全部扫描器)——错误进 UI 不进日志,崩溃后只剩用户转述 | 半好:范式一致,可观测性弱 |
| UI/worker 线程边界 | VolumeIndex.h "worker-only" 注释、SessionService "UI 只碰两个原子"——**仍纯注释**;全仓 GetCurrentThreadId 仅 CrashDump 用;无 AssertOwnerThread 类断言 | r4 建议未落地 |
| std::thread 裸用 | 裸 spawn 语义上仅 2 处(DelegateOp.cpp:120 管道读线程、SessionService worker_);**池 7 处 3 习语**(§0 Top5):游标式 ×4、队列式 ×3、hw-clamp 样板 ×7 文件、上限 6/8 两种策略 | 无统一池抽象,正确性各自为政(WalkBuild 的 v2.17b parentFrn 截断正是队列式复杂度的代价) |
| 任务代数/DONE 映射 | DoneMessageFor 单 switch + Post() taskGen 戳 + UI 按 gen 丢弃——r4 👍 结论存活(Post() 内 6 消息名硬编码见 (e)) | 生产端好 |
| 日志约定 | Logger 单例文件+ODS 双写、逐条 flush;tests main 重定向;但覆盖见上 | 机制可用 |
| 魔数集中度 | 控件/定时器/菜单/消息 ID 集中在 resource.h(89 行,带出处注释)👍;**例外仍在且已咬人**:RootsDlg 内联 id 1001/100 两处硬编码相距 35 行,直接造成 Top1 回归;IDC_BTN_REFRESH/IDC_BTN_DEDUP 死定义未清 | 集中纪律+局部例外,例外的代价已兑现 |

## 5. 构建 / 质量基建 / 仓库卫生

- **警告**:/W4 主工程、/W3 测试工程(工程间漂移),`TreatWarningAsError=false`。**v2.17b 的 CRITICAL(parentFrn bool 截断)代码注释自证"Warnings-as-errors would have caught it"**(VolumeIndex.cpp:589-593)——/WX 的收益已被仓库自己的事故记录背书,仍未开。
- **PDB/符号**:两工程 Debug/Release 均 GenerateDebugInformation=true(PDB 有产);但 **Latest 部署只拷 exe+rules.json**,PDB 留在 build\Release\ 且会被下次构建覆盖 → **旧 dump 事后不可符号化**(v2.16d 崩溃就是靠当时还在的 PDB 符号化的;这类运气不可复制)。无符号服务器/归档目录。
- **CI**:无(.github 不存在);构建+双配置测试纪律靠提交信息自述。
- **仓库卫生**:跟踪文件 176;**build/ 896MB 未纳管**(10 输出目录:intermediate 285M + Debug-verify 62M + Debug 50M + Release-tour2 44M + Release-verify 43M + Release 42M + probe 41M + Release-tour 41M + p6 39M + p5 39M + p8 38M…);`[Rr]elease/` 等 gitignore 模式**匹配不到 `Release-verify` 这类名字**,故 exe/rules.json 成为 untracked 噪声;`.tmp-review/`、`.tmp-shots/` 仍漏(.gitignore 只写了 `.tmp-review2/`);`build/Latest/` 已补。无 README(仓库无入口文档);根目录无 build_log_full.txt(r4 报告后已删,核验通过)。
- **诊断脚本**:仅 tools/sym.py(1593B),**未见 sym2**(全仓 ls 核验);它硬编码 `build\p7\MiniSys.pdb` 路径与搜索目录——p5-p9/probe 这 6 个 ad-hoc 目录正是历次诊断构建的残骸,一次性脚本+一次性目录互相锁定。
- **黑名单脚本未运行**;rotate.ps1/build.bat/incr_build.bat/screenshot.ps1 仅读文本。

## 6. 测试体系与文档漂移

### 6.1 测试体系(真值表)

| 指标 | 值 | 对照 |
|---|---|---|
| 测试宏计数 | **98**(79 TEST + 19 TEST_F) | 提交自述 98/98 一致 |
| EXPECT/ASSERT | 412 | — |
| 高风险零覆盖 | MainWindow 2229 / Presenters 986 / FastWalk 199 / LargeFileScanner 277 / FolderTreeScanner 159 / AppScanner 159 | 与 r4 同名单,行数全部变大 |
| 有专属测试文件的单元 | 11 个(SessionService/VolumeIndex/GuardRails×3/JunkRules/PlanBuilder/QuarantineOp/DelegateOp/OperationLog/Json/DevBuildCache/ScanItem/PerfBench) | 无增减 |
| Debug+Release 双跑 | v2.18 起 commit 自述双配置 | r5 确立的验收标准存活 |

### 6.2 文档漂移清单(逐条对码核验,15 条)

| # | 文档陈述 | 代码现实 | 判定 |
|---|---|---|---|
| 1 | ARCHITECTURE ADR-010/§11 + DESIGN-v2 ADR-010:requireAdministrator | app.manifest asInvoker(v2.17) | **漂移(权限模型级)** |
| 2 | rotate.ps1 注释:"需要管理员的功能才在应用内自行请求提权" | 全仓无 runas/提权请求代码 | 漂移(无中生有) |
| 3 | main.cpp:20 注释 "both are elevated, so the message goes through" | asInvoker 下新旧实例可能一提权一非提权(UIPI 反向恰好不通) | 代码注释漂移 |
| 4 | ARCHITECTURE §1 "5 个功能页" | TabId 6 页(v2.3 起) | 漂移(r4 已记,未修) |
| 5 | ARCHITECTURE §3 "MainWindow 915 行" | 2229 | 漂移(未修) |
| 6 | ARCHITECTURE §10 history.tsv 9 列 | history.jsonl + v2.14 签名 sig 字段 | 漂移(未修) |
| 7 | ARCHITECTURE §8 "消息不带参数" | 8 种 WM_APP + taskGen wParam | 漂移(未修) |
| 8 | ARCHITECTURE §5 链接库 10 项 | 12 项;shlwapi **仍零引用**(本轮复验) | 漂移(未修) |
| 9 | DESIGN-v2 M0 验收 "MainWindow <400 行" | 2229,无任何文档承认回弹,无"现状校准"附录 | 漂移(静默,r4 建议#10 未做) |
| 10 | evolve-log.md | 止于 10-07 Round 2;其后 20+ 提交(含三轮评审执行)零记录 | 流程失守 |
| 11 | 需求.txt "C 盘瘦身" | v2.11 起全固定磁盘"磁盘瘦身助手" | 需求侧漂移 |
| 12 | DESIGN-v2 §13 性能目标 "待基准" | v2.13 实测 41ms/76ms 已一年未回填 | 漂移 |
| 13 | REVIEW-4 Top10 路线图 | 0/10 执行且无缓办记录 | 计划-执行脱钩 |
| 14 | 仓库入口文档 | 无 README/git ls-files 核验 | 缺失 |
| 15 | ARCHITECTURE §13 R-009 死代码 | IDC_BTN_REFRESH/IDC_BTN_DEDUP/OpType::MoveFilePath 仍是死定义(IDI_APPICON 已用) | 部分存活 |

抽检一致项(无漂移):rules.json 27 条=15 safe+7 cautious+3 advanced+2 info,与实施报告一致;DoneMessageFor 双射存活;tests 双跑声明与宏计数吻合。

## 7. 被推翻的假设 / 已验证无问题

**被推翻/修正:**
1. "测试框架为 gmock/gtest 风格自实现"(任务书存疑项)——**推翻**:vendored 真 googletest(gtest-all.cc),无 gmock。
2. "round-4 R-1 修复后 RootsDlg 完全可用"——**推翻**:浏览分支漏改,浏览按钮静默失效至今(§0 Top1,本轮新发现)。
3. "sym.py 存在 sym2 多版本维护成本"(任务书线索)——**未证实**:tools/ 仅 sym.py 一个诊断脚本;真正的问题是它硬编码 p7 目录。
4. "v2.19 全行取大小可能拖慢搜索首显"——静态读码:池跑在 worker 内、cancel-aware、attribute-only,UI 线程不参与(性能数值未验证,见局限)。

**已验证无问题(抽检记录):**
- include 图 0 环(Tarjan,95 节点);r4 结论复验成立。
- `flush;` 缺陷修复存活(grep 0 命中);v2.10"修改时间列不可见"修复存活(SetListColumns INSERT/DELETE,Presenters.cpp:253-263)。
- 98 测试宏计数与提交自述一致;手抄清单当前 25/25 无漂移。
- rules.json 与内置规则计数一致;T-B1 双层白名单复验点存活(SessionService.cpp:276/696)。
- 单实例+takeover 逻辑结构完整(main.cpp:16-61),goto 收敛于局部。

## 8. 值得保留的实践(增量)

1. r4 清单 1-10 全部存活(外置规则+校验+回退、DoneMessageFor 单源、零环、resource.h 集中、原子写、*ForTesting 纪律、Presenter 快照契约、evolve 记录格式、EnsureBuilt 门面、version.h 单源机制)。
2. **v2.18/v2.19 提交的风险自评段**("display-only, zero data/logic dependencies…")与活体截图验证闭环(v2.19a 擦除缺陷即截图抓到)——小步快跑下的自我验证范式。
3. 菜单命令**重派发回 WM_COMMAND 单点**(MainWindow.cpp:2200-2202)——新增菜单项不再复制 handler。
4. v2.18 PerfBench 预算 Debug 比例放宽(#ifdef NDEBUG)——测试预算与构建形态对账。
5. 代码内**事故考古注释**(v2.17b/v2.16d/L-1 等"当时发生了什么+为什么这么修")——虽未上文档,但考古价值高,是本仓文档债的意外对冲。

## 9. 演进路线图(ROI 排序;S=≤半天 M=1-3 天 L=≥1 周)

| # | 项 | 规模 | 动机(本轮证据) | 做法/增量步骤 | 风险 |
|---|---|---|---|---|---|
| R1 | **修 RootsDlg 浏览按钮**(1134 行 `GetDlgItem(h,1)`→1001)+ 把 1001/100 提入 resource.h 常量 | S | Top1 活缺陷;同一魔数两次手写已实际咬人 | 1 行修复 + 2 个 define 替换;活体点一次浏览验证 | 极低 |
| R2 | **asInvoker 对账**:写定降级行为(非提权时签名链休眠是否可接受/是否加 runas 旁路),更新 ADR-010、ARCHITECTURE、main.cpp/rotate.ps1 注释 | S | 权限模型翻转 3 个版本无评审无文档;安全链前提失效 | 决策一页纸 → 注释/文档三处;可选:`IsElevated()` 时才启用签名 UI 文案 | 低;需用户拍板安全取舍 |
| R3 | **仓库/构建卫生**:gitignore 补 `build/ .tmp-review/ .tmp-shots/`;ad-hoc 配置改 `/p:OutDir=build\diag\`;build.bat 加 clean;build_number.txt 移出 git 跟踪;sym.py 参数化路径 | S | 896MB/10 目录;每次构建脏树;诊断脚本锁死 p7 | 纯仓库操作+脚本参数化;一次性删 10 目录回收 ~890MB | 无 |
| R4 | **开 /WX + Latest 同步归档 PDB**(如 build\Latest\symbols\<build>.pdb)+ 双配置构建自检脚本 | S | v2.17b 自证 WZX 可拦;v2.16d 靠运气符号化 | 先 /W4 清零存量(两工程对齐 W4)→ /WX;PostBuild 拷 PDB 进版本化目录 | 存量告警工作量未知(先跑清单) |
| R5 | **util/Parallel.h 统一池**:游标式 `ForEachParallel(items, fn, cap)` + 队列式 `DirWalkPool`,替换 7 处 | M | 7 池 3 习语 2 种上限;WalkBuild 队列复杂度已产 CRITICAL | 先抽游标式(4 处,机械替换+单测)→ 队列式(SizePool/WalkBuild 合并,谨慎) | 中:队列式合并涉 walk 正确性,分两步走 |
| R6 | **列/消息魔数常量化**:列数 constexpr + per-tab 列描述符;WM_APP 连续编号块 + Post() 改查表 + TaskKind→msg 双射 static_assert/单测 | S | (a)/(e) 走查;数值散布 4+3 处 | 纯重构+1 个单测;给未来"第 7 列/第 9 消息"铺路 | 低 |
| R7 | **静态库拆分**(r4 §2.2:util→platform→core;UiMessages.h 消 core→res) | M | 手抄清单+40.9% 无覆盖的根因;违规边又活一周期 | 三步各自一提交;tests 清单换 3 个库引用 | 链接错误潮(=显形隐藏依赖,收益) |
| R8 | **MainWindow 三拆**:WndProc 命令/NOTIFY 路由表化;Tab 描述符(可见性矩阵+文案数据化);-probe 与 RootsDlg 迁出 | M-L | 486/151 单函数;12 职责簇 | 每拆一块跑 98×2 + 截图回归;目标先 <1500 行 | 中:路由表漏分支=静默失效,R6 的映射断言先行 |
| R9 | **可测性补课**:Presenters/FolderTreeScanner/LargeFileScanner 进测试构建(列渲染/勾选态/多级 linkage 是纯逻辑);FolderTree depth 循环体去重+并行化(r4 #3) | M | 4745 行零覆盖;v2.18 函数内复制 | 随 R7 顺路;FolderTree 先抽 EmitDir= 一函数再并行 | 低-中 |
| R10 | **响应性两债**(启动 TryLoadCachedResults×6 移 worker;关闭 Shutdown 加超时/后台化)+ **文档再生**(ARCHITECTURE 重写、evolve-log 复活为提交门) | S+M | r4 #4/#5 复验未做;evolve 失守=路线图失执行 | 两债各一提交;文档以 §6.2 清单为底稿 | 低 |

**排序汇总**:R1→R2→R3→R4(全部 S,一周内可清完"止血包")→ R6(S,防下一处魔数)→ R5→R7(结构性,二选一先 R7 若要测试红利、先 R5 若要并发红利)→ R9→R8→R10。若只做一件事:**R1+R3**(半小时级,消掉一个活缺陷与全部卫生噪声);若做三件:加 R2(把未评审的安全模型翻转变成有决策记录的架构事实)。

## 10. 未验证项与局限

1. 全部结论静态:未构建、未运行 98 测试、未启动 GUI;Top1 缺陷(浏览按钮失效)为逐行读码+句柄语义推演(IDOK==1 返回按钮控件;EM_REPLACESEL 对 BUTTON 无效),**未活体复现**——建议按约束在 devCacheRoots 对话框点一次"浏览添加…"验证。
2. 工作集 ~905MB@3M 项、v2.19 取大小"几十毫秒/千行"均为提交自述引用,本轮未测;内存结论以容器结构推断(byFrn_/childrenOf_/dirPaths_/filePaths_/frnToDirPath_ 五个哈希表主导)。
3. 函数度量为启发式(±10%;switch 抬高 CC;只计 ≥15 行函数,分母与 r4 不同);"substr@GuardRails.cpp:145"为已知误报(实为 StripPrefix 内调用点被误配对),不影响 Top 榜其余条目。
4. include 图按头文件名→模块解析,`#pragma comment` 链接依赖不入图;tests 链接库差集(srclient pragma 兜底)r4 已记,未重验链接行为。
5. build/ 仅清点文件名与体量(未读产物内容);build_number=95、version.h=v2.19 为文本读取。
6. UIPI 断言(asInvoker 下 takeover 消息可能不通)为机制推演,未构造双实例验证。
7. r4 报告的"Instance()/For() 双入口不产生双索引"未重新验证(引用 r4 结论)。
