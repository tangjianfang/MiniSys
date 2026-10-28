# MiniSys 第七轮评审 · 06 软件工程与可扩展性 + 测试覆盖体系（架构角色第四评）

> 日期：2026-10-29 · 对象：HEAD `6621d92`（v2.23，build_number.txt 工作区已到 109）· 只读静态分析
> 方法：脚本量化（`.tmp-review/r7/exp/`：funcs.py 函数度量、includes.py include 图 + Tarjan SCC）+ git/wc/grep 逐条核验 + 对 r6 报告全部"已修复/未做"结论逐项对码复核。未构建、未运行测试、未启动 exe、未读用户数据、未触碰黑名单脚本（build.bat / rotate.ps1 / screenshot.ps1 / incr_build.bat 仅读文本）。
> 前序：round-4 首评、round-6 第三评（`docs/review-2026-10-14-r6/06-architecture.md`）。本轮按任务书以【100% 测试覆盖路线图】为重点章，架构度量做增量复核。

---

## 0. 摘要

**架构健康度：5 / 10（r4 首评 6，r6 5.5，继续小幅下滑）。** 底子依旧：include 图 0 环（80 节点 Tarjan 复验）、任务代数单源（taskGen + DoneMessageFor）、规则外置 + 校验 + 内置回退、原子写纪律、v2.20 签名链修复全部存活。但三条 r6 判定的下行曲线全部延续：**MainWindow 再 +253 行至 2482（WndProc 长到约 570 行 / 启发式 CC 168）**；**测试编译覆盖率从 59.1% 再降到 57.9%（12 文件 / 5,098 行 / 42.1% 零编译覆盖，UI 层仍然 0）**；**r6 的"一周止血包"4 项只做了 R1（修 bug），R2/R3/R4（权限模型对账、/WX + PDB、仓库卫生）一项未动**。r6 综合路线图 P1 六项仅"批量选择"部分落地（v2.22 反选），B-4 惰性化、T-B5 迁移闸决策、Executing 可取消、内存压缩、导出全部未做。

**本轮新发现 1 个 Confirmed 功能性回归**：v2.22 给快捷键表加了第 4 项（Ctrl+I 反选）但 `CreateAcceleratorTableW(acc, 3)` 的手写计数没有跟着改——**Ctrl+K 命令面板从 v2.22 起在键盘上不可达**，且"信任并签名旧记录"这个升级路径**只存在于命令面板里**，等于整条 v2.14 旧记录采纳路径被无声掐断（§2 Top1）。这与 r6 Top1（RootsDlg 浏览按钮 `GetDlgItem(h,1)`）是同一类病：**手写的计数/ID 字面量第二次咬人**，而两类都可用 `_countof` 或常量化零成本消灭。

### 分维度评分（对比 r6）

| 维度 | r6 | 本轮 | 依据 |
|---|---|---|---|
| 分层与依赖 | 7 | **7/10（持平）** | 0 环存活；core→res 违规仍在（SessionService.cpp:12）；双 God object 继续长（MainWindow +253、SessionService +99） |
| 可测试性 | 4 | **3.5/10（↓）** | 零编译覆盖 5,098 行 / 42.1%（r6 4,745 / 40.9%）；UI 六文件 2,156 行仍 0；99 用例全部打在 core/util |
| 扩展性 | 5 | **5/10（持平）** | 右键菜单🟢/规则数据化🟢存活；列魔数 6/5 散布 3 文件；设置项 6 落点；新 Tab 仍散射 8 处 |
| 横切一致性 | 6 | **5.5/10（↓）** | taskGen 单源存活；但快捷键计数魔数实际咬人；27/37 个 .cpp 零日志（r6 19/29，分母变化后比例不变） |
| 构建与质量基建 | 4 | **3.5/10（↓）** | 无 /WX、无 CI、Latest 仍不拷 PDB、build_number.txt 仍跟踪（工作区正在脏）、build/ 919MB/17 目录（r6 896MB/10） |
| 文档 | 4 | **3.5/10（↓）** | evolve-log 停更 22 天 / 约 30 提交；v2.21–v2.23 四个 UI 版本零文档；ARCHITECTURE 仍是 v1 时代 |

### Top 10 结构性问题

