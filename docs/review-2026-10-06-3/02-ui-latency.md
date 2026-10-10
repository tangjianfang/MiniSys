# 02 · UI 线程延迟补审(完整延迟预算表 + watchdog 方案)

> 日期:2026-10-11 · 对象:HEAD `76f6514`(v2.13c, build 35)
> 上轮(docs/REVIEW_2026-10-06-3.md §5)02 号因限额中断,遗留三件事:完整延迟预算表、
> UI stall watchdog、"1000 行插入 0.8-1.6s"的复跑与虚拟列表评估。本报告即该三件事。
> 方法:重建微基准(探针源码失传,重写于 `%TEMP%\minisys-review-20261011\02-exp\lvbench.cpp`)
> + 复用仓库 `util/Json.cpp` 实测缓存反序列化 + 全量读码。**未启动 MiniSys.exe**(约束),
> 应用级数字 = 实测组件之和(标注"推算"处为线性外推,微基准已验证 1000→5000 严格线性)。
> 仓库 / 用户数据零写入;探针只写 %TEMP%。

---

## 0. 摘要

### Top 卡顿源排序(按对用户可感知延迟的贡献)

| # | 卡顿源 | 位置 | 实测/推算 |
|---|---|---|---|
| 1 | **RenderItems 全删全插**:ListView_InsertItem 每行 ~1.26ms(含 LVS_EX_CHECKBOXES 内部开销),6 列 SetItemText 仅 ~6µs/行 | Presenters.cpp:140-200 | **1000 行 1.26-1.36s;5000 行 6.3-6.8s(实测)** |
| 2 | **键入搜索端到端** = 200ms 防抖 + worker ~100-150ms + UI 端全量渲染 1.26s@1000 | MainWindow.cpp:338/468-476 | **~1.6s@1000(组件实测求和)** |
| 3 | **排序点击** = ToLower 比较器排序 13-82ms + 全量重渲染 | Presenters.cpp:406-463 | **1.3s@1000 / 6.4s@5000** |
| 4 | **启动首屏缓存路径**:WM_CREATE 同步 ×6 JSON 反序列化(上限 5×5000 项 ≈ 1.1s)+ 活动页全量渲染 | MainWindow.cpp:595-598, SessionService.cpp:233-300 | 今日数据 11.6ms(**实测**);最坏 ~1.1s + 渲染(**推算**) |
| 5 | **批量勾选(Ctrl+A/全不选)** 每行一次 ListView_SetCheckState ≈ 0.13ms/行 | MainWindow.cpp:400-415/1836-1845 | **134ms@1000 / ~670ms@5000(实测模式)** |
| 6 | 勾选单行 → UpdateExecButton → CollectChecked 全表扫描 | MainWindow.cpp:229-242/1453-1486 | 4-8ms@1000(实测);**~20-40ms@5000(推算)** |
| 7 | CaptureCheckState(GetItem+ToLower+map 逐行) | Presenters.cpp:68-78 | 6-7ms@1000 / 37-53ms@5000(实测) |
| 8 | -tour 巡页:每 4s 一次全量切页(含渲染,大页单次即 >4s)+ 每 tick 写 settings.json | MainWindow.cpp:506-511/1172 | 仅评审模式 |

### 预算达标判定(16ms 帧 / 100ms 响应)

- **不达标(结构性)**:任何 ≥100 行列表的渲染路径 —— 切页、搜索落屏、排序、校验应用。
  100 行即 ~126ms,超出 100ms 响应预算;唯一治本方案是虚拟列表(实测等价渲染 60-90ms 且与 N 无关)。
- **达标**:右键菜单、说明面板、空闲重扫启动、关闭(常规)、状态栏每 tick 开销、
  Results(tab) 持锁拷贝、单行勾选(@1000)。
- 上轮 0.8-1.6s 结论**复现成立**(1.26-1.36s,两次独立运行)。

### 新增非延迟发现(审计中顺带,代码级 Confirmed)

U-8:切页时**外出页签的勾选状态丢失/串染**(共享 ListView × 错时机 CaptureCheckState)。

---

## 1. 方法

| 数据 | 方式 |
|---|---|
| ListView 插入/删除/虚拟模式/各种 UI 线程模式 | 自建探针 `lvbench.exe`(最小宿主窗口 + 与 Controls.cpp:21-43 完全一致的 ListView 样式/列宽/6 列文本,逐行调用序列照抄 Presenters.cpp:172-198)。每相位 5 次取 min/med/max;两轮独立运行(机器 96 DPI,同机)。 |
| TryLoadCachedResults 成本 | `jsonbench.exe` 链接仓库原版 `util/Json.cpp`+`StringUtils.cpp`,只读解析 `%LOCALAPPDATA%\MiniSys\cache\results-*.json` 真实文件 ×20 次,另造 5000 项合成文档(= kMaxCachedItems 上限)。 |
| 应用级各路径 | 读码定位调用链(MainWindow.cpp 1945 行 / Presenters.cpp 887 行 / SessionService.cpp 1055 行全文精读),按上述实测组件求和;线性外推已由 1000/5000 两点验证(1.258ms/行 vs 1.259ms/行)。 |
| 未做 | 活体运行(约束禁动进程);DPI≠96 场景;滚动中的增量重绘帧成本(虚拟模式 repaint 已测全量失效路径)。 |

