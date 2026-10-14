# 02 · UI 线程卡顿/延迟审查(v2.19a)

> 日期:2026-10-14 · 对象:HEAD `2a9d653`(v2.19a,2026-10-14 02:12,工作树源码无改动,git status 仅 docs/未跟踪)
> 运行版本归属:build/Latest/MiniSys.exe(mtime 2026-10-14 02:12)与日志会话起点 02:12:20.949、提交 2a9d653(02:12)三者一致。
> 规模口径:索引 3,014,639 项(日志实测 enum done(V1));index-cache-C.bin = 267,743,481 B;搜索结果缓存 results-1.json = 437,184 B / 1000 项;history.jsonl = 23,430 B / 93 条;settings.json = 211 B。
> 方法:全文精读 MainWindow.cpp(2229)/Presenters.cpp(986)/SessionService.cpp(1094)/Layout.cpp/Controls.cpp/Settings.cpp/Logger.cpp/main.cpp/VolumeIndex.cpp(选读)/OperationLog.cpp(选读)+ 复跑上轮微基准 jsonbench.exe(今日真实缓存文件)与 lvbench.exe(本机复测)+ 日志副本时间戳分析(.tmp-review/r6/minisys-log-copy.log,949 行,截至 02:12:21)。**未启动/未触碰运行中的 MiniSys.exe**(pid 19128)。

---

## 0. 摘要:Top 卡顿根因排名

| # | 根因 | 位置 | 量级(当前规模) | 置信度 |
|---|---|---|---|---|
| 1 | **键入→列表更新 ≈ 470-520ms**:200ms 防抖 + worker 全索引线性扫描 ~230ms(76ms/百万 × 3.01M,上轮实测率外推)+ v2.19 全行取大小/排序/收尾 ~30-70ms + UI 落屏 ~8-12ms | MainWindow.cpp:507/1701、SessionService.cpp:497-599、VolumeIndex.cpp:1025 | 每键 ~0.5s | 结构 Confirmed / 量值 Likely(无活体搜索计时) |
| 2 | **SaveCacheIfWorthwhile 无条件全量重写 267MB 且无取消检查点**:每次启动、每次空闲维护(≥15min)都 RefreshFromUsn 成功即全量 fwrite(3.08M 次 40B 逐行 fwrite);期间 cancelScan_ 无效 → 用户回位打字被 busy 闸阻塞(搜索 80ms 重试空转)最长≈整个写尾;Shutdown join 同等 | VolumeIndex.cpp:266-303(291-292 无条件 save)、1262-1317(写循环无 cancel);SessionService.cpp:309-341(CAS busy 闸)、1089-1092 | 写尾 ~1-5s(NVMe 推断);cache mtime=02:12 证明每次启动都重写 | 代码 Confirmed / 时长 Likely |
| 3 | **启动首帧 ≈ 510ms(实测)**:进程/COM/日志初始化 + CreateControls + 5×SHGetStockIconInfo + LoadSettings + OnTabChanged(含首次 SaveSettings 写盘)+ **TryLoadCachedResults×5 同步 59.5ms(今日实测)** + Refresh@1000 | MainWindow.cpp:775-852、816-818;SessionService.cpp:240-307 | 两会话 507/512ms(日志实测) | Confirmed(实测) |
| 4 | **启动后 ~5s 搜索 UI 被禁用**:索引缓存 267MB 载入实测 3.0-5.4s(4 会话)在 worker,但 SetTaskBusy(Indexing) 关掉 editSearch/chkMatchPath/btnQuickFilter;载入完成后才自动补跑恢复的查询 | MainWindow.cpp:1645-1653、1404-1418;VolumeIndex.cpp:1175-1260(TryLoadCache 3.08M 次逐行 fread,无 cancel 检查点) | 启动→可搜索 ≈5.5-6s | Confirmed |
| 5 | **排序比较器 ToLower 堆分配未修(上轮 U-5 遗留)**:case 0/2 每次比较 2-4 个临时 wstring;今日复测 J 相位 61.7-84ms@5000 / ~13ms@1000;预计算键(J2)3.0-4.1ms → 20× | Presenters.cpp:461-506 | @1000 排序总 ~20ms(可过 100ms、超 16ms 帧);@5000 ~120ms 超标 | Confirmed(复测) |
| 6 | History 页激活 O(N) 注册表+HMAC(无任何缓存):每行 VerifySignature → 每次 RegOpen+RegQuery 取机器密钥 + 每次 BCryptOpenAlgorithmProvider;93 条今日 ~10-15ms,1000 条线性 → ~80-120ms | Presenters.cpp:876-944、OperationLog.cpp:243-248、SecretStore.cpp:64-107 | 今日可过 / 增长后超标 | 代码 Confirmed / 外推 Likely |
| 7 | 次级:每次切标签 SaveSettings 同步写盘+MoveFileExW(~0.5-2ms,MainWindow.cpp:1424);UI 热路径 MS_LOG 逐条 flush(每键 2 次,MainWindow.cpp:1702 + SessionService.cpp:498,Logger.cpp:70-73);NM_CUSTOMDRAW 每行 SPI_GETHIGHCONTRAST 系统调用(MainWindow.cpp:387-391);resultsMu_ 持锁窗口含日志 flush/全量拷贝(SessionService.cpp:185-187、587-595、624-639) | 各处 | 单次 0.1-2ms,磁盘忙时尖峰 | Confirmed(代码) |