| # | 级别 | 问题 | 证据（file:line） | 置信度 |
|---|---|---|---|---|
| 1 | **P1 功能回归（新）** | 快捷键表手写计数：`ACCEL acc[]` 4 项但 `CreateAcceleratorTableW(acc, 3)` 只注册前 3 项——v2.22（d574af3）插入 Ctrl+I 后 **Ctrl+K 被挤出表外，命令面板键盘不可达**；"信任并签名旧记录"仅存在于面板（MainWindow.cpp:1263）→ 旧记录采纳路径整链不可达。触发：v2.22/v2.23 任意键盘使用 Ctrl+K。修复：`_countof(acc)`（1 行） | MainWindow.cpp:370-376；git show d574af3 确认插入未改计数；空态文案 :637 与 About :2281 仍承诺"Ctrl+K 命令面板" | **Confirmed** |
| 2 | P1 结构 | MainWindow 回弹第二波：2229→**2482**（+253/4 天，全是 v2.21–v2.23 UI）；**WndProc 约 570 行 / CC 168**（r6 486/151）；新增第⑬⑭职责簇：**主题常量与 owner-draw 全家**（DrawTabItem/DrawPrimaryButton/DrawInfoText/zebra/erase，~330 行）与 **v2.23 懒展开树消息链**；方法 37 个 / 数据成员 17 个 | funcs.py；MainWindow.cpp:100-326（绘图簇）、858-872（TREE_CHILDREN） | Confirmed |
| 3 | P1 可测性 | **42.1% 编译单元行数不在测试构建内**（12 文件 5,098 行）：MainWindow 2482 / Presenters 1061 / FastWalk 199 / LargeFileScanner 277 / AppScanner+FolderTreeScanner 318 / ui 三件 546 / main+CrashDump+Icons 215。99 个用例 100% 打在已覆盖的 25 个文件上，**新增的 UI 代码持续以零测试状态堆积**（v2.21–v2.23 四版本约 +530 行 UI 零测试） | 本报告 §2.1 矩阵；vcxproj diff | Confirmed |
| 4 | P1 流程 | **r6 路线图执行率（P1 层）≈ 1/6**：B-4 Finalize 惰性化未做（VolumeIndex.cpp:694 缓存载入后仍同步 Finalize）、T-B5 迁移闸决策未做（GuardRails.cpp:79-80 Program Files 仍在 fileSubtree 保护表）、Executing 可取消未做（SetTaskBusy 在 busy 时禁用 scan 钮，MainWindow.cpp:1907，无取消路径）、内存压缩未做（VolumeIndex.h:216-222 五个 unordered_map 原样）、N3 导出未做（全仓 grep csv/Clipboard 0 命中）、N5 批量选择仅反选落地；止血包 R2/R3/R4 0/3 | 逐项对码 | Confirmed |
| 5 | P1 重复 | 手写线程池 **7 处 / 3 习语 / 上限 6/8 两种**原样存活：游标式 fetch_add ×4（JunkScanner.cpp:279-310、LargeFileScanner.cpp:91-111、DevBuildCache.cpp:300-337、SessionService.cpp:639-667 取大小池）+ 队列式 ×3（PathUtils.cpp:174-270、VolumeIndex.cpp:557-653 WalkBuild、FastWalk.cpp:56-）；`hardware_concurrency` clamp 样板逐字出现在 **7 个文件** | grep std::vector<std::thread> + hardware_concurrency | Confirmed |
| 6 | P2 可测性 | **两份手抄源码清单**风险原样：MiniSysTests.vcxproj 手抄 25/37 个 app .cpp（本轮核对 25/25 无漂移）；新 .cpp（如 Settings.cpp 进了，但 ui/ 从未进）全靠人肉加行；两工程警告级别仍漂移（主 /W4、测试 /W3），`TreatWarningAsError=false` | MiniSysTests.vcxproj:79-103 vs MiniSys.vcxproj | Confirmed |
| 7 | P2 扩展 | 列模型魔数仍无常量：**6 列**（scan tabs）/ **5 列**（history）以字面量散布 Presenters.cpp:140-143（kScanCols 定义，唯一半集中点）、158-167（`i*6+k` 六处）、195-196（`col>5`/`row*6`）、484-529（sortCol_ case 0-5）、954-957/959-969（`i*5+k`）、980（`col>4`）；Controls.cpp:59-63 创建时又写死 5 列（初始状态依赖 SetListColumns 补插第 6 列） | 读码 | Confirmed |
| 8 | P2 横切 | **主题不可切换**：v2.23"浅色主题"实为 2 个命名常量（kChromeBgr/kTabPill，MainWindow.cpp:122-123）+ **13 处内联 RGB()**（斑马纹 :558、风险色 :606-612、强调色 :207-208、禁用色 :220、文字灰阶 :179-181/:311-313、发丝线 :266）散在 4 个绘图函数里；高对比度分支与具体颜色逐处 if-else 交织，无调色板结构 | MainWindow.cpp 全量读码 | Confirmed |
| 9 | P2 基建 | build/ **919MB / 17 个输出目录**（r6 896MB/10，p5-p9/probe/probe2-4/tour/tour2/verify 又加了 7 个）；`.gitignore` 仍漏 `build/`、`.tmp-review/`、`.tmp-shots/`（git status 三条 untracked 噪声常驻）；build_number.txt 仍被跟踪（本轮工作区正脏：109）；Latest 部署仍只拷 exe+rules.json **不拷 PDB**（build.bat 尾部，r6 R4 未做）；build_log_full.txt **仍被 git 跟踪**（r6 报告称"已删"不实，见 §10） | du / git ls-files / build.bat | Confirmed |
| 10 | P2 横切 | 日志覆盖不均依旧：MS_LOG 共 53 处集中在 10 个文件，**27/37 个 .cpp 零日志**（全部扫描器、全部 ui/、全部 platform/ 除 SystemRestore、DeleteOp/QuarantineOp/Junction/DelegateOp）——错误范式 bool+errOut 只进 UI 文案不进日志；线程边界契约仍纯注释（GetCurrentThreadId 全仓仅 CrashDump.cpp:52 一处；static_assert 全仓 1 处） | grep -c 逐文件 | Confirmed |

---

## 1. 度量数据

### 1.1 规模与编译单元行数（wc -l 实测；cpp 合计 12,109 / 头 2,275 / 测试 1,993；r6 时 cpp 11,602）

| 文件 | 本轮 | r6 | Δ | 说明 |
|---|---|---|---|---|
| MainWindow.cpp | **2482** | 2229 | **+253** | v2.21 单强调色钮/信息卡、v2.21a 斑马纹、v2.22 owner-draw Tab 条、v2.23 浅色主题+懒树 |
| core/VolumeIndex.cpp | 1399 | 1388 | +11 | |
| core/SessionService.cpp | 1193 | 1094 | **+99** | v2.23 FolderChildrenAsync 懒展开取子节点（含一个取大小池） |
| ui/Presenters.cpp | 1061 | 986 | +75 | 懒树节点工厂/两轮挂接/路径映射 |
| core/GuardRails.cpp | 468 | 468 | 0 | |
| core/OperationLog.cpp | 417 | 390 | +27 | 签名相关 |
| core/JunkRules.cpp | 411 | 406 | +5 | |
| core/JunkScanner.cpp | 405 | 405 | 0 | |
| util/Json.cpp | 364 | 364 | 0 | |
| core/DevBuildCache.cpp | 356 | 356 | 0 | |
| util/PathUtils.cpp | 285 | 285 | 0 | |
| 其余 26 个 .cpp | ≤277 | — | ≈0 | LargeFileScanner 277 / MoveJunctionOp 251 / FastWalk 199 / DelegateOp 193 / CommandPalette 189 / Junction 188 / Controls 179 / Layout 178 / QuarantineOp 167 / FolderTreeScanner 159 / AppScanner 159 / SecretStore 156 / Hash 132 / DeleteOp 115 / Logger 104 / Settings 83 / main 80 |

v2.19a→v2.23 的 **+507 行净增量里约 86%（+438）落在 4 个最大文件**（MainWindow/Presenters/SessionService/VolumeIndex），God object 吸聚趋势未变。

### 1.2 函数长度 / 圈复杂度 Top（funcs.py 启发式：括号配对 + 关键词计数；switch/TaskDialog 抬高 CC；只计 ≥15 行函数 127 个，**分母与 r6 脚本不同，只比同一脚本口径的趋势**）

| 函数 | 行 | CC | r6（行/CC） | 备注 |
|---|---|---|---|---|
| MainWindow::WndProc | **≈570** | **168** | 486/151 | 435-1005；8 类消息 + WM_NOTIFY 内嵌 NM_CUSTOMDRAW 三段 owner-draw（tab/复选框/风险色/斑马纹） |
| DevBuildCache::Scan | 174 | 48 | 170/47 | |
| FolderTreeScanner::Scan | 122 | 37 | 140/39 | v2.23 收敛（懒展开移走了深层遍历） |
| VolumeIndex::BuildFull 附近（:371 起 117 行） | 117 | 27 | 194/36 口径差 | |
| CommandPalette::Show | 101 | 25 | — | 含手写模态泵 |
| VolumeIndex 缓存载入（:568 起 90 行） | 90 | 21 | — | fread 链 |
| MainWindow::OnCreate | 78 | — | — | 1007-1084 |
| MainWindow::OnExecute | 184 | ~30 | 184/36 | 2019-2202，确认文案组装仍单函数 |
| MainWindow::OnDevCacheRoots | 108 | — | — | 1398-1505 手写模态对话框 |
| MainWindow::OnTabChanged | 107 | — | — | 1569-1675 |

≥100 行 8 个（r6 口径 20 个）；≥50 行 31 个；CC≥20 16 个、CC≥30 5 个。**榜首次序与 r6 一致，唯一恶化点仍是 WndProc。**

### 1.3 include 图（includes.py，80 节点 / 190 边）