---

## 2. 发现清单(U-1…,本轮新编号,与 REVIEW_2026-10-06-UI §4 旧 U-1..5 无关)

### U-1(CONFIRMED,实测)渲染全删全插是全部超标路径的共同根因

- **触发/频率**:每次切页(OnTabChanged→Refresh, MainWindow.cpp:1157)、搜索落屏
  (WM_APP_SEARCH_DONE→Refresh, MainWindow.cpp:468-476)、排序(ApplySortAndRefresh)、
  校验应用(ApplyDeadPaths render=true, MainWindow.cpp:747-767)、扫描完成(OnScanDone)。
- **成本(实测)**:1000 行 1.26-1.36s;5000 行 6.3-6.8s;严格线性 ~1.26ms/行。
  归因:ListView_InsertItem 占 ~89%(G1: 仅 InsertItem ×1000 = 1120ms);
  SetCheckState ×1000 = 134ms;5×SetItemText 合计仅 ~6ms/千行。
  去掉 LVS_EX_CHECKBOXES 样式:1000 行 0.79-0.88s(该扩展样式使插入贵 ~1.4-1.5 倍);
  WM_SETREDRAW 包裹:0.69-0.75s(仅 1.8 倍收益,治标)。
- **修复草案**:LVS_OWNERDATA 虚拟化(见 §6),等价渲染实测 60-90ms 且与行数无关。
- **收益**:所有超标路径一次性进入预算(切页/搜索落屏/排序/校验应用 → 全部 <100ms)。

### U-2(Confirmed·代码+实测组件, Likely·最坏场景)启动首屏同步 ×6 JSON 反序列化

- **触发**:每次启动,WM_CREATE 内同步执行 MainWindow.cpp:595-597
  `for (0..TabId::Count) svc.TryLoadCachedResults(tab)`,随后 598 行重渲染活动页。
  全部发生在窗口 ShowWindow 之前 —— 用户看到的是"程序迟迟不出现"。
- **成本(实测)**:今日真实缓存 4 文件合计 **11.6ms**(LargeFiles 240 项 9.9ms 为主);
  合成 5000 项 = **222ms/文件**(parse 203 + ScanItem 化 15 + Utf8ToWide 3.5,约 44µs/项)。
  设计上限 5 文件 × 5000 项(kMaxCachedItems, SessionService.cpp:112)≈ **1.11s**;
  叠加活动页渲染(U-1)即最坏 ~7s 首屏(推算)。
- **修复草案**:①活动页缓存保留同步(首屏要显示),其余 5 个 tab 改为**首次激活时懒加载**
  (OnTabChanged 里 try-load-once);或 ②全部投 worker,完成后 PostMessage 重渲染活动页。
  收益:首屏 JSON 从最坏 1.11s → ~44ms(仅活动 tab)。

### U-3(Likely)关闭可能挂在工作线程收尾上

- WM_CLOSE→SaveSettings(写 settings.json,~ms)→DestroyWindow→PostQuitMessage 后,
  RunMessageLoop 尾部 `SessionService::Shutdown()`(MainWindow.cpp:176 → SessionService.cpp:1050-1053)
  join worker。窗口已销毁但进程存活,表现"关了还在任务管理器里"。
- 最坏情形:worker 正在 SaveCacheIfWorthwhile(VolumeIndex.cpp:1209)—— 本机 index-cache-C.bin
  **262 MB**,fwrite 尾巴 join 等待 **1-3s(Likely,NVMe 估算)**;正在空闲重扫(WalkBuild)时
  取消点在每目录弹出(VolumeIndex.cpp:540-547),<50ms 即响应,无碍。
- **修复草案**:Shutdown 前 SetProgress+日志;或把 index-cache 写盘移到独立低优先级线程/
  分片写;至少 WM_DESTROY 里给用户可见性(托盘气泡不必要,写日志即可定位)。

### U-4(CONFIRMED,实测模式)批量勾选每行一条消息,千行百毫秒级

- IDM_LIST_SELECTALL/SELECTNONE(MainWindow.cpp:400-415)与 OnSelectAll(1836-1845):
  逐行 `ListView_SetCheckState`。实测该宏模式 **134ms@1000**(G3),推算 ~670ms@5000。
  R-2 修掉了 O(N²) 的 recount 风暴(batchUpdate_ 闸,MainWindow.cpp:229-242 已核对在位),
  剩余 O(N) 消息发送本身仍超 16ms 帧预算。