**上轮三项遗留复核**:①UI stall watchdog — **未做**(grep 全 src 无 watchdog/QPC 代码);②启动异步 JSON — **未做**(OnCreate 816-818 仍同步循环);③关闭尾巴 — **未做**(RunMessageLoop 尾 SessionService::Shutdown join,根因即 #2)。内存压缩(路线图#2)未做,工作集 ~905MB 与线索一致。

**好消息(上轮 P0 的交付质量)**:虚拟列表在位且有效 —— 今日复测 M 相位:SETITEMCOUNT+全窗强刷 68.7-83ms 且 @1000/@5000/@100000 同价;滚动/勾选/全选/切标签全部进预算(见预算表)。上轮 U-1(1.26s 渲染)/U-4(134ms 批量勾选)/U-6(勾选全表扫描)/U-8(切页串染)随机制消失,已逐项核对代码确认。

---

## 1. 方法与证据

| 数据 | 来源 |
|---|---|
| 启动首帧 | 日志副本:"MiniSys starting" → 首条 "DrawInfoText"(首次信息条绘制,首帧可见的强代理):01:56:50.527→51.034 = **507ms**;02:12:20.949→21.461 = **512ms**(两会话一致) |
| 索引缓存载入 | 日志副本 4 会话 "loaded cache":+2.95s / +5.29s / +4.98s / +5.37s(3.01M 项,267MB) |
| 启动 JSON 成本 | jsonbench.exe(上轮探针,读真实缓存只读)今日复跑:results-1 1000 项 **43.3ms**(parse 39.9)、results-2 226 项 9.0、results-0 86 项 4.4、results-3 1.9、results-4 0.9 → 合计 **59.5ms**(探针比现行少解析 2 字段,实际 ~62-66ms);合成 5000 项 203.8ms |
| ListView 各相位 | lvbench.exe 今日复跑(与上轮同源探针):J 排序 ToLower@5000 61.7-84ms、J2 预计算 3.0-4.1、K Results 拷贝@1000 0.23-0.58ms、L UpdateStatusBar 消息组 1.66µs、L2 GetDiskFreeSpaceExW 39.4µs、M 虚拟全窗强刷 68.7-83ms(N 无关)、M2 跳行重绘 ~60ms |
| 调用链/锁语义 | 全文读码(见各条 file:line);grep 复核(watchdog=无、SetBatchUpdate=无调用者、MS_LOG UI 侧清单) |
| 未做 | 活体搜索端到端计时(日志副本截至启动后 0.5s,无一次 SearchAsync 记录);fwrite 267MB 实际时长;触碰运行中进程(约束) |

---

## 2. 发现清单(U-1…)

### U-1(CONFIRMED·实测)启动首帧 ≈510ms,构成可分解、可削减
- **触发**:每次启动。链:main.cpp:63-73(COM+DPI+CrashDump+Logger 开文件+3×EnablePrivilege)→ MainWindow::Create → WM_CREATE → OnCreate(MainWindow.cpp:775-852):CreateControls(776,~27 控件+NONCLIENTMETRICS 字体)→ 5×SetStockButtonIcon(798-802,SHGetStockIconInfo 走 shell 图标缓存,可含磁盘)→ LoadSettings(810,settings.json 同步读)→ OnTabChanged(811,完整切页:ShowWindow 批量+info 172 字符+BuildIndexAsync 启 worker+Refresh 空+**SaveSettings 写盘**+OnSize/DT_CALCRECT)→ **TryLoadCachedResults×5(816-818,同步 59.5ms 实测)** → Refresh(820,RenderItems@1000 ~5-8ms)→ ShowWindow/UpdateWindow(216-217)。
- **后果**:507/512ms 两会话一致,超上轮 ≤500ms 目标;距"丝滑"差距主因是进程初始化+图标+同步 JSON 三块。
- **修复草案**:(a) 非活动 tab 缓存改首次激活懒加载(OnTabChanged 里 try-load-once,活动 tab 保留同步)→ 省 ~16ms(仅留 results-1);(b) 全部投 worker + DONE 后重渲染 → 省 ~60ms;(c) 图标装载延后到首帧后(SetTimer 0)→ 省 ~10-50ms(Likely)。

### U-2(LIKELY·组件求和)键入→列表更新 ≈470-520ms —— 交互延迟第一源
- **链**:EN_CHANGE(MainWindow.cpp:507-509)→ SetTimer 200ms(每键重置,连打不触发 ✓)→ WM_TIMER(701-705)→ RunSearch(1701-1720:MS_LOG flush + 若在搜则 CancelScan;SearchAsync 失败则 80ms 重试 1707-1717)→ worker body(SessionService.cpp:503-596):ParseFilterTerms → **VolumeIndex::Search 线性扫 3.01M 节点**(VolumeIndex.cpp:1058-1110;上轮实测 76ms/百万 → ~230ms)→ pass2 std::sort(ToLower/比较,1114-1116,~3-5ms@1000 命中)→ **v2.19 全行取大小**(541-568:≤6 线程池 GetFileAttributesExW×1000,~20-60ms)→ StoreResults → 3×Results() 全量拷贝仅为 SetProgress/2 条 MS_LOG(587-595,~0.75ms/次)→ Post SEARCH_DONE → UI:Refresh(Results 拷 0.25ms+RenderItems ~6ms+RestoreViewState)+UpdateSearchStatus→ShowHint→SetWindowText+OnSize(1757-1765)+UpdateStatusBar ≈ **8-12ms**。
- **后果**:每键 0.5s 才见结果;连续输入时每次停顿都付一次全价。UI 线程本身全程 <15ms(不卡、只慢)。
- **修复草案**:(a) 防抖 200→100ms(中断输入的即时感翻倍,S);(b) 大小补齐只同步取可见 ~前 100 行,其余 idle 分批/滚动时按需(v2.19 目的的等价实现,S);(c) pass1 降本属 03 号领域(如后缀桶/名称池 SIMD),另报。

### U-3(CONFIRMED)267MB 索引缓存:启动载入 3-5.4s 期间搜索 UI 禁用;载入/保存均无取消检查点
- **证据**:日志 4 会话载入 +2.95/+5.29/+4.98/+5.37s(TryLoadCache 3.08M 次 40B 逐行 fread + Finalize 三张哈希表,VolumeIndex.cpp:1208-1245);SetTaskBusy(Indexing) 禁 editSearch/chkMatchPath/btnQuickFilter(MainWindow.cpp:1645-1653,scanning=true 分支);OnScanDone 1555-1562 才补跑恢复的 "ext:pdb"。
- **后果**:启动→可搜索 ≈5.5-6s(载入 3-5.4 + 搜索 ~0.3);窗口可浏览但"输入即搜"承诺延迟兑现。关闭/接管(-takeover,main.cpp:19-44 等 10s)期间 join 亦含此路径。
- **修复草案**:fread 改整块读(267MB 一次 fread 到缓冲再指针展开,预计 3-5s → ~1-2s);分段处查 cancelScan_;SaveSettings 式"载入完成先放行 UI"(先建最小可用索引或允许对旧缓存先搜)。

### U-4(CONFIRMED·代码)SaveCacheIfWorthwhile 无条件全量重写 + 无取消点 → 回位打字阻塞与关闭尾巴的共同根因
- **证据**:EnsureBuilt(VolumeIndex.cpp:283-298):valid 且 serial 匹配 → `RefreshFromUsn(cancel)` 成功(**即使 0 变更**)→ `SaveCacheIfWorthwhile()` 无条件全量写(1262-1317:3.08M 次 40B 逐行 fwrite,无 cancel 检查;仅 nodes≥10000 门槛)。调用方:BuildIndexAsync(每次启动,SessionService.cpp:359-391)、IdleMaintenanceAsync(≥15min 空闲,413-495)。busy 闸:StartTask CAS(SessionService.cpp:309-316)→ 搜索/新任务在 worker 结束前 IsBusy()=false 不成立 → RunSearch 80ms 重试空转(MainWindow.cpp:1713-1717)。WM_ACTIVATE 的 CancelScan(MainWindow.cpp:756-762)对 fwrite 循环无效。
- **后果**:①每次启动在 ~5s 载入后再花 ~1-5s 重写 267MB(纯浪费,除非有 journal 增量);②空闲维护后用户回来立刻打字 → 搜索无响应直到写完(cache mtime=02:12 佐证每次启动重写);③WM_CLOSE 后进程存活=写尾(上轮 U-3 遗留 #5 的根因);④磁盘被自家 267MB 写占满时,U-1/U-2 的 JSON/日志 flush 全体变慢(正反馈)。
- **修复草案**:(a) RefreshFromUsn 返回实际变更数,>0 才写(省掉绝大多数全量写);(b) 写循环每 64K 行查 cancelScan_,取消则删 .tmp 保留旧缓存;(c) fwrite 改整块缓冲写(写时长 5-10×);(d) 关闭路径把未完成的保存转后台线程+限时 join(遗留 #5)。

### U-5(CONFIRMED·复测)排序比较器 ToLower 分配未修(上轮 U-5 原样)
- Presenters.cpp:461-506,case 0(分类)最坏 4 次 ToLower/比较、case 2(项目)2 次;今日复测 J:61.7-83.9ms@5000、~13ms@1000;J2 预计算 3.0-4.1ms。
- **后果**:排序点击总成本 ~20ms@1000(过 100ms 预算、超 16ms 帧)、~120ms@5000(超标)。
- **修复草案**:上轮方案不变 —— snapshot_ 挂 lowerTitle/lowerCategory 键或排 uint32 索引;S 规模,独立可合。

### U-6(CONFIRMED·代码)History 页激活的 O(N) 注册表+HMAC(无缓存)
- HistoryPresenter::Refresh(Presenters.cpp:876-898):LoadAll(整文件读+93 条 JSONL 解析)+ 每行 HistoryRiskLabel → VerifySignature(OperationLog.cpp:243-248)→ GetOrCreateMachineSecret **每次** RegOpen+RegQuery(SecretStore.cpp:64-74,无静态缓存)+ HmacSha256 **每次** BCryptOpenAlgorithmProvider/CreateHash(76-107,无句柄复用)。UndoSelected(946-984)再验一遍。
- **后果**:93 条 ≈10-15ms(可过);线性增长,千条历史时 ~80-120ms 超预算。
- **修复草案**:secret 与算法句柄各一次进程缓存(S);LoadAll 解析时顺手验签一次存入 OpRecord(bool),渲染零加密。

### U-7(CONFIRMED·代码,量级微)NM_CUSTOMDRAW 每行一次 SPI_GETHIGHCONTRAST
- MainWindow.cpp:382-392:CDDS_ITEMPREPAINT 对每个可见行调 SystemParametersInfoW(SPI_GETHIGHCONTRAST)(~1-10µs 系统调用)+ dynamic_cast + ItemAtRow。滚动/重绘期间每帧 ×可见行数(~40)。
- **修复草案**:进程缓存 + WM_SETTINGCHANGE 失效,或每 CDDS_PREPAINT 查一次存 thread_local。S,收益微(纯洁癖级,但一行改动)。

### U-8(CONFIRMED·代码)切标签即同步写盘:SaveSettings
- OnTabChanged 尾 MainWindow.cpp:1424 每次 SaveSettings → Settings::Save(Settings.cpp:54-81):序列化 + ofstream 写 .tmp + MoveFileExW。同路径还有 IDC_CHK_MATCHPATH(514)、IDC_CHK_ADVANCED(547)、WM_CLOSE(765)。~0.5-2ms/次,杀软/盘忙时尖峰可能。
- **修复草案**:脏标记 + 500ms 合并定时器 + WM_CLOSE 兜底。S。

### U-9(CONFIRMED·代码)UI 热路径 MS_LOG 逐条 flush
- Logger::Log(Logger.cpp:62-74):mutex + ofstream << + **flush()** + OutputDebugStringW(再拼一次串)。UI 线程调用点:RunSearch(每键,MainWindow.cpp:1702)、SearchAsync enter(每键,SessionService.cpp:498)、OnScanDone(1546)、DrawInfoText 前 2 次(134-139)、Exclusion(2174)。
- **后果**:每键 2 次同步刷盘(~0.2-2ms;与 U-4 的 267MB 写并发时被放大)。
- **修复草案**:后台日志线程(环形缓冲+批量 flush)或 INFO 级免 flush。S。

### U-10(CONFIRMED·代码,量级小)resultsMu_ 持锁窗口含 flush/全量拷贝
- RunScan 在锁内 MS_LOG_INFO+SetProgress(SessionService.cpp:624-639);SaveResultsCache 在锁内全量拷贝 1000-5000 项(185-187);SearchAsync 收尾 3×Results() 拷贝(587-595);IdleMaintenance 每表 Results(tab) 拷贝(449)。UI 侧 Results(tab)/Refresh 需同锁 —— 等待 ≈拷贝时长(0.25-2.6ms,K 相位实测)+ 日志 flush 尖峰。上轮"争锁无问题"结论在 v2.19 的 3×收尾拷贝出现后略微恶化,仍属 P3。
- **修复草案**:SearchAsync 收尾缓存 hits 计数局部变量(省 3 拷贝);RunScan 日志移出锁。S。

### 次级观察(不计编号)
- **取消竞态闪变**:被取消搜索的 WM_APP_SEARCH_DONE 若在新任务启动前到达(gen 未变)会以部分结果 Refresh 一次,80ms 后纠正(SessionService.cpp:596 post 用当下 gen;MainWindow.cpp:641-643 门槛放行)。外观小问题,非延迟。
- **batchUpdate_ 死标志**:v2.15 后无 SetBatchUpdate(true) 调用者(Presenters.h:102),LVN_ITEMCHANGED 的 InBatchUpdate() 分支(MainWindow.cpp:361)恒真路径死代码。
- **RestoreViewState O(N×S)**(Presenters.cpp:93-124):S=选中行数;实测口径 0.25ms@1000×50 → 全选后每次 Refresh ~5ms@1000,可忽略;仅在搜索结果全选+连打时叠进 U-2。

---

## 3. 延迟预算表(现状 @1000 行 vs <100ms 命令预算 / 16ms 帧)

| # | 场景 | 调用链(责任点) | 现状 | 判定 | 主因 |
|---|---|---|---|---|---|
| 1 | 启动首帧 | main→OnCreate(§2 U-1) | **507/512ms 实测** | ✗(超 500) | 进程 init+图标+同步 JSON 59.5ms |
| 2 | 启动→可搜索 | BuildIndexAsync→TryLoadCache(3-5.4s 实测)→OnScanDone→RunSearch | **≈5.5-6s** | ✗(功能等待,UI 不卡) | U-3 |
| 3 | 键入→列表更新 | 200ms 防抖+worker 搜索+落屏(§2 U-2) | **≈470-520ms**(Likely) | ✗ | U-2(防抖+全索引扫描) |
| 4 | 滚动 1000 行 | 虚拟列表 GETDISPINFO×可见行+DOUBLEBUFFER | **<1ms/帧**;最坏全窗强刷 69-83ms(实测) | ✓ | — |
| 5 | 勾选一行 | NM_CLICK/Space→ToggleCheckAt(map+单行重绘)+UpdateExecButton→CollectChecked O(N) | **1-2ms** | ✓ | — |
| 6 | Ctrl+A 全选/全不选 | SetAllChecked O(N) map+一次 Redraw+O(N) recount | **2-3ms** | ✓ | — |
| 7 | 排序点击 | ApplySortAndRefresh:sort 13ms@1000/62-84ms@5000 + RenderItems 6-8ms | **~20ms@1000 / ~120ms@5000** | ✓@1000 / ✗@5000 | U-5 |
| 8 | 切标签 | OnTabChanged:ShowWindow 批量+info 重绘+Refresh(拷 0.25+渲染 6)+UpdateStatusBar×2+SaveSettings 写盘+OnSize | **~10-20ms**(+1-2ms 写盘) | ✓ | — |
| 9 | 历史页激活 | LoadAll+93×(注册表+HMAC) | **~10-15ms@93** | ✓(千条时 ✗) | U-6 |
| 10 | 搜索落屏(UI 侧) | SEARCH_DONE→Refresh+ShowHint→OnSize+UpdateStatusBar | **8-12ms** | ✓ | — |
| 11 | Alt-Tab 回位校验 | WM_ACTIVATE→400ms timer→OnVerifyList(paths 拷 0.3-1ms UI)→worker 30-60ms | UI **<1ms** | ✓ | — |
| 12 | 空闲维护期回位打字 | busy 闸直至 267MB fwrite 写完 | **最长≈写尾 1-5s**(Likely) | ✗ | U-4 |
| 13 | 关闭 | SaveSettings→Shutdown join | 常规 **<10ms**;写缓存中 **1-5s** | ✓/✗ 最坏 | U-4(遗留#5) |

---

## 4. 卡顿可观测性方案(上轮遗留 #1 的落地设计,针对当前代码)

上轮 review-2026-10-06-3/02-ui-latency.md §5 伪代码仍适用(泵级 QPC 计时 + RAII PhaseTag + 心跳线程 + >500ms 冻结抓栈),本轮只列**注入点更新**与**新增项**:

1. **泵计时**:RunMessageLoop(MainWindow.cpp:231-268)GetMessageW→DispatchMessageW 前后 QPC;>50ms 记 {msg, wp, ms, phase}。注意模态内层泵(RootsDlgProc 1219-1225、TaskDialog、TrackPopupMenu)由心跳层兜底。
2. **PhaseTag 注入点**(RAII 压栈):OnCreate、OnTabChanged、ListTabPresenter::Refresh/RenderItems/ApplySortAndRefresh、RunSearch、OnScanDone、OnVerifyDone、OnPlanDone、ShowHint→LayoutWindow、DrawInfoText。把 "WM_APP_SEARCH_DONE 12ms" 直接归因到 "Refresh/RenderItems"。
3. **心跳抓栈**:独立线程 250ms PostMessage WM_APP_WATCHDOG_BEAT;UI 侧 WndProc 首行 atomic store 回执;连续 2 拍未回执 → SuspendThread+CaptureStackBackTrace(复用 platform/CrashDump.cpp 机制,写 %LOCALAPPDATA%\MiniSys\logs\stalls.log,不解析符号、不上送;同一进程限 10 次/小时)。
4. **新增:worker 任务阶段 span**(本轮证明必要——日志有时间戳但无耗时字段,搜索/缓存读写全靠推算):StartTask 记任务墙钟;EnsureBuilt 内 TryLoadCache / RefreshFromUsn / SaveCacheIfWorthwhile / Search pass1 / size-fetch 各记 QPC span 一条 INFO("phase=save_cache ms=1834 bytes=267743481")。一行改动级,先于 watchdog 落地即可校准 U-2/U-4 的量值。
5. **新增:启动打点**:OnCreate 末尾与首帧(可在 DrawInfoText 首绘)各一条 INFO 含 ms —— 本轮 510ms 就是从日志时间戳人肉算出的,应自动化。
6. 开销预算:泵计时 ~40-60ns/消息(<0.1% 单核);心跳 4 msg/s;抓栈仅事故态。

---

## 5. 修复顺序建议(收益/成本)

| 序 | 项 | 规模 | 预期收益 |
|---|---|---|---|
| 1 | U-4 缓存写入条件化(变更数>0 才写)+ 写循环取消点 + 整块写 | S-M | 消除每次启动/空闲维护的秒级磁盘写;回位打字不再被阻塞;关闭尾巴大概率消失(遗留#5) |
| 2 | §4-4/4-5 worker 阶段 span + 启动打点(先于 watchdog) | S | 所有延迟结论从推算变实测,后续一切工作的度量基线 |
| 3 | U-2(a)(b):防抖 200→100ms + 大小补齐分批 | S | 键入延迟 ~470→~330-380ms(最大单杠杆) |
| 4 | §4 watchdog 全量 | S-M | 冻结可归因;校准 U-2/U-4 |
| 5 | U-1(b)(c):启动 JSON worker 化/懒加载 + 图标延后(遗留#4) | S | 首帧 510→~420-450ms(进程 init 占底) |
| 6 | U-5 排序键预计算 | S | @5000 排序 62-84→3-4ms(20×,复测验证过) |
| 7 | U-3 TryLoadCache 整块读+取消点 | S-M | 启动→可搜索 5.5→~2-3s |
| 8 | P3 打包:U-6 验签缓存、U-7 SPI 缓存、U-8 SaveSettings 节流、U-9 日志异步、U-10 锁窗口收紧 | 各 S | 各 1-15ms;历史页千条免疫 |

---

## 6. 线索逐条结论(任务书 ①-⑥)

| 线索 | 结论 | 证据 |
|---|---|---|
| ① 列表 LVS_OWNERDATA + cells_ 预渲染 6 wstring/行 | **成立且健康**:Refresh@1000 ≈5-8ms(列宽 6×GET/Set + SyncCheckDefaults O(N) ToLower+map + 6000 wstring 赋值 + FormatTimeShort×1000 + SETITEMCOUNT + RestoreViewState),非卡顿源 | Presenters.cpp:135-172;M 相位复测 N 无关 |
| ② 每键 SearchAsync,worker 每次 spawn ≤6 线程取 1000 行大小 | **成立**:池= min(hw,6),n<64 退 1 线程(SessionService.cpp:541-568);spawn/join ~0.5ms/搜索(worker 侧,无风暴——busy 闸+80ms 重试);但它是 U-2 端到端延迟的组成(~20-60ms@1000) | SessionService.cpp:497-599 |
| ③ DrawInfoText SS_OWNERDRAW(GetWindowTextW 2048 + 逐行 DrawText) | **成立且无害**:实测文本 172 字符/3 行(日志),单次 ~30-100µs;每次 ShowHint 额外触发 OnSize 全布局(~0.5-1ms);不在滚动/键入热路径 | MainWindow.cpp:119-178;日志 "DrawInfoText: 172 chars, 3 lines" |
| ④ Tab NM_CUSTOMDRAW 选中加粗(BoldUiFont 静态缓存) | **成立且正确**:首次 WM_GETFONT+GetObject+CreateFontIndirect 后进程缓存;仅选中项 ITEMPREPAINT 一次 SelectObject | MainWindow.cpp:100-113/332-348 |
| ⑤ Layout.cpp 信息条高度 DT_CALCRECT 测量 | **成立且无害**:每次布局 GetWindowTextW(2048)+一次 DT_WORDBREAK\|DT_CALCRECT(~50-200µs@172 字符),频率=信息变更/OnSize,非热路径 | Layout.cpp:122-145 |
| ⑥ FolderTree 两遍建树(depth=2,实际 ≤4 轮) | **成立且无害**:UI 线程 TreeView 重建,N=27-200 项,<10ms;3M 项扫描本身在 worker(串行定大小是 03 号遗留#3) | Presenters.cpp:643-734 |

---

## 7. 被推翻的怀疑 / 已验证无问题

| 怀疑 | 结论 | 证据 |
|---|---|---|
| 虚拟化后勾选/批量勾选仍是消息风暴 | **无问题**:map 权威化,单行=O(log N)+单行 Redraw;Ctrl+A=O(N)+一次 RedrawItems(2-3ms@1000) | Presenters.cpp:216-241;MainWindow.cpp:570-580/2112-2129 |
| 快速键入造成 worker 线程 spawn 风暴 | **无问题**:200ms 防抖每键重置(连打不触发);StartTask CAS 拒绝并发;80ms 重试节流 | MainWindow.cpp:507-509/1713-1717;SessionService.cpp:309-316 |
| StartTask 在 UI 线程 join 前一 worker 会卡 | **无问题(常规)**:join 只在 CAS 成功后发生,此时旧 worker 已置 taskKind_=None、仅剩 CoUninitialize+线程退出(µs 级);例外=U-4 的 fwrite 尾(CAS 进不去,表现为重试而非 join 卡) | SessionService.cpp:315/338 |
| LVN_GETDISPINFO 每格 wcsncpy_s / 每行状态 map 查找拖滚动 | **无问题**:SSO 拷贝+一次 ToLower+map find ≈1µs/行;滚动帧 <1ms | Presenters.cpp:190-214 |
| LVS_EX_DOUBLEBUFFER 未启用 | **已启用** | Controls.cpp:33-35 |
| UpdateStatusBar 频率/磁盘查询拖累 | **无问题**(复测):消息组 1.66µs、GetDiskFreeSpaceExW 39.4µs;LargeFileScanner 进度每 1024 目录节流 | Layout/L 相位复测;LargeFileScanner.cpp:148-155 |
| WM_APP DONE 消息缺 gen 门槛导致旧任务覆写 | **无问题**:六种 DONE/START 全带门槛(含 wp==0 补盖) | MainWindow.cpp:612-698;SessionService.cpp:70-84 |
| DrawInfoText 每次绘制打日志拖累 | **无问题**:仅前 2 次记数(loggedDraws<2) | MainWindow.cpp:134-139 |
| 空闲维护 30s tick 永续重入(B-3 复发) | **无问题**:15min 冷却+WM_ACTIVATE 取消在位 | MainWindow.cpp:1305-1309/756-762 |
| 命令面板有重活 | **无问题**(上轮结论,代码未变) | CommandPalette.cpp 未改动 |

---

## 8. 未验证 / 局限

1. **无活体搜索计时**:日志副本截至 02:12:21(启动后 0.5s),全日志 0 条 "SearchAsync enter"/"Search done"(仅 2 条历史 RunSearch)。U-2 的 470-520ms = 上轮实测率(76ms/百万)×3.01M 外推 + 组件求和,标 Likely;§4-4 落地后一行日志可校准。
2. **267MB fwrite 实际时长 1-5s 为 NVMe 推断**(逐行 fwrite×3.08M 的 CRT 开销未单测);同样待 §4-4。
3. **unelevated 下 RefreshFromUsn 是否失败→BuildFull 全量 walk**:23:53 会话有 "cannot open volume C:(Win32 5)+USN truncated"(随后该会话无 loaded cache),而 01:19/01:41/01:56 会话载入后无警告 —— 需更长日志窗口判定;若成立,启动→可搜索在某些会话是分钟级(全量 walk),属 03 号领域但对 U-3 量值有决定性影响。
4. lvbench/jsonbench 复测时 MiniSys.exe 运行中(905MB 常驻)+ 评审机有并发负载:B2/C 等旧实插相位中位偏高属噪声(min 与上轮一致);关键相位 J/J2/K/L/M 用 min 口径,结论不受影响。
5. jsonbench 探针的 JsonToScanItem 比现行少 dangerous/lastWriteFiletime 两字段,59.5ms 应视作 ~62-66ms(约 +5-8%)。
6. 首帧 510ms 以首条 DrawInfoText 日志为代理(静态控件首绘≈窗口可见后首帧);未用 ETW/高速摄影精确到 WM_PAINT。
7. 未活体验证滚动帧率、DPI≠96(本机复测仍 96/同机)、owner-data 勾选的 comctl 语义差异(上轮已列,本轮代码路径与上轮交付一致,未见回归报告)。
8. 版本归属基于源码 HEAD 2a9d653 + build/Latest mtime 02:12 + 日志会话起点三者一致;未对运行中进程取模块哈希(约束禁触碰 pid 19128)。