- **环：0**（Tarjan 全单点 SCC）——r4/r6 结论三连存活。
- fan-in Top：StringUtils.h 25、PathUtils.h 22、Logger.h 20、Scanner.h 10、OperationLog.h 8——util 地位不变。
- 跨层：core→util 53、core→core 53、ui→core 12、ui→ui 11、app→core 9、app→ui 7、core→platform 7、platform→util 6、**core→res 1（SessionService.cpp:12 → res/resource.h，第三轮存活）**、ui→res 2。
- fan-out Top：MainWindow.cpp 19、SessionService.cpp 15、Presenters.cpp 15。

### 1.4 God object 职责簇（对照 r6 增量）

- **MainWindow（2482 行 / 37 方法 / 17 数据成员）**：r6 十二簇全在；新增 ⑬ **主题与 owner-draw 全家**（kChromeBgr/kTabPill + DrawTabItem + DrawPrimaryButton + DrawInfoText + WM_ERASEBKGND + NM_CUSTOMDRAW 内的斑马/复选框/风险色，合计 ~330 行全在本文件匿名 namespace 与 WndProc 内）⑭ **v2.23 懒树消息链**（WM_APP_TREE_CHILDREN 堆载荷搬运 + FolderTreePresenter dynamic_cast 三连）。`dynamic_cast` 26 处（r6 21）。
- **SessionService（1193 行单例）**：扫描/搜索/索引/执行/维护/缓存/进度/撤销/verify/preview/懒取子节点 —— 第 11 簇。
- **VolumeIndex（1399 行）**：九簇 + -probe 诊断 API 原样。

### 1.5 r6 Top10 / 路线图执行核验（本轮逐条对码）

| r6 项 | 本轮核验 | 结果 |
|---|---|---|
| R1 RootsDlg 浏览按钮 GetDlgItem(h,1) | MainWindow.cpp:1377 已是 `GetDlgItem(h, 1001)`，注释注明三源复核 | ✅ 已修复（但 1001/100 内联魔数**未**提入 resource.h，r6 R1 后半未做） |
| T-B1 HMAC flag | SecretStore.cpp:81-82 `BCRYPT_ALG_HANDLE_HMAC_FLAG` | ✅ 已修复 |
| B-2 Logger UTF-8 | Logger.cpp:17-23 窄流 + 显式 UTF-8 | ✅ 已修复 |
| B-1 缓存脏标志 | VolumeIndex.cpp dirtySinceSave_（:153-154、299-301、826） | ✅ 已修复 |
| B-3 空闲维护抢占 | maintenanceActive_（SessionService.h:205）+ WM_ACTIVATE 取消（MainWindow.cpp:989-993）+ 15 分钟冷却（:1554-1556） | ✅ 已修复 |
| X-41 复选框自绘 | MainWindow.cpp:564-590 owner-draw glyph | ✅ 已修复 |
| R-2 GetDlgItem(h,1) | 同 R1 | ✅ |
| B-4 Finalize 惰性化（P1） | VolumeIndex.cpp:689-694 缓存载入后仍直接 Finalize() | ❌ 未做 |
| T-B5 迁移闸决策 | GuardRails.cpp:79-80 Program Files 仍全量入 fileSubtree 保护；无决策文档 | ❌ 未做 |
| Executing 可取消 | SetTaskBusy busy 时禁 scan（MainWindow.cpp:1907），无取消路径 | ❌ 未做 |
| 止血包 R2 asInvoker 对账 | ARCHITECTURE.md:17/289/306 与 DESIGN-v2.md:311 仍写 requireAdministrator；main.cpp:20 注释"both are elevated"仍在 | ❌ 未做（文档三级漂移存活） |
| 止血包 R3 卫生 | .gitignore 无 build/；build_number.txt 跟踪中 | ❌ 未做 |
| 止血包 R4 /WX + PDB | 两工程 TreatWarningAsError=false / Level3；build.bat 不拷 PDB | ❌ 未做 |
| N5 批量选择 | v2.22 InvertChecked（Presenters.cpp:254-264）+ Ctrl+I | 🟡 部分（无"按风险选择"） |
| N3 导出 | 全仓无 csv/剪贴板代码 | ❌ 未做 |

**r6 路线图执行率：P0 修复 8/8 ✅（d8ee0e4 交付质量高）；P1 结构项 1.5/7；止血包 1/4。** 修复纪律好、结构纪律差——与 round-4 后的形态完全相同。

---

## 2. 【重点章】100% 测试覆盖路线图

用户目标："实现 100% 测试用例的覆盖，包括 UI"。本章给出可执行路线：先量化现状，再分层给出现实上界与逐 commit 迁移步骤。

### 2.1 现状矩阵：源模块 × 用例数 × 覆盖性质

测试真值（宏计数实测）：**99 用例 = 80 TEST + 19 TEST_F，14 个测试文件，420 处 EXPECT/ASSERT**（r6 为 98/79+19/412；v2.20 后 +1）。gtest 为 vendored（third_party/googletest，仅 include/ + src/，测试工程编译 gtest-all.cc；**无 gmock**；版本号"1.15.2"仅见实施报告自述，vendored 树无版本标记可验）。

| 源模块（行数） | 进测试编译 | 专属测试文件（用例数） | 覆盖性质判定 |
|---|---|---|---|
| core/GuardRails 468 | ✅ | GuardRailsTests 7 + GuardRailsP0 13 + GuardRailsAdversarial 15 | **密集行为覆盖**（含 HMAC/签名链） |
| core/DevBuildCache 356 | ✅ | DevBuildCacheTests 13 | 密集 |
| core/VolumeIndex 1399 | ✅ | VolumeIndexTests 9（TEST_F 8，AddNodeForTesting 注入） | **部分**：注入路径覆盖，Search/Finalize 边角多（CC 27-44 函数群） |
| util/Json 364 | ✅ | JsonTests 6 | 良好（27 坏样本 fuzz 在 r6 driver 内） |
| core/SessionService 1193 | ✅ | SessionServiceTests 6 | **稀疏**：FakeScanner/BlockingScanner/ThrowingScanner 生命周期；RunPlan/搜索/懒展开 0 |
| core/OperationLog 417 | ✅ | OperationLogTests 5 | 中等 |
| core/JunkRules 411 | ✅ | JunkRulesTests 5 | 中等（27 规则数据校验） |
| core/PlanBuilder 68 | ✅ | PlanBuilderTests 5 | 良好 |
| core/DelegateOp 193 | ✅ | DelegateOpTests 6（TEST_F） | 中等 |
| core/QuarantineOp 167 | ✅ | QuarantineOpTests 3 | 中等 |
| core/Scanner.h ScanItem | ✅ | ScanItemTests 3 | 序列化 |
| 跨模块 | ✅ | PerfBenchTests 3 | 预算型 |
| platform/SecretStore 156 | ✅ | （无专属；TrustChain 2 例在 GuardRailsAdversarialTests.cpp:229-275） | 部分；`secret.empty()` 早退仍是空转残径（v2.20 已修主路径） |
| core/JunkScanner 405 | ✅ | **无** | **仅编译覆盖** |
| core/MoveJunctionOp 251 | ✅ | **无** | 仅编译覆盖（四步预检/回滚 0 断言） |
| core/DeleteOp 115 | ✅ | **无** | 仅编译覆盖 |
| core/Settings 83 | ✅ | **无** | 仅编译覆盖（**无往返测试**——加一个 SettingsRoundtrip 是 S 级速赢） |
| platform/Hash/Junction/Privilege/RestartManager/SystemRestore、util/PathUtils/StringUtils/Logger/DirSizeCache/Win32Error | ✅ | **无专属** | 间接覆盖（被上层用例穿过）；PathUtils SizePool 285-270 池 0 断言 |
| **MainWindow 2482** | ❌ | — | **零** |
| **ui/Presenters 1061** | ❌ | — | **零**（cells_ 生成/勾选态/快照契约/ApplyDeadPaths 全部纯逻辑可测而未测） |
| ui/CommandPalette 189 | ❌ | — | 零（V1 按钮修复靠人工） |
| ui/Controls 179 / ui/Layout 178 / ui/Icons 67 | ❌ | — | 零 |
| core/LargeFileScanner 277 / AppScanner 159 / FolderTreeScanner 159 | ❌ | — | **零**（三个扫描器 + util/FastWalk 199 = 794 行核心算法零测试） |
| util/FastWalk 199 | ❌ | — | 零（并发遍历/reparse 剪枝/取消传播 0 断言） |
| main 80 / platform/CrashDump 68 | ❌ | — | 零（合理豁免候选） |