- **修复草案**:虚拟化后勾选态即 checkStateByPath_ map 写入,零 ListView 消息;
  未虚拟化前可先只在批量路径写 map + 尾部一次性同步(收益有限,不单独立项)。

### U-5(CONFIRMED,实测)排序比较器每次比较 ToLower 两次堆分配

- Presenters.cpp:417-456 六个 stable_sort 分支中,case 0/2 每次比较构造 2-4 个临时
  wstring(ToLower(a.title) 等),实测 **13ms@1000 / 82ms@5000**;
  预计算小写键 + 索引排序后 **0.8ms / 4.6ms(16-18 倍)**(J/J2 相位)。
- **修复草案**:排序键缓存进 snapshot_(扫描/加载时生成一次 lowerTitle/lowerCategory),
  ApplySortAndRefresh 改排 `std::vector<uint32_t>` 索引。S 规模;虚拟化前即可做,
  且虚拟化后排序本来就要改索引序,此工作是子集。

### U-6(Confirmed@1000 实测 / Likely@5000)单行勾选触发全表 CollectChecked

- 每次 check 翻转 → UpdateExecButton(MainWindow.cpp:229-242→1453-1486)→
  CollectChecked(Presenters.cpp:340-351)逐行 GetCheckState+GetItem:
  实测 **4-8ms@1000**(G4),线性推算 ~20-40ms@5000 → 超 16ms 帧预算。
- **修复草案**:LVN_ITEMCHANGED(checkFlipped)时增量维护 checkedCount/checkedBytes
  (旧态从 checkStateByPath_ 查),UpdateExecButton 变 O(1)。S 规模。

### U-7(Likely,代码读证)-tour 巡页模式的自干扰

- TIMER_TOUR 每 4s(MainWindow.cpp:506-511)切页 → OnTabChanged 完整路径:ShowWindow
  批量 + Refresh(**含 U-1 渲染,大页单次 1.3-6.8s > 4s 间隔,UI 持续饱和**)+
  尾部 SaveSettings **每 tick 写一次 settings.json**(1172 行)。切到 Search 且无有效索引时
  还会触发 BuildIndexAsync(MainWindow.cpp:1157)。评审专用,不进发布预算表,但建议:
  tour tick 前 `if (taskMode_ != None || 上次渲染未完成) return;` + tour 模式下跳过 SaveSettings。
- 命令面板(CommandPalette.cpp)开销实测级评估:17 条命令、Refill O(17×len) ListBox 重建
  <1ms,模态循环无重活 —— **无问题,不必优化**。

### U-8(CONFIRMED,代码追踪;活体未验)切页丢失/串染外出页勾选状态

- 机制:单一共享 ListView。TCN_SELCHANGE→OnTabChanged(MainWindow.cpp:226-227)只调
  **进入页** presenter 的 Refresh;其首步 CaptureCheckState(Presenters.cpp:68-78)此刻读到的是
  **外出页的行**,lParam 却按下自己的 snapshot_ 解释:
  ① 用户在外出页做的取消勾选只存在于 ListView,从未入 map(勾选翻转仅 UpdateExecButton,
  MainWindow.cpp:229-242)→ 切回时按旧 map 重渲染,**取消勾选被静默还原**(与 R-5/L-1 同类,
  R-5 修的是搜索刷新路径,切页路径漏网);
  ② 若外出页行号落入本页 snapshot 范围,会把错误路径写入本页 checkStateByPath_
  (低危:仅污染本页渲染初值)。
- **修复草案**:TCN_SELCHANGE 里切页**之前**对 ActivePresenter()(旧 tab)调一次公开的
  CaptureState();S 规模。虚拟化后该机制整体消失(map 即唯一真源)。

### 已验证无问题 / 被推翻假设(单列)