**零覆盖高风险 Top5**：MainWindow（2482，含全部命令路由与确认流）、Presenters（1061，虚拟列表数据契约——v2.15/v2.16d 两次线上事故都在此文件）、LargeFileScanner+FastWalk（476，top-N/去重/并发遍历核心算法）、FolderTreeScanner（159，v2.18/v2.23 两次重写零回归网）、CommandPalette（189，有自己的消息循环）。

### 2.2 测试基建评估

1. **两份手抄清单：是，漂移风险现行**。MiniSysTests.vcxproj 逐条 `ClCompile Include="..\MiniSys\src\..."` 25 个（+gtest-all+15 个测试 cpp）；主工程 37 个。本轮 25/25 与磁盘一致、无多余项，**当前无漂移**——但漂移不报错、链接失败才暴露：主工程新增 .cpp 若被既有测试引用符号，测试工程漏加即 LNK2019；漏加新文件则纯静默零覆盖（ui/ 六文件从未进过就是实例）。
2. **gtest 来源**：vendored third_party/googletest（src/gtest-all.cc 编入测试工程，include 三路径见 vcxproj:52）。无 FetchExternal、无包管理——离线可复现（优点），升级靠手工（次要）。
3. **mock 手段盘点**（现状可用的缝）：
   - `Scanner` 纯虚接口 → FakeScanner/BlockingScanner/ThrowingScanner（SessionServiceTests.cpp:22-58）——**唯一真正的多态 mock 点**；
   - `SessionService::SetWindow(nullptr)` 关闭全部 PostMessage（SessionService.h:32）——"窗口 mock"的全部；
   - `*ForTesting` 静态 **8 个**：Logger::SetLogPathForTesting、SessionService::SetCacheDirForTesting、OperationLog::SetHistoryPathForTesting、VolumeIndex::ResetForTesting/AddNodeForTesting/FinalizeForTesting/ApplyChangesForTesting——测试进程全局重定向（tests/main.cpp:18-29 统一做日志+缓存），**该模式可推广**且已证明能防真实用户数据污染（V-R5 事故后建立）；
   - **无接口的核心服务**：SessionService 单例、VolumeIndex::For 静态注册表、OperationLog 单例、GuardRails 全静态——无法替换实例，只能"真体 + 重定向"。Presenters 对 SessionService::Instance() 的直接调用（Presenters.cpp:62 等）决定了 presenter 测试必须与真单例共存（可行，见 2.4）。
4. **测试不碰 UI 窗口**：全部用例在控制台进程跑，无 HWND 创建——因此 ui/ 层从未被编译进来也从未被需要；这是覆盖率数字与"UI 可测性"的双重后果。

### 2.3 UI 测试策略（Win32 无头/有头）

**关键可行性事实（本轮读码确认，静态推断未运行）**：`UiHandles` 是纯 HWND 值结构、全部成员 nullptr 默认（UiHandles.h:11-35）；Presenters 对控件的一切访问都经 `SendMessageW` 族宏（ListView_GetHeader/RedrawItems/SetItemState…），**对 nullptr HWND 调用安全返回 0**。因此 ListTabPresenter/HistoryPresenter 在无头测试进程里可以真实运行：Refresh→RenderItems 的控件消息全部无害空转，而 snapshot_/cells_/checkStateByPath_ 等数据成员照常演化。这是"Presenter 层无头直测"的地基，不需要任何 mock 框架。

分层拆分（每层一个可合入 commit）：

**L1 抽纯函数（S，先行）**：把 Presenters.cpp 匿名 namespace 与静态成员中的纯文本函数抽到新头 `ui/ListText.h`（实现放新 cpp，编译进测试）：
- `LocalizeCategory`（Presenters.cpp:362-383，含 Duplicate 递归）
- `OneLine`（:323-337）、`FormatTimeShort`（:341-358）
- `RiskBadge`（:393-408，改自由函数）
- `HistoryTypeLabel`/`HistoryRiskLabel`（:992-1019，从 HistoryPresenter 静态成员移出；HistoryRiskLabel 依赖 OperationLog::VerifySignature——真体可用，测试里已有签名基建）
- `FormatCount`/`FormatAge`（MainWindow.cpp:59-98，同抽）
→ 直测 30-40 个用例（大小格式化边界、时间戳 0、Duplicate 嵌套、未签名记录标签等）。

**L2 Presenter 无头直测（S-M，核心红利）**：把 `ui/Presenters.cpp` 加进 MiniSysTests 清单，新建 PresenterTests.cpp：
- 构造 `ListTabPresenter(TabId::Junk, UiHandles{})`；
- 经 `SessionService::Instance().StoreResults(tab, items)` 喂数据（真单例 + SetCacheDirForTesting，模式与 SessionServiceTests 相同）→ `Refresh()` → 断言 `Snapshot()`/`cells_`（经 SupplyDispInfo 读出）；
- **SupplyDispInfo 用假 NMLVDISPINFOW**：栈上构造 `{.item={.iItem=row,.iSubItem=col,.mask=LVIF_TEXT,.pszText=buf,.cchTextMax=64}}` 直接调用——正是 v2.16d 越界崩溃（Presenters.cpp:183-190 注释）的那条路径，今天它零测试；
- 勾选态语义：SyncCheckDefaults 种子（recommended）、ToggleCheckAt 未见过路径取 !recommended、InvertChecked、SetAllChecked、CollectChecked —— v2.22 反选逻辑（:254-264）目前 0 断言；
- ApplyDeadPaths 剪枝 + StoreResults 同步 + $RECYCLE.BIN 伪项豁免（:426-451）；
- 排序：OnColumnClick(col) × 6 列 × 升降序，断言 Snapshot 顺序（排序 permutes snapshot，RestoreViewState 对 null HWND 空转，安全）。
- 预计 40-60 用例，直接把 1,061 行从零覆盖变为主体覆盖。