| 假设(任务书点名复查项) | 结论 | 证据 |
|---|---|---|
| Results(tab) 持锁拷贝拖慢 UI / 与 worker 争锁 | **无问题** | 实测拷贝 0.4ms@1000、4ms@5000(K 相位);worker 侧持 resultsMu_ 做全量拷贝仅 SaveResultsCache(SessionService.cpp:178-181,~0.5ms),争锁窗口微秒级 |
| UpdateStatusBar 每 tick SB_SETPARTS+GetDiskFreeSpaceExW 频率过高 | **无问题** | 消息组 ~1.5µs/次(L);GetDiskFreeSpaceExW 35-47µs/次(L2);即使扫描进度每目录一贴(JunkScanner.cpp:298)也是亚毫秒级。旧 U-5(history.jsonl 每 tick 解析)已由 Generation 缓存修复,复核在位(SessionService.cpp:847-877) |
| RenderItems 的 LocalizeCategory map 查找 / OneLine 拷贝 | **无问题** | std::map wstring 查找 ~0.2-0.5µs/行;OneLine 已 reserve 单遍。被 U-1 的 InsertItem 成本淹没 3 个数量级 |
| CaptureCheckState/CaptureViewState O(N) 字符串拷贝是主卡点 | **部分成立但次级** | 前者实测 6-7ms@1000 / 37-53ms@5000(H 相位);后者 + RestoreViewState N×K 匹配仅 0.2/1.4ms(I 相位,**被推翻**)。仅虚拟化后随机制一并删除 |
| 插入慢是"绘制/重绘"造成 | **被推翻** | 列表隐藏(SW_HIDE)插入仍 613-634ms(N2);去掉 DOUBLEBUFFER 反而 1195-1209ms(N3);去 LABELTIP/INFOTIP 无变化(N4);1 列插入 1171ms(N5)。成本在 comctl32 逐条 InsertItem 内部维护(含 checkbox 状态位),不在 paint 也不在列数 |
| DeleteAllItems 昂贵 | **被推翻** | 2.5-3.2ms@1000、8-11ms@5000(B1) |
| LVM_SETITEMCOUNT 预分配提示能救急 | **被推翻** | N1(SETITEMCOUNT 提示+SETREDRAW)723-817ms,与纯 SETREDRAW 无显著差 |

---

## 3. 完整延迟预算表

预算口径:帧内路径(每事件)16ms;命令型响应(含一次性工作)100ms。
"现状"= 实测组件求和(§标注);@N = 列表 N 行。搜索结果上限 1000(kMaxResults),
大文件/垃圾行数现机 240/1(缓存实测),5000 为压力上界。

| # | 场景 | 调用链(责任代码) | 现状 @1000 / @5000 | 目标 | 判定 | 差距主因 |
|---|---|---|---|---|---|---|
| 1 | 启动首屏(进程→窗口可见) | WinMain→Create→OnCreate:CreateControls+icons(~20-50ms 推算)+LoadSettings(~0.1ms)+OnTabChanged(空渲染)+**TryLoadCachedResults×5-6(实测今日 11.6ms;上限 1.11s)**+活动页 Refresh 渲染(MainWindow.cpp:556-619) | 今日(lastTab=Junk,1 项):**~80-150ms(推算)**;最坏 lastTab=搜索(1000 项):~1.4s | ≤500ms | 今日达标 / 最坏不达标 | U-2 + U-1 |
| 2 | 切页 | OnTabChanged:ShowWindow 批量(<1ms)+Refresh(Capture 6-7ms + Results 拷 0.4ms + **渲染 1.26ms×N**)+UpdateStatusBar×2+SaveSettings 写盘(1-5ms)+OnSize→LayoutWindow(含 DT_CALCRECT 2048 字符,~0.1-0.5ms 推算)(MainWindow.cpp:1068-1175) | **~1.29s / ~6.4s** | ≤100ms | ✗ | U-1 |
| 3 | 键入搜索(击键→列表更新) | EN_CHANGE→SetTimer 200ms(MainWindow.cpp:338)→RunSearch→SearchAsync worker(76ms/百万条搜索 + 150×GetFileAttributesExW 大小补齐 + StoreResults + 1.5s 节流存盘,SessionService.cpp:491-566)→WM_APP_SEARCH_DONE→Refresh 全量渲染(MainWindow.cpp:468-476) | **200+~150+1270 ≈ 1.6s / (5000 不适用,搜索截断 1000)** | ≤300ms | ✗ | 防抖 200 + U-1(渲染占 80%) |
| 4 | 勾选一行 | LVN_ITEMCHANGED→UpdateExecButton→CollectChecked(MainWindow.cpp:229-242, 1453-1486) | **4-8ms / ~20-40ms(推算)** | ≤16ms | ✓@1000 / ✗@5000 | U-6 |
| 5 | 排序点击(列头/按钮) | ApplySortAndRefresh:Capture×2(6-7ms)+stable_sort(13/82ms)+**渲染**(Presenters.cpp:406-463) | **~1.28s / ~6.4s** | ≤100ms | ✗ | U-1(排序键 U-5 次之) |
| 6 | 右键菜单弹出 | NM_RCLICK→CreatePopupMenu+8×AppendMenu+TrackPopupMenu(MainWindow.cpp:1904 OnListContextMenu) | **<5ms(推算,菜单式模态)** | ≤100ms | ✓ | — |
| 7 | 打开说明面板 | ItemAtRow+TaskDialogIndirect(Presenters.cpp:659-714) | **<5ms 弹出(内容静态)** | ≤100ms | ✓ | — |
| 8 | 校验完成应用 | WM_APP_VERIFY_DONE→ApplyDeadPaths:LastDeadPaths 拷 + Capture×2 + O(N×D) std::find + **渲染** + StoreResults(Presenters.cpp:357-382) | 无死项 ~7ms;有死项 **~1.3s / ~6.4s** | ≤100ms | ✗(有死项时) | U-1 |
| 9 | 空闲重扫开始 | TIMER_IDLE_REFRESH 30s→OnIdleCheck(GetLastInputInfo+LastScanAt,<0.1ms);倒计时 1s tick 仅 ShowHint;OnScan→StartScan 投 worker(<1ms)(MainWindow.cpp:1020-1060) | **<1ms** | ≤16ms | ✓ | — |
| 10 | 关闭 | WM_CLOSE→SaveSettings(写盘)→DestroyWindow→PostQuit→Shutdown→join worker(MainWindow.cpp:545-548, 176) | 常规 **<10ms**;worker 正写 262MB 索引缓存时 **1-3s(Likely 推算)** | ≤200ms | ✓/△ 最坏 | U-3 |
| 11 | 批量勾选(Ctrl+A/右键全选,补充行) | 逐行 SetCheckState + 尾部 recount(MainWindow.cpp:400-415, 1836-1845) | **~140ms / ~690ms(实测模式 G3+G4)** | ≤100ms | ✗@5000 | U-4 |
| 12 | -tour 巡页 tick(评审模式,不计入产品预算) | 每 4s 全量切页 + SaveSettings 写盘(MainWindow.cpp:506-511, 1172) | 每 tick = 场景 2 成本;大页 >4s 自过载 | — | △ | U-7 |

---

## 4. 微基准数据表

环境:同机 Windows 11,96 DPI,宿主窗口 1100×720 逻辑(与 MiniSys 默认一致),
ListView 样式/列宽照抄 Controls.cpp + Presenters.cpp(6 列:分类/风险/项目/大小/详情/修改时间),
行文本含 CJK 与 emoji 徽标,详情列 60-140 字符路径。每相位 5 次运行,两轮独立执行
(表中 med1/med2);探针 `02-exp\lvbench.cpp`(源码 + lvbench.txt 原始输出留档)。

| 相位 | 内容 | @1000 med1/med2 (ms) | @5000 med1/med2 (ms) |
|---|---|---|---|
| A | 插入+5×SetItemText+SetCheck(=RenderItems 内循环) | **1364 / 1258** | **6771 / 6295** |
| B1 | DeleteAllItems 单独 | 2.7 / 3.2 | 8.8 / 11.2 |
| B2 | 全周期 delete+重插(=一次 Refresh) | 1322 / 1294 | 6510 / 8165* |
| C | A + 强制同步绘制 | 1426 / 1372 | 6556 / 6731 |
| D | A 包 WM_SETREDRAW=FALSE | 685 / 750 | 3324 / 3322 |
| E | A 去掉逐行 SetCheckState | 1149 / 1171 | 5879 / 6057 |
| F | 去掉 LVS_EX_CHECKBOXES 样式整体 | 790 / 876 | 3953 / — |
| G1 | 仅 InsertItem ×1000(带 checkbox 样式) | 1120(单次) | — |
| G2 | 仅 SetItemText(详情列)×1000 | 5.8 | — |
| G3 | 仅 SetCheckState ×1000 | 134.5 | —(线性推算 ~670) |
| G4 | CollectChecked 模式 ×1000(500 勾选) | 3.9 / 8.1 | —(推算 20-40) |
| H | CaptureCheckState 模式(GetItem+ToLower+map) | 6.2 / 7.4 | 36.9 / 53.1 |
| I | RestoreViewState N 行×50 选中路径匹配 | 0.20 / 0.28 | 1.4 / 1.4 |
| J | stable_sort 标题列(ToLower/比较) | 12.7 / 13.1 | 71.5 / 81.8 |
| J2 | 同排序,预计算小写键(修复候选) | 0.57 / 0.81 | 3.4 / 4.6 |
| K | vector<ScanItem> 全量拷贝(=Results(tab) 持锁) | 0.60 / 0.43 | 3.7 / 4.1 |
| L | UpdateStatusBar 消息组(SB_SETPARTS+4×SETTEXT+GETRECT) | 1.3 µs/次 | — |
| L2 | GetDiskFreeSpaceExW(C:) 单次 | 47 µs / 35 µs | — |
| M | 虚拟模式 SETITEMCOUNT+整窗重绘(dispinfo 256 次) | 69 / — | 83 / —(**@100000: 77ms,与 N 无关**) |
| M2 | 虚拟模式跳至 4999 行+重绘 | — | 66 |
| N1 | SETITEMCOUNT 预分配提示+SETREDRAW | — / 817 | — |
| N2 | 列表隐藏(SW_HIDE)下插入 | — / 634 | — |
| N3 | 去 LVS_EX_DOUBLEBUFFER | — / 1209 | — |
| N4 | 去 LVS_EX_LABELTIP/INFOTIP | — / 1334 | — |
| N5 | 仅 1 列(分类+勾选)插入 | — / 1171 | — |