**L3 MainWindow 中的纯逻辑抽离（S-M）**：`ComposeScanDoneText`（OnScanDone 的 dashboard/降级通知组装，MainWindow.cpp:1826-1877）、`CountActionable`（UpdateExecButton 的 InfoOnly 过滤+合计，:1986-1995）、执行确认文案组装（OnExecute 的 risk 分组，:2069-2135）、`ComposeScanTimeLine`——全是"结果→文案"纯函数，抽到 `ui/SummaryText.cpp` 进测试。**这不需要 MainWindow.cpp 进测试工程**，即可覆盖它最有回归风险的 1/3。

**L4 OnPlanDone/OnScanDone 状态机提取（M）**：把 `SetTaskBusy` 的控件矩阵参数化为 `{各控件 → 期望态}` 数据表（本身就能单测"Indexing 时 tab 可用、list 禁用"这类 r6 反复咬人的矩阵错误），OnPlanDone 的反馈分支（failed/skipped/quarantined/freed 四象限）提取为返回结构体的纯函数。

**L5 窗口级最小宿主（M，"dtrepro2.cpp 模式"）**：.tmp-shots/dtrepro2.cpp 已证明该工作流可行（vcvars64 + /utf-8 编译的小 exe 创建真实控件、跑 WM_DRAWITEM、printf 断言 + PrintWindow 截图）。正式化为两条：
- a) **无头真窗口 gtest**：MiniSysTests 增加一个 fixture（`#ifdef MINISYS_UI_TESTS`），在测试进程里 `MainWindow::Create(hInst, SW_HIDE)`——控制台进程可以注册窗口类并创建消息窗口（静态推断；需一次实验确认 InitCommonControlsEx/comctl6 清单依赖）。随后 `SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDC_BTN_SCAN,0),0)` + 手动泵几轮消息，断言 presenter 快照/状态栏文本/按钮使能。**模态对话框是主要障碍**——但项目已有天然缝：`dialogs::ConfirmTask`（ui/Dialogs.h）是全部破坏性确认的唯一咽喉，TaskDialogIndirect 调用集中在此 + OnExecute/OnAbout 两处；给它加一个可重定向的函数指针（SetConfirmHandlerForTesting），模态即变可测。
- b) **截图回归**：已有 -tour + tools/screenshot.ps1 先例（被动 PrintWindow，不注入输入）；配合 .tmp-shots 的墨水密度法（项目记忆：vision 对 9pt 字重不可靠）做像素级断言。
- 风险与限制：WM_TIMER 驱动的空闲逻辑、GetLastInputInfo 等系统态在宿主里行为不同——这些进豁免清单。

**L6 Win32 壳豁免清单（现实上界的另一半）**：明确不追求行覆盖 100% 的部分：main.cpp 单实例/takeover 轮询、CrashDump、Privilege、真实 ShellExecute/SHBrowseForFolder/TrackPopupMenu 弹出、DrawText 像素效果、DPI 跨屏行为。这些靠 L5 冒烟 + 截图回归兜底，不追行覆盖数字。

### 2.4 行覆盖 100% 的现实上界（分层目标）

| 层 | 内容 | 目标 | 依据 |
|---|---|---|---|
| 纯逻辑层 | Json/PathUtils/StringUtils/JunkRules/PlanBuilder/GuardRails/ListText(L1)/SummaryText(L3) | **行覆盖 100% 可达** | 已是 99 用例的主场；L1/L3 抽离补齐 |
| 粘合层 | Presenters/SessionService/OperationLog/VolumeIndex/各 Op/Settings | **≥90%**（分支覆盖为主） | L2 直测 + RunPlan/懒展开/回滚补课；VolumeIndex Search 的 CC44 函数群最难 |
| Win32 壳 | MainWindow 路由/绘图、Controls/Layout/Icons/CommandPalette、main/CrashDump/Privilege | **不追 100%**；冒烟 + 截图，行覆盖 ~50-70% | 模态/像素/系统态不可确定性断言 |

量化：12,109 行 cpp 中豁免壳约 1,500-1,800 行（MainWindow 绘图与 Shell 段 ~1,200 + main/CrashDump/Privilege/Icons ~250 + Controls/Layout 大部）→ **全仓行覆盖现实上界 ≈ 85-88%**；若把"100%"定义为"除豁免清单外 100%"，则 L1-L6 全部落地后可宣告达成。**诚实的说法：字面 100% 行覆盖（含 Win32 壳）在本架构下不可达也不值得追**；用户目标应落为"可测单元 100% + 壳层冒烟化"。

### 2.5 增量迁移步骤（每步一个可合入 commit，按 ROI 排序）

| 步 | commit 内容 | 规模 | 立即收益 |
|---|---|---|---|
| T0 | 修 ACCEL 计数（`_countof(acc)`）+ 顺手 `CreateAcceleratorTableW` 处加 static_assert 风格注释 | S（1 行） | 消掉活回归 |
| T1 | MiniSysTests.vcxproj 加入 FastWalk/LargeFileScanner/AppScanner/FolderTreeScanner + 各配最小临时目录用例（%TEMP% 真文件树，模式同 OperationLogTests 的 per-test path） | S-M | 794 行核心算法脱离零覆盖 |
| T2 | Presenters.cpp 进测试清单 + PresenterTests（§2.3 L2 全项） | M | 1,061 行；v2.16d 事故路径获得回归网 |
| T3 | ui/ListText.h 抽离（L1）+ 用例 | S | 文案/格式化 30+ 用例 |
| T4 | ui/SummaryText 抽离（L3）+ 用例 | S-M | MainWindow 最易回归 1/3 可测 |
| T5 | dialogs::ConfirmTask 可重定向 + SetTaskBusy 矩阵数据化 + 矩阵用例 | S-M | 破坏流可注入 |
| T6 | UI 宿主 fixture（L5a）+ 3 个冒烟用例（OnCreate/OnTabChanged×6/命令重派发） | M | MainWindow 首次进测试 |
| T7 | 列数 constexpr + per-tab 列描述符 + TaskKind→DONE 消息双射用例（r6 R6 遗愿） | S | 魔数再咬人前布防 |
| T8 | 覆盖率度量接入（OpenCppCoverage 或 VS /cov）→ build.bat 输出报告（只读步骤） | S | 后续一切目标可度量 |
| T9 | 静态库拆分（r6 R7：util→platform→core 三步）→ 手抄清单问题连根拔 | M-L | 清单漂移绝迹；测试链接库而非抄源码 |

---

## 3. 分层与可测试性

- **依赖纪律**：0 环、fan-in 合理、层间方向正确——这是四轮评审一贯的底子，仍成立。
- **core→res 违规**（SessionService.cpp:12 include resource.h 取 WM_APP_* 消息号）第三轮存活：修法仍是 r6 R7 的 UiMessages.h 下沉，10 分钟工作量，未做。
- **单例/静态注册表不可注入**：VolumeIndex::For、SessionService::Instance、OperationLog::Instance、GuardRails 全静态——mock 不可行，测试只能"真体+重定向"。8 个 *ForTesting 静态是现行补偿，**建议推广为新代码规约**（任何写 %LOCALAPPDATA% 的新组件必须带 SetXxxDirForTesting）。
- **两份清单**：见 §2.2；漂移当前 0，风险结构性存在。
- **Presenters 的隐性可测性**（本轮新结论，静态推断）：null-HWND 安全性使 L2 路线不需要重构前置——这是与 r6"Presenters 0 单测"结论相比的新机会点，值得先写一个 spike 验证（见 §11 局限）。

---

## 4. 扩展性走查（逐项追踪代码，改动文件清单 + 行数估计）

**(a) 新增主列表列 — 🟡（改文案/宽度）/ 🔴（加新列）**
现状路径：kScanCols 一处定义（Presenters.cpp:140-143）是唯一集中点，但 `i*6+k` 六处（:162-167）、`col>5`/`row*6`（:195-196）、sortCol_ case 0-5（:484-529）、Controls.cpp:59-63 初始 5 列（与 presenter 的 6 列**天然不同步**，靠 SetListColumns 运行时补插）、历史页另一套 5 列（:954-957、:959-969、:980）。加第 7 列 ≈ 4 文件 10+ 处魔数，且 Controls 初始列与 presenter 列的差集是 v2.13c 事故（第 6 列不可见）的病根未除。**修复前置 = T7 列描述符**，之后加列 = 1 条表项 + 1 个格式化 lambda。

**(b) 新增数据写操作（对照 v2.14/v2.20 真实提交形态）— 🟡**
管线确实存在且是资产：PlanBuilder::Build → GuardRails 复验 → SessionService::RunPlan 分派 → Op 类 Execute/Undo → OperationLog。新增一种操作触碰：OpType 枚举（Operation.h）+ OperationLog 序列化/反序列化两处 case（OperationLog.cpp:381、:392）+ HistoryTypeLabel case（Presenters.cpp:994-999）+ 新 Op 类 + RunPlan 分派 + PlanBuilder 构造 + GuardRails 策略 + 确认文案 + 测试 ≈ **7-8 文件 / 300-500 行**。结构清晰但枚举散射（同一语义 4 处 switch），漏一处 = 历史页显示错/旧档读不出——建议 OpType→元数据表驱动（label/serialize/colorize 一处）。

**(c) 新增右键菜单项 — 🟢**
OnListContextMenu AppendMenuW 一行 + resource.h IDM + WM_COMMAND case + 单点重派发（MainWindow.cpp:2457 `SendMessageW(hwnd_, WM_COMMAND, ...)`）= 2 文件 5-8 行。r6 结论存活。

**(d) 新增设置项 — 🔴（六落点 + 无往返测试）**
Settings.h 字段 → Settings.cpp Load 一行 + Save 一行（手写序列化，Settings.cpp:29-40/45-58）→ MainWindow LoadSettings（:2323-2341）+ SaveSettings（:2343-2360）各一段 → 消费者 → 可选 UI（resource.h + Controls + Layout + OnTabChanged 可见性 + 命令面板）。默认值双份（struct 内 + AsInt 兜底）。**无 schema/迁移/往返测试**——而 Settings.cpp 已在测试编译里，加 SettingsRoundtrip 用例是全仓性价比最高的一颗测试（S，10 行 fixture）。devCacheRoots 型带对话框设置项另需 ~110 行手写模态框（OnDevCacheRoots + RootsDlgProc，:1318-1505）。

**(e) 新增事件并显示 — 🟡**
WM_APP+1..+9 已用满无空洞（resource.h:60-73），下一个 +10；但定义分居两个不连续块且 +9 排在 +7/+8 前。契约链 6 处：resource.h define + DoneMessageFor case（SessionService.cpp:38-48）+ TaskKind 枚举 + **Post() 里硬编码 6 个消息名的 gen 戳过滤（:79-81）** + WndProc case + handler。生产端单源👍，消费端无双射校验——T7 的双射用例仍是补法。

**(f) 新增主题 — 🟡（当前实际是 🔴 需先抽调色板）**
v2.23"浅色主题"= 2 常量 + 13 处内联 RGB + 4 个绘图函数里逐处的高对比度 if-else（详见 §0 Top8）。切深色主题需触碰 WndProc erase、DrawTabItem、DrawPrimaryButton、DrawInfoText、zebra、风险色共 ~15 处并重新审计每处 hcOn 分支。**先做 Theme 调色板结构体（struct Theme { chromeBgr, tabPill, accent, accentText, textPrimary, textSecondary, warn, zebra, hairline, … } + LightTheme()/HighContrastFallback()）**，绘图函数只读 Theme——之后加主题 = 一个新函数 + 设置项。S-M 规模。
另注：主题相关 v2.21-2.23 四版本在 docs/ 零记载（ARCHITECTURE 仍无 ui 层描述）。

**(g) 新增 Tab/Presenter — 🔴（未变）**
TabId.h + TabName（MainWindow.cpp:46-56）+ OnCreate presenter 注册（:1019-1024）+ **OnTabChanged 可见性矩阵 15+ ShowWindow（:1578-1604）+ 六路 tabInfoText switch（:1614-1648）+ 扫描按钮语义（:1610、:1912）** + Controls + Layout + Settings.lastTab + 命令面板 kTabs + SessionService per-tab 数组（唯一自动部分）。Tab 描述符（可见性/文案/按钮语义数据化）仍是正解，r6 R8 一半，未动。

---

## 5. 横切一致性

| 关注点 | 现状 | 评价 |
|---|---|---|
| 错误范式 | bool + `std::wstring& errOut` 全仓统一；std::error_code 用于 fs 调用（11 文件）与 Win32 GetLastError 并存但边界清晰（fs 用 ec、API 用 GetLastError+FormatW） | 范式一致✅；**可观测性弱**：错误进 UI 文案不进日志 |
| 吞异常策略 | SessionService worker 有 catch 全包（StartTask body 包裹，6 处 catch）；UI 线程无 try（Win32 惯例，CrashDump 兜底） | 可接受；建议 worker catch 里统一 MS_LOG |
| 日志 | MS_LOG 53 处/10 文件；27/37 零日志（全部扫描器、全部 ui/、platform 大部） | 不均如 r6；扫描失败排障仍只能靠 UI 文案转述 |
| 线程边界 | VolumeIndex.h:68-71 "worker-only" 注释契约存活；GetCurrentThreadId 全仓 1 处（CrashDump）；无 AssertOwnerThread | **纯注释第四轮**；建议 SessionService 加 debug-only 断言（S） |
| 魔数 | resource.h 集中纪律👍；例外仍在且**又咬一次**：ACCEL 计数（本轮 Top1）、RootsDlg 内联 1001/100（MainWindow.cpp:1337/1377/1444/1448，r6 修复了 bug 但常量化未做）、CommandPalette 内联 1/2 控件 id（CommandPalette.cpp:113/116） | 手写计数/ID 是本项目已两次兑现的风险 |
| 任务代数 | taskGen + DoneMessageFor + UI 按 gen 丢弃，单源存活；Post() 内 6 消息名硬编码 | 生产端好；消费端待双射用例 |
| 手写模态泵 ×3 | 主循环、RootsDlg（OnDevCacheRoots 内）、palette::Show——三处各自处理 WM_QUIT 转发（R-1/R-2 修复后各自正确） | 习语重复，且每处都是曾经出 bug 的地方；可抽 RunModalLoop(owner, dlg, pump) |

---

## 6. 构建 / 质量基建与仓库卫生