\* B2@5000 第二轮 med 受并发负载干扰(max 11.5s),以 min 6.07-6.30s 与 A 一致性为准。
线性度:A 相位两轮 1.364/1.258 ms/行(1000)vs 1.354/1.259 ms/行(5000)—— 严格线性,外推可信。

JSON 反序列化(jsonbench.exe,仓库原版 Json.cpp,真实缓存文件 ×20 次):

| 文件 | 大小/项数 | Utf8ToWide | Json::Parse | 项→ScanItem | 合计 |
|---|---|---|---|---|---|
| results-0(Junk) | 275 B / 1 | 0.001 | 0.027 | 0.003 | **0.031 ms** |
| results-1(Search) | (当前无文件) | — | — | — | 估 44 ms@1000 项(按 44µs/项) |
| results-2(LargeFiles) | 109,119 B / 240 | 0.184 | 9.182 | 0.548 | **9.9 ms** |
| results-3(Apps) | 21,532 B / 45 | 0.037 | 1.001 | 0.058 | **1.1 ms** |
| results-4(FolderTree) | 7,697 B / 27 | 0.007 | 0.538 | 0.034 | **0.58 ms** |
| 合成 5000 项(上限) | 2.53 MB | 3.5 | 203.1 | 15.3 | **221.9 ms** |

---

## 5. UI stall watchdog 设计(Win32 落地)

目标:① 每条 >50ms 的消息处理留痕(消息号+耗时+阶段标签);② 独立线程心跳检测真冻结
(UI 线程 >500ms 不泵消息)并抓栈;③ 开销 <0.1% CPU、零用户数据上送。

### 5.1 注入点

全部落在 MainWindow.cpp:140 RunMessageLoop 与 WndProc 外壳,core 不动:

1. **泵级计时(主注入点)**:RunMessageLoop 里 GetMessageW→DispatchMessageW 前后各一次
   QueryPerformanceCounter(~20ns),超阈值记录 {msg, wParam, hwnd, ms, tick}。
   覆盖一切派发路径(含模态对话框外的所有窗口过程);模态内部(TaskDialog/MessageBox/
   TrackPopupMenu/SHBrowseForFolder)有自己的泵,由心跳层兜底。
2. **阶段标签(RAII)**:`struct PhaseTag { const char* name; }` 在 OnTabChanged/Refresh/
   RenderItems/ApplySortAndRefresh/OnScanDone/OnVerifyDone/OnPlanDone 入口压栈,
   记录条目带当前栈顶标签 —— 把 "WM_APP_SEARCH_DONE 1.3s" 直接归因到 "Refresh/RenderItems"。
3. **心跳线程**:std::thread 周期 PostMessageW(hwnd, WM_APP_WATCHDOG_BEAT(自定义
   RegisterWindowMessageM), gen, 0) 并检查 UI 线程是否已回执上一拍;UI 侧在 WndProc
   首行对 BEAT 消息 `beatGen_.store(wp)`(一条 atomic store,纳秒级)。
4. **冻结抓栈**:心跳发现连续 2 拍(默认 2×250ms)未回执 → SuspendThread(UI 线程)+
   CaptureStackBackTrace(或 StackWalk64,DbgHelp 已在工程可用;注意 dbghelp 线程安全,
   抓栈专用一个初始化过的 HANDLE,或直接复用 platform/CrashDump.cpp 的机制写
   MiniDumpWriteDump 到 %LOCALAPPDATA%\MiniSys\logs\stall-<ts>.dmp,文件头附最近 64 条
   消息日志环形缓冲快照)→ ResumeThread。**只对 >500ms 冻结才动线程**,且同一进程
   限 10 次/小时,避免 SuspendThread 本身引入二次问题。
5. **环形缓冲**:SPSC 单写(UI 线程)单读(抓栈时同线程读)→ 免锁 std::array<Entry,1024>。

### 5.2 开销

- 泵计时:每消息 2×QPC ≈ 40-60ns,10⁴ 消息/秒 ≈ 0.05% 单核。
- 心跳:4 msg/s + 1 atomic;空闲时零分配。
- 抓栈:仅事故态,单次 <50ms(在 watchdog 线程,不影响 UI)。

### 5.3 隐私与产物

- 落盘仅 %LOCALAPPDATA%\MiniSys\logs\stalls.log(文本:时间/消息号/耗时/阶段标签/
  模块名+偏移,**不解析符号、不含路径内容**)+ 可选 minidump(含内存,默认关闭,
  settings.json `watchdogDump:false` 默认)。轮转 1MB×2(仿 Logger 5MB 轮转)。
  进程为管理员,dump 文件 ACL 继承用户目录,无上送。

### 5.4 伪代码

```cpp
// ---- watchdog.h ----
class UiWatchdog {
public:
    static UiWatchdog& Instance();
    void Attach(HWND uiThreadOwner);      // WM_CREATE 末尾调用
    void Detach();                        // WM_DESTROY
    void OnBeat(WPARAM gen);              // WndProc 首行: if(msg==kBeat) OnBeat(wp)
    void PumpStart(MSG&); void PumpEnd(); // RunMessageLoop 包裹
    void PushPhase(const char*);          // RAII PhaseTag
private:
    struct Entry { UINT msg; WPARAM wp; double ms; const char* phase; ULONGLONG tick; };
    std::array<Entry, 1024> ring_;  size_t ringHead_ = 0;      // 仅 UI 线程写
    std::atomic<ULONG64> beatAck_{0};                          // UI 写,watchdog 读
    std::atomic<bool>   frozenReported_{false};
    std::thread th_;  HANDLE uiThread_;  HWND owner_;
    static constexpr double kMsgMs = 50.0;                     // 消息阈值
    static constexpr DWORD kBeatMs = 250, kMissLimit = 2;
    void Loop();                                               // watchdog 线程体
    void CaptureStackFrozen();                                 // 事故态
};

// ---- RunMessageLoop 改造(MainWindow.cpp:140)----
while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    UiWatchdog::Instance().PumpStart(msg);
    ... 原 TranslateAccelerator/Tab/Enter/Esc/Translate/Dispatch ...
    UiWatchdog::Instance().PumpEnd();
}

// ---- PumpStart/PumpEnd ----
thread_local MSG s_cur;  thread_local LARGE_INTEGER s_t0;
PumpStart(MSG& m){ s_cur=m; QueryPerformanceCounter(&s_t0); }
PumpEnd(){
    LARGE_INTEGER t1; QueryPerformanceCounter(&t1);
    double ms = ElapsedMs(s_t0, t1);
    if (ms >= kMsgMs) Append({s_cur.message, s_cur.wParam, ms, CurrentPhase(), GetTickCount64()});
    if (ms >= kMsgMs) FlushIfDue();          // 批量写,普通 ofstream append
}

// ---- WndProc 首行 ----
if (msg == kBeatMsg) { UiWatchdog::Instance().OnBeat(wp); return 0; }

// ---- 心跳线程 ----
void UiWatchdog::Loop(){
    ULONG64 gen = 0;
    while (!stop_) {
        ++gen;
        PostMessageW(owner_, kBeatMsg, gen, 0);
        Sleep(kBeatMs);
        if (beatAck_.load() < gen) {                          // 本拍未回执
            if (++misses >= kMissLimit && !frozenReported_.exchange(true)) {
                CaptureStackFrozen();                          // Suspend+BackTrace
                frozenReported_ = false;                       // 由恢复路径或超时复位
            }
        } else misses = 0;
    }
}
void UiWatchdog::OnBeat(WPARAM gen){ beatAck_.store(gen); }
```

验收:向 RenderItems 临时 Sleep(300) 应产出一条 {WM_APP_SEARCH_DONE, 300ms, phase=Refresh}
日志;卡 600ms 应产出 dump + 最近消息环形快照。

---

## 6. 虚拟列表(LVS_OWNERDATA)评估

### 6.1 实测收益(M 相位)

| 指标 | 现状(实插) | 虚拟模式 | 倍率 |
|---|---|---|---|
| 渲染 N 行(数据→可见) | 1258ms@1000 / 6295ms@5000 | **SETITEMCOUNT+整窗重绘 69-83ms,100000 行同价**(dispinfo 仅 256 次=可见行×列) | **18×@1000、76×@5000、∞@10万** |
| 清空 | 2.5-11ms | O(1)(count=0) | — |
| 排序后呈现 | 重插全量 | 重排索引+Invalidate(排序本身 U-5 修复后 0.8-4.6ms) | — |
| Capture/RestoreViewState | 6-53ms/次 | **机制删除**(状态常驻 presenter) | — |
| 勾选读写 | 0.13ms/行消息 | map 读写 O(log N),零消息 | — |

与 16ms 预算的关系:60-90ms 是**整窗强制重绘**口径(含首帧字体/双缓冲合成);
稳态增量失效(滚动/单行变化)远低于此。即使保守取 90ms,也满足 100ms 响应预算;
帧预算(16ms)内的增量更新由控件自身滚动机制保证(现状亦如此)。

### 6.2 改造内容(估 1.5-2.5 人日 + 回归)