- **警告**：主工程 /W4 + TreatWarningAsError=false；测试工程 /W3（工程间漂移）。v2.17b bool 截断事故的自证（"Warnings-as-errors would have caught it"，VolumeIndex 注释）仍挂着，/WX 第五轮未开。开 /WX 前需清存量（数量未测，见局限）。
- **PDB/符号**：两工程双配置 GenerateDebugInformation=true（PDB 有产）；但 build.bat Latest 部署只拷 exe+rules.json（build.bat 尾部实测）——**旧 dump 事后符号化仍靠运气**，r6 R4 后半未做。
- **CI**：仍无（无 .github/）；构建+双配置测试纪律全靠提交自述。
- **版本戳副作用**：incr_build.bat 为 PreBuildEvent（MiniSys.vcxproj:47-49），每次构建 bump build_number.txt（**git 跟踪中**，本轮工作区即脏：109）并重生成 version.h（已 gitignore）→ 每次构建至少重编 MainWindow.cpp + MiniSys.rc 两 TU + 脏工作树。修法（r6 R3）：build_number.txt 移出跟踪、或改读环境变量/时间戳。
- **仓库卫生**：跟踪文件 184；build/ 919MB 未纳管但 **17 个 ad-hoc 输出目录**（Debug/Debug-verify/Release/Release-tour/Release-tour2/Release-verify/intermediate/Latest/p5-p9/probe/probe2-4）；`.gitignore` 模式 `[Rr]elease/` 匹配不到 `Release-verify`/`Release-tour` 等（r6 已指出未修）；`.tmp-review/`、`.tmp-shots/`、`build/` 仍三条 untracked 常驻噪声；**build_log_full.txt 仍被跟踪**（2 字节空壳，r6 报告"已删"不实——见 §10）；minisys-shot.png 在 .gitignore 有条目（本轮核对其为 untracked，OK）；sym.py 仍硬编码 `build\p7`。无 README。

---

## 7. 测试体系与文档漂移清单

### 7.1 测试体系
见 §2（重点章）。补充三点：a) **Debug+Release 双跑**纪律在提交自述中延续（静态口径：宏计数 99 与 v2.20 后提交自述一致）；b) 测试与真实用户数据隔离已制度化（tests/main.cpp 重定向 + 8 个 ForTesting）；c) **测试对"环境空转"的免疫力仍不完全**：TrustChain.HmacContract 的 `secret.empty()` 早退分支保留（GuardRailsAdversarialTests.cpp:236-239）——HKCU 秘钥正常可建时应不触发，但该分支一旦走进即整例空转，建议改为 `ASSERT_FALSE(secret.empty())` 失败显性化（S）。

### 7.2 文档漂移清单（逐条对码，12 条；标 * 为本轮新增认定）

| # | 文档陈述 | 代码现实 | 判定 |
|---|---|---|---|
| 1 | ARCHITECTURE.md:10/§3 "5 个功能页" | TabId 6 页（v2.3 起） | 漂移（r4 起 4 轮未修） |
| 2 | ARCHITECTURE.md:39 "MainWindow 915 行" | 2482 | 漂移 |
| 3 | ARCHITECTURE.md ADR-010 + DESIGN-v2.md:311 "requireAdministrator 维持不变" | app.manifest asInvoker（v2.17） | **权限模型级漂移，4 轮未修** |
| 4 | main.cpp:20 注释 "both are elevated" | asInvoker 下可能一提权一非提权 | 代码注释漂移（r6 已记未修） |
| 5 | evolve-log.md | 止于 2026-10-07 Round 2；其后约 30 提交（v2.14→v2.23 + 三轮评审执行）零记录 | **流程失守 22 天** |
| 6 | DESIGN-v2-实施报告 "MainWindow 最终 474 行" | 2482（5.2 倍），无任何文档承认回弹 | 静默漂移 |
| 7 | ARCHITECTURE.md §10 history.tsv 9 列 | history.jsonl + sig 字段（v2.14） | 漂移 |
| 8 | ARCHITECTURE.md §8 "消息不带参数" | 9 种 WM_APP + taskGen wParam | 漂移 |
| 9 | * ARCHITECTURE/DESIGN-v2 全文档 | v2.21-2.23 主题系统、懒树、反选、命令面板（v2.10 起即无记载）在 docs/ 零描述；架构文档没有 ui/ 层的一节 | 漂移（结构级缺失） |
| 10 | * r6 报告 §5 "根目录无 build_log_full.txt（已删，核验通过）" | git ls-files 仍跟踪、磁盘存在（2 字节） | **r6 核验结论有误**（本轮纠正） |
| 11 | 需求.txt "C 盘瘦身" | v2.11 起全固定磁盘 | 需求侧漂移（r6 已记） |
| 12 | * DESIGN-v2-实施报告 "54/54 通过"（历史文档） | 现值 99 | 历史快照，可接受；但全仓无一处"当前测试数"活文档（README 缺失的后果） |

---

## 8. 值得保留的工程实践（增量）

1. r4/r6 清单全部存活：外置规则 + 校验 + 内置回退、DoneMessageFor 单源、零环依赖、resource.h 集中纪律、原子写（settings/history/cache 三处 temp+rename）、*ForTesting 红利、Presenter 快照契约（P1-1）、EnsureBuilt 门面、version.h 单源。
2. **代码内事故考古注释**：每处历史缺陷都留"当时发生了什么 + 为什么这么修"（Presenters.cpp:183-190、MainWindow.cpp:251-256、GuardRailsAdversarialTests.cpp:230-236…）——本仓最强的知识管理，四轮评审反复受益。
3. v2.20 修复质量：8 个 P0 修复全部带根因注释 + 用例升级（把"断言错误行为"的空转测试改成真路径断言），是"修 bug 顺便修测试契约"的范本。
4. 菜单/命令单点重派发（MainWindow.cpp:2457）。
5. 主列表虚拟化 + 勾选态 map 单源（checkStateByPath_）——v2.15 的重构让"全选/反选/勾选保持"从消息风暴变成纯数据操作，这也是 L2 可测性的来源。
6. UiHandles 纯值快照 + Presenter 不拥有控件——无意中构成了无头测试地基（§2.3）。

---

## 9. 演进路线图（ROI 排序；S=≤半天 M=1-3 天 L=≥1 周）