1. Controls.cpp:创建时加 LVS_OWNERDATA(或首次 Refresh 时切换样式)。
2. Presenters:`std::vector<uint32_t> order_`(排序后行→snapshot 索引);
   RenderItems 收缩为 `ListView_SetItemCount(n)`(+首行 EnsureVisible 恢复);
   LVN_GETDISPINFO 供 6 列文本(分类本地化串、风险徽标、FormatSize、OneLine、
   FormatTimeShort —— 建议在扫描/加载时一次性物化进 snapshot_,回调零分配)+
   `LVIF_STATE`(stateMask=LVIS_STATEIMAGEMASK)从 checkStateByPath_ 供勾选位
   (owner-data 下控件不缓存 checkbox 状态,必须回调供给 —— 这正好让 map 成为唯一真源,
   **U-4/U-6/U-8 与 CaptureCheckState 机制一并消失**)。
3. NM_CLICK/LVN_ITEMCHANGED:勾选翻转 → map 写入 + 增量 UpdateExecButton(O(1),
   checkedCount/bytes 常驻维护)。
4. NM_CUSTOMDRAW 的 ItemAtRow(MainWindow.cpp:270-272)改 `snapshot_[order_[row]]`;
   OnOpenLocation 的 ListView_GetItem lParam 同理(Presenters::ItemAtRow 单点收口)。
5. 排序:U-5 的键预计算 + order_ 重排,不再触发重插。
6. HistoryPresenter 同模式改造(行数小,可选);FolderTree(TreeView,顶层文件夹,量小)不动。
7. 测试:PerfBenchTests 增渲染相位断言;`-tour` 视觉巡页验证勾选/滚动/排序(X-7 回归面)。

### 6.3 风险

- owner-data + LVS_EX_CHECKBOXES 的 state 回调语义是文档化行为,但**需在 Win10/11
  comctl 6.0x 实测一次**(探针 M 未覆盖 state 位回调)—— 列为改造首个里程碑。
- LABELTIP/FULLROWSELECT/DOUBLEBUFFER 与 owner-data 兼容(Everything/资源管理器同款组合)。
- 共享 ListView 跨页串染类缺陷(R-1/U-8)随"per-presenter order_+map"结构性消失,是收益项。
- 回滚成本低:保留现 RenderItems 路径,样式开关切换(约 20 行)即可 A/B。

**结论:虚拟化是唯一能把全部渲染路径拉进预算的方案,强烈建议排 P0;U-5/U-6 是其子集工作,
可先行合入。**

---

## 7. 修复顺序建议

| 序 | 项 | 规模 | 依据 |
|---|---|---|---|
| 1 | U-1 虚拟列表(含 6.2-2 state 回调验证里程碑) | M(1.5-2.5 天) | 所有超标路径的唯一治本 |
| 2 | U-5 排序键预计算(虚拟化前置子集) | S | 82ms→4.6ms@5000,独立可合 |
| 3 | U-6 勾选增量计数(虚拟化后自然 O(1),先行版 S) | S | 20-40ms→0@5000 |
| 4 | U-2 缓存懒加载/worker 化 | S-M | 最坏首屏 1.1s→44ms |
| 5 | U-8 切页前 Capture 外出页(虚拟化前必加) | S | 正确性 |
| 6 | §5 watchdog | S-M | 后续一切延迟工作的度量基线 |
| 7 | U-3 关闭 join 可见性 / U-7 tour 自保护 | S | 打磨 |

---

## 8. 被推翻假设 / 未验证

**被推翻**(证据见 §2 末表):Results 持锁拷贝、状态栏频率、LocalizeCategory/OneLine、
RestoreViewState O(N·K)、DeleteAllItems、"插入慢=绘制"、SETITEMCOUNT 提示、
DOUBLEBUFFER/LABELTIP 归因、命令面板开销。

**未验证/局限**:
1. 未活体运行 MiniSys.exe(约束)—— 预算表为实测组件求和;应用级端到端数字待 watchdog
   (§5)落地后一行日志即可校准。
2. 探针在 96 DPI 单显示器测得;150% DPI 下列宽/行高放大,插入成本估计 +10-30%(列数不变,
   不影响结论量级)。
3. owner-data 下 checkbox state 回调(LVIF_STATE)未在探针中实测 —— 虚拟化首个里程碑。
4. U-8 为代码级追踪(所有 Capture 调用点穷举:Presenters.cpp:60/360/409/568 均为
   "本 presenter 行在屏"时机,切页无外出捕获),未活体复现;置信度 Confirmed(代码路径)
   /活体待验。
5. 关闭时 262MB 缓存写尾巴(U-3)为容量/盘速推算,未实测 fwrite 时长。
6. jsonbench 的合成 5000 项文档字段全非空;真实数据含空字段时 parse 略快,量级不变。

## 附:产物清单(全部在 %TEMP%\minisys-review-20261011\)

- `02-exp\lvbench.cpp / build_lv.cmd / lvbench.exe / lvbench.txt`(ListView 微基准,源码+两轮原始输出)
- `02-exp\jsonprobe\`(jsonbench 源码 + 仓库 Json/StringUtils 副本 + jsonbench.txt)
- `02-ui-latency.md`(本报告)