| # | 项 | 规模 | 动机（本轮证据） | 做法/增量 | 风险 |
|---|---|---|---|---|---|
| R1 | **修 ACCEL 计数** `CreateAcceleratorTableW(acc, _countof(acc))` | S（1 行） | §0 Top1 活回归；Ctrl+K/旧记录签名整链不可达 | 一行 + 活体点一次 Ctrl+K | 无 |
| R2 | **测试速赢三连**：SettingsRoundtrip 用例 + TrustChain 早退改 ASSERT + Presenters 进测试清单（T2） | S-M | 42.1% 零覆盖；最高行数/最纯逻辑比 | 见 §2.5 T2 | 低（null-HWND 行为需一次 spike 确认） |
| R3 | **仓库/构建卫生**（r6 R3 原样仍适用）：gitignore 补 build/.tmp-*；build_number.txt 移出跟踪；build_log_full.txt git rm；sym.py 参数化 | S | 919MB/17 目录、每构建脏树、r6 核验误导 | 纯仓库操作 | 无 |
| R4 | **/WX + Latest 拷 PDB**（r6 R4 原样） | S-M | v2.17b 自证可拦；v2.16d 靠运气符号化 | 先清存量告警→/WX（两工程对齐 W4）→PostBuild 拷 PDB 到 build\Latest\symbols\<build>\ | 存量告警量未知 |
| R5 | **asInvoker 对账 + 文档再生**（r6 R2）：一页决策（T-B5 顺带）→ 修 ARCHITECTURE/DESIGN-v2/main.cpp 注释/rotate.ps1 → evolve-log 复活为提交门 | S+M | 权限模型漂移 4 轮；路线图连续两轮 0 执行的根因是"记账不清账" | 决策先行，机械修改随后 | 低；T-B5 需用户拍板 |
| R6 | **纯函数抽离 L1+L3**（ui/ListText + ui/SummaryText） | M | 测试红利/行最低成本；MainWindow 减重 | 每 commit 一个抽离簇 + 用例 | 低 |
| R7 | **列/消息/快捷键常量化 + 双射用例**（T7；含 ACCEL、RootsDlg id、CommandPalette id 入 resource.h） | S | 魔数两次咬人 | constexpr + 1 个双射测试文件 | 低 |
| R8 | **Theme 调色板结构** | M | 13 处内联 RGB；深色/高对比度维护成本 | struct Theme + LightTheme()，绘图函数改读 | 低 |
| R9 | **静态库拆分**（r6 R7：util→platform→core；UiMessages.h 消 core→res） | L | 手抄清单 + 零覆盖的根因；违规边第四轮存活 | 三步各一提交；tests 换库引用 | 链接错误潮=显形隐藏依赖 |
| R10 | **MainWindow 四拆 + Tab 描述符**（r6 R8 升级版）：①绘图簇 → MainWindowChrome.cpp（~330 行）②命令/面板/快速筛选/RootsDlg → MainWindowCommands.cpp（~390）③设置/空闲/校验 → MainWindowLifecycle.cpp（~200）④执行流 → MainWindowExecute.cpp（~350）；WndProc 拆 OnNotify/OnCommand/OnTimer/OnAppMsg 四个成员函数后剩 ~60 行路由；tabInfoText 六块 switch → const 数组 | L | 2482 行/37 方法/CC168 | 每拆一块跑 99×2 + 截图；目标先 <1,400 行 | 中：路由漏分支=静默失效（R7 双射先行） |
| R11 | **UI 宿主测试 L5**（无头真窗口 fixture + ConfirmTask 注入 + 截图回归） | M-L | "包括 UI"的覆盖目标 | §2.3 L5 | 中：comctl6 清单/模态泵时序 |

**先做什么最快提升可测试性与稳定性：R1→R2（半天到两天，消一个活回归 + 1,100 行从零到有）。若只做一件：R1。若做三件：+R3/R4（卫生 + 防回归基建，全部 S）。**

---

## 10. 被推翻的假设 / 已验证无问题

**被推翻 / 修正：**
1. *"v2.22 只是 UI 提交，无风险"*（r6 综合报告"近 5 提交审计干净"止于 v2.19a 之后 d8ee0e4；v2.22 未被任何评审覆盖）——**推翻**：d574af3 引入 ACCEL 计数回归（§0 Top1），静默移除 Ctrl+K。
2. r6 §5 "build_log_full.txt 已删，核验通过"——**不实**：git ls-files 仍跟踪、磁盘存在（2 字节空文件）。r6 该条核验结论错误（可能只查了内容非空与否）。
3. 线索块"v2.23 light theme 后调色板是否真可切换"（假设：有主题机制）——**部分证伪**：无任何 Theme 结构，仅 2 常量 + 13 内联 RGB；"主题"目前是硬编码浅色，不可切换（§4f）。
4. 线索块"MainWindow 2482 行"——证实（wc 实测），且 WndProc 约 570 行/CC 168 较 r6 继续恶化（+84 行/+17 CC）。
5. r6 摘要"asViewer 后机密不可读（05 已部分修正为 HMAC flag 才是根因）"——复核成立：SecretStore.cpp:81 HMAC flag 已修 + HKCU 机密路径在（v2.20），签名链测试从空转改真路径（GuardRailsAdversarialTests.cpp:229-258 注释自证）。

**已验证无问题（抽检记录）：**
- include 图 0 环（Tarjan 80 节点复验）；r4/r6 结论存活。
- r6 交付的 8 个 P0 修复全部真实在码（§1.5 表，逐条 file:line 亲读）。
- 手抄清单 25/25 与磁盘一致，当前无漂移。
- rules.json 27 条 = 15 safe + 7 cautious + 3 advanced + 2 info，与实施报告一致。
- 原子写纪律三处在位（settings.json temp+rename Settings.cpp:60-70；history 与 cache 同模式）。
- 单实例 + takeover 结构完整（main.cpp:16-61），WM_QUIT 转发在两个手写模态泵里均已处理（RootsDlgProc 后置 PostQuitMessage :1474；palette quitEaten :168-186）。
- v2.16d SupplyDispInfo 越界守卫双实现（List :190-199 / History :975-987）在位。

---

## 11. 未验证项与局限

1. **全静态，未构建/未跑 99 测试/未启动 GUI**；"99 通过"为宏计数静态口径；r6 交付双跑声明沿用其自述。
2. **ACCEL 回归未活体复现**：证据为代码 + `git show d574af3` diff（插入第 3 项未改计数 3）+ 全仓唯一触发路径排查（IDC_ACCEL_PALETTE 仅 WM_COMMAND 一处、消息循环手写键处理无 'K'）；按约束未启动 exe 验证。
3. **Presenter 无头直测的 null-HWND 安全性是静态推断**（SendMessageW(nullptr) 返回 0 的 Win32 语义 + 宏展开读码），未写 spike 编译运行——T2 第一步应是 10 分钟 spike（列入路线图前置）。
4. 函数度量为启发式（括号配对 + 关键词），与 r6 脚本口径不同，只用于趋势对比；CC 对 switch/TaskDialog 抬高。
5. build/ 仅清点目录名与体量（du/git 级），未读产物；919MB/17 目录含 17 个子目录实测。
6. 工作集 ~905MB、冷启动 4.1s、Finalize 3.72s 等 r6 性能数字未复测，仅引用；B-4 未做的判定依据是代码结构（VolumeIndex.cpp:689-694 缓存载入后同步 Finalize 仍在），非运行时测量。
7. googletest 具体版本无法从 vendored 树验证（无版本标记文件），"1.15.2"采信实施报告。
8. FolderTree v2.23 懒树对"双倍全盘遍历"（r6 P2）的缓解效果未量化：FolderTreeScanner 仍 depth=2 默认全盘定大小，深层改懒加载——首轮 I/O 是否减半需实测。
9. T-B5（Program Files 迁移被拦）产品语义未做真机验证，仅确认 GuardRails.cpp:79-80 保护表现状与 r6 一致。
10. 未评估 MiniSysTests 链接 gtest 的 Debug/Release 运行时选择细节（vcxproj RuntimeLibrary 未逐配置比对）。
