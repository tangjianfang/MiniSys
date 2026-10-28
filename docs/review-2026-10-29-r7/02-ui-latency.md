# 02 · UI 线程卡顿/延迟审查（r7 · v2.23）

> 日期：2026-10-29 · 对象：HEAD `6621d92`（v2.23，2026-10-15 02:16），工作树对 `MiniSys/src` 无改动（git status 仅 build_number.txt/docs）。
> 运行版本归属：build/Latest/MiniSys.exe mtime 2026-10-15 02:15、SHA-256 `60ccce52…19b83`，与 v2.23 提交时间一致；今日日志会话（01:29/02:00）即该 exe 产生。
> 规模口径：C: 索引 3,427,118 条（469,811 目录）；index-cache-C.bin 267,743,481 B（01:34 walk 后重写）；搜索页 1000 行/页；历史 history.jsonl 实测 **142 行**（任务书"约 330 条"与文件不符，按 142 计并注明）；settings.json 211 B。
> 方法：全文精读 MainWindow.cpp(2482)/Presenters.cpp(1061)/SessionService.cpp(1193)/VolumeIndex.cpp(1399)/Layout.cpp/Controls.cpp/CommandPalette.cpp/Icons.cpp/Dialogs.h/FastWalk.cpp/PathUtils.cpp/Logger.cpp/Settings.cpp/OperationLog.cpp/SecretStore.cpp/main.cpp + **今日用户日志逐行分析**（minisys.log 1334-1388 行，两会话完整）+ **微基准实测**（`.tmp-review/r7/scratch/r7bench.exe`，链接真实 OperationLog/SecretStore/Json/StringUtils/PathUtils 源文件 + Logger 桩，历史数据用 scratch 副本，HKCU RecordKey 只读）。未启动 MiniSys.exe。

---

## 0. 摘要：Top 卡顿根因排名

| # | 根因 | 位置 | 量级（今日规模实测/推算） | 置信度 |
|---|---|---|---|---|
| 1 | **扫描期间整个 UI 被锁死**：Scanning 模式禁用 tab/list/tree/搜索框/全部按钮（仅剩取消），而 LargeFiles 扫描 19-25s、FolderTree 16.7s（日志实测）；空闲自动重扫（数据龄≥30min）在用户离开时启动、回来时若未跑完仍处于锁定态 | MainWindow.cpp:1883-1948（SetTaskBusy lockBrowse/EnableWindow 矩阵）、1520-1567（OnIdleCheck 自动重扫）；日志 "Scan tab=2 … 24946/24241/19083 ms"、"tab=4 … 16681 ms" | 19-25s 交互全灭 | Confirmed（代码+日志） |
| 2 | **v2.23 惰性展开把全子树遍历放进单 worker 且不可抢占**：每展开一个节点，对每个子目录 `DirectorySizeParallel(kid,4)` 完整递归定大小（无取消、无预算）；FolderExpand 任务不置 maintenanceActive_ → 键入搜索被 busy 闸 80ms 空转直到展开完成；Shutdown join 等当前子树走完 | SessionService.cpp:531-582（FolderChildrenAsync）、PathUtils.cpp:174-283（SizePool 无 cancel）；MainWindow.cpp:707/1713-1717（busy 重试） | 展开 C:\ 顶层 ≈ 全盘 walk（16-50s）；展开大目录 = 其子树全量（秒~分钟级），期间搜索不可用 | Confirmed（代码）/ 量级 Likely |
| 3 | **启动→可搜索 ≈5.7s**：TryLoadCache 逐行 fread 3.4M×40B + **同步 Finalize 重建三张哈希表（B-4 惰性化仍未实施）**≈3.7-4.8s；期间 SetTaskBusy(Indexing) 禁用搜索 UI | VolumeIndex.cpp:1186-1271（TryLoadCache+Finalize:1256）、MainWindow.cpp:1652-1659/1883-1907；日志 01:29:22.946→27.772 ≈4.8s | 5.7s（实测 22.291→28.000） | Confirmed |
| 4 | **"重建索引" = 49s 全盘 walk + 267MB 逐行 fwrite + 4.7s Finalize，且写循环无取消检查点**：asInvoker 下打开卷句柄永远 Win32 5 → walk 是唯一索引来源；保存期间回位打字/关闭都要等写尾 | VolumeIndex.cpp:316-519（BuildFull→WalkBuild:528-681）、677-679（walk 后无条件 SaveCacheIfWorthwhile）、1273-1328（40B/行 fwrite 无 cancel）；日志 01:34:06.342→34:55.833 = 49.5s+4.7s | 54s/次手动重建；写尾 1-5s 阻塞 busy 闸与 Shutdown | Confirmed（代码+日志） |
| 5 | **键入→结果 ≈0.31-0.45s**：200ms 防抖 + worker pass1 线性扫描 96-233ms（日志实测）+ 大小补齐 + UI 落屏 8-12ms | MainWindow.cpp:716-718（200ms）、SessionService.cpp:599-698、VolumeIndex.cpp:1036-1142；日志 189ms/233ms/96ms | 每次停顿 0.3-0.45s | Confirmed（日志实测，链路分解） |
| 6 | **每行 160µs 的自绘复选框（X-41）+ 每行 2×SPI_GETHIGHCONTRAST + zebra 建刷 + UiScale 动态 GetProcAddress**：v2.20-v2.23 视觉改动的每帧账单 ≈183µs/行 → 整列表重绘（排序/刷新/改列宽/拖尺寸）≈5.5ms@30 可见行 | MainWindow.cpp:535-615（ITEMPREPAINT）、104-118（BoldUiFont 无关）+ Layout.cpp:14-33（UiScale 每次取 proc）；微基准 159.7µs/4.09µs/1.69µs/7.58µs | 全量重绘 +5.5ms；滚轮每档 +0.5ms | Confirmed（微基准实测） |
| 7 | **排序比较器 ToLower 堆分配仍未修（r6 U-5 原样）**：@1000 5.35ms vs 预计算 0.93ms（5.7×）、@5000 43ms vs 6.7ms（微基准复测） | Presenters.cpp:475-532 | @1000 排序总 ~12ms（勉强过帧预算）；缓存上限 5000 行时 43ms 超标 | Confirmed（复测） |
| 8 | **8.7s 的消息派发空隙（无法归因的 UI 线程/IO 停顿）**：01:32:46.282 worker 记录 "Scan tab=2 done" → 01:32:55.018 UI 才处理 OnScanDone（前两次扫描该间隔为 1ms/10ms） | 日志 1370-1372 行；候选：SaveResultsCache 的 MoveFileExW 停顿 / UI 线程硬缺页 / 杀软扫描新写文件 | 单次 8.7s 无响应窗口 | Confirmed（现象）/ 归因 Speculative |

**好消息（复核 r6 修复在位）**：B-1 缓存脏标志（VolumeIndex.cpp:299-302 仅 dirtySinceSave_ 才写）✅；B-3 空闲维护抢占（SessionService.cpp:365-369/591-595 maintenanceActive_）✅；B-6a 每键 3 次全量拷贝（SessionService.cpp:667-671 hitCount 先取后 move）✅；QuarantineUsageText 按 Generation 缓存（SessionService.cpp:979-1009）✅；B-2 Logger UTF-8 ✅（日志中文完整）。虚拟列表机制健康：搜索落屏 8-12ms、勾选/全选/反选均为 O(N) map + 单次重绘。

---

## 1. 方法与证据来源

| 数据 | 来源与口径 |
|---|---|
| 启动/搜索/扫描耗时 | 用户真实日志（只读副本分析）`%LOCALAPPDATA%\MiniSys\logs\minisys.log` 2026-10-29 两会话全量 55 行：首帧以首条 DrawInfoText 代理（r6 同口径）；搜索耗时 = "SearchAsync enter"→"Search worker body done"（worker 侧）；"Scan tab=N done: X items in Y ms" 为 worker 端到端 |
| 历史/排序/绘制/写盘微开销 | **本轮微基准实测**（.tmp-review/r7/scratch/：r7bench.cpp + build.cmd + 输出留存可重跑）：链接真实 OperationLog/SecretStore/Json/StringUtils/PathUtils（Logger 用桩，未触碰用户 minisys.log；历史用 history-copy.jsonl 副本经 SetHistoryPathForTesting 注入；HKCU RecordKey 已存在故只读）。/O2 x64，空载机器，MiniSys.exe 未运行 |
| 调用链/锁语义 | 全文读码（各条给 file:line） |
| 局限 | 无活体 UI 计时（未运行 GUI）；8.7s 空隙无堆栈；Finalize 与 save 的拆分靠 r6 冷启测量外推；FolderExpand 展开耗时无实测（该功能为 v2.23 新增，今日日志无展开事件） |

---

## 2. 发现清单（U-1…）

### U-1（Confirmed·日志实测）启动路径：首帧 517-655ms，启动→可搜索 5.7s；B-4 惰性化未实施
- **链**：main.cpp:63-73（COM/DPI/CrashDump/Logger 开文件/3×EnablePrivilege）→ OnCreate（MainWindow.cpp:1007-1084）：CreateControls（27 控件+NONCLIENTMETRICS）→ 5×SetStockButtonIcon（1030-1034）→ LoadSettings → OnTabChanged（含首次 SaveSettings 写盘）→ **TryLoadCachedResults×5 同步 JSON（1048-1050，r6 实测 59.5ms；results-1.json 428KB/1000 项是主项）** → 首帧（01:29:22.291→22.946 = **655ms**；02:00 会话 517ms）。搜索页 OnTabChanged → BuildIndexAsync → **TryLoadCache：3.4M 次 40B 逐行 fread（1186-1249）+ Finalize（1256）同步重建 byFrn_/childrenOf_/dirPaths_（187-265，DFS+三哈希表）** → 4.8s 后可用（22.946→27.772）。
- **后果**：冷启动后"输入即搜"承诺 5.7s 才兑现；Indexing 期间 editSearch/chkMatchPath/btnQuickFilter 被禁用（SetTaskBusy 1883-1907）。
- **修复草案**：(a) Finalize 惰性化——dirPaths_/frnToDirPath_ 只在首次 HasDir/Search/需要路径匹配时构建，或按需分片（r6 已给方案）；(b) fread 改整块读+指针展开（3-5s→<1s）；(c) 非活动 tab 缓存懒加载（首次激活才读）；(d) 分段处查 cancelScan_。
- **预期收益**：启动→可搜索 5.7→≈1.5-2s；首帧 −60ms（JSON 懒加载）。

### U-2（Confirmed·日志实测）键入→结果 ≈0.31-0.45s（r6 的 470-520ms 已自然改善：索引 3.4M 但 pass1 提前满 1000 截断）
- **链**：EN_CHANGE（MainWindow.cpp:716-718）→ SetTimer 200ms → WM_TIMER(933-937) → RunSearch（1953-1972，MS_LOG 同步刷盘 22.7µs）→ SearchAsync（worker）：ParseFilterTerms → **VolumeIndex::Search pass1 线性扫全量节点**（1069-1121；实测 'ext:pdb' 满页 189ms@3.09M / 233ms@3.43M；带 folder: 类过滤 96ms）→ pass2 sort(ToLower 比较)+OriginalCasePathOf → **全 1000 行 GetFileAttributesExW 补大小**（SessionService.cpp:636-664，≤6 线程）→ SaveResultsCache 节流 1.5s（677-685）→ Post → UI Refresh（Results 拷贝 0.25ms + RenderItems 预渲染 6000 wstring ≈6-8ms + RestoreViewState）+ ShowHint→OnSize 全布局 + UpdateStatusBar ≈8-12ms。
- **修复草案**：(a) 防抖 200→80-100ms；(b) pass1 降本：后缀桶（ext: 查询先按扩展名分桶倒排）或名称池一次性预扫描索引；(c) 大小补齐只做可见前 ~100 行，其余 idle 分批（r6 同案）；(d) 增量搜索（上次结果集内细化）。
- **预期收益**：键入→结果 0.31-0.45s → **<120ms**（防抖+pass1 桶化后 worker <30ms）。

### U-3（Confirmed·代码+日志）asInvoker 下索引只能靠 walk：重建=49s+267MB 写+4.7s Finalize；写循环无取消点
- **机制**：BuildFull 打开 `\\.\C:` 拿到 Win32 5（VolumeIndex.cpp:327-333，今日日志 01:34:06.342 实证）→ 走 WalkBuild 6 线程并行遍历（528-681，49s/3.43M 实测）→ **WalkBuild 成功即 SaveCacheIfWorthwhile（677-679）：3.43M 次 40B 逐行 fwrite（1305-1313），无 cancel 检查点** → BuildFull 再 Finalize+写日志（walk done 01:34:51.162 → indexed 01:34:55.833 = 4.7s，与 r6 Finalize 3.72s + 写盘 ~1s 相符）。B-1 修的是"零变更也重写"，walk 后的重写仍在。
- **触发条件**：仅手动"重建索引/F5@搜索页"（Invalidate→forceFullNext_）、缓存缺失/卷序列号不匹配、非 walk-built 缓存 + RefreshFromUsn 失败。**缓存为 walk-built（当前机器必然如此）时 EnsureBuilt 是 no-op**（RefreshFromUsn:699-700 `if (walkBuilt_) return true`）→ **普通启动不会 walk**（今日 01:29 会话证实：载入缓存 5.5s 即可搜）。
- **修复草案**：(a) 写循环每 64K 行查 cancel，取消则删 .tmp；(b) fwrite 改 4-8MB 块缓冲（写尾 5-10×）；(c) 重建走"先构建新树再原子替换"以保搜索可用；(d) 提权一次性建立 USN 权限的引导（或文档说明）。
- **预期收益**：写尾 1-5s→<0.5s 且可中断；关闭/接管不再等写尾。

### U-4（Confirmed·代码，量级 Likely）v2.23 惰性展开：FolderChildrenAsync 每个子目录全子树定大小，无取消、不可抢占、阻塞搜索
- **代码**：SessionService.cpp:536-581——枚举子目录循环内对每个 kid 调 `DirectorySizeParallel(kid, 4)`（PathUtils.cpp:274-283；SizePool Worker 212-261 **不接收 cancel**）；循环体仅在两个 kid 之间查 cancelScan_（550）。任务类型 FolderExpand **不设置 maintenanceActive_**（该标志只有 IdleMaintenanceAsync 设，443-447）→ SearchAsync/StartScan 的抢占分支（587-595/362-369）对它无效 → busy 闸期间 RunSearch 以 80ms 定时器空转（MainWindow.cpp:1965-1969）直到整个展开完成。Shutdown（1188-1191）cancel+join：只能在 kid 之间生效，当前 kid 的全子树遍历必须走完。
- **量级**：展开 C:\ 顶层 ≈ 一遍全盘 walk（参照 FolderTree 扫描 16.7s、walk 49s 热缓存差异）；展开任一大目录（Windows/Users）≈ 其子树全量。用户感知：点展开箭头 → "… 加载中"数十秒，**且此时切到搜索页打字完全无响应**（80ms 重试静默）。
- **修复草案**：(a) SizePool/FolderChildrenAsync 传入 cancel 引用，FindNextFileW 循环内检查；(b) DirectorySizeParallel 先浅层枚举（第一层立即返回条目数/浅大小），深大小后台分批补齐；(c) FolderExpand 改低优先级可抢占（或并入 maintenanceActive_ 语义）；(d) 展开期间在状态栏给出可取消提示。
- **预期收益**：展开从"数十秒黑箱+锁死搜索"变为"立即见子目录列表，大小渐进填充"。

### U-5（Confirmed·代码+日志）扫描期间 UI 全锁定；空闲自动重扫可埋伏回位用户
- **代码**：SetTaskBusy(Scanning)（MainWindow.cpp:1897-1907）：list/tree EnableWindow(FALSE)、tab EnableWindow(FALSE)（Indexing 除外）、editSearch/chkMatchPath/btnQuickFilter/exec/undo/open 全 FALSE——只留 scan=取消。OnIdleCheck（1520-1567）：空闲≥5min 且活动页数据龄≥30min → 10s 倒计时 → **OnScan() 整页重扫**（LargeFiles 20-25s、FolderTree 16.7s、Junk 3s）；数据龄 10-30min → IdleMaintenanceAsync（walk-built 卷 = RefreshWalk 全量重走 49s+写盘，459-469）。
- **用户感知**：任何一次扫描 = 9-25s 不能切页/不能搜索/不能排序（列表本身也被禁用）；回位用户撞上自动重扫时同样锁定（可用"取消"脱身，但无任何进度外的说明为何锁）。
- **修复草案**：(a) Scanning 仅锁定会与 worker 竞争 results_ 的动作（exec/scan），列表排序/勾选/切页本可只读浏览（v2.8 已为 verify/preview 网开一面，Indexing 也保留切页——把 Scanning 对齐同一策略，切换时由"落发起页"机制兜底，SessionService 已支持）；(b) 自动重扫改为仅闲时 + 回位立即取消（当前 WM_ACTIVATE 只取消 Verify，984-994）。
- **预期收益**：25s 锁定 → 0（浏览不受限）；这是"丝滑"最大单项。

### U-6（Confirmed·微基准）X-41 自绘复选框 DrawFrameControl = 159.7µs/行 —— 每帧绘制热点的 87%
- **代码**：MainWindow.cpp:572-590 —— CDDS_ITEMPREPAINT 对**每个可见行**调 DrawFrameControl(DFC_BUTTONCHECK)（uxtheme 主题绘制路径）。微基准（内存 DC、DPI 96）：**159.7µs/次**。30 可见行的整列表重绘 ≈ 4.8ms 仅复选框；滚轮每档（≈3 新行）≈0.5ms；列宽拖动/排序/刷新/改窗口尺寸都是全重绘路径。
- **修复草案**：按 DPI 预渲染 checked/unchecked 两枚小位图（进程缓存，WM_DPICHANGED 失效），ITEMPREPAINT 里 BitBlt/TransparentBlt（≈1-2µs，**80-150×**）。
- **预期收益**：整列表重绘 −4.5ms；排序/刷新/拖列宽/滚动全部受益。

### U-7（Confirmed·微基准）UiScale 每次调用都 GetModuleHandleW+GetProcAddress+GetDpiForWindow = 7.58µs
- **代码**：Layout.cpp:14-33 —— WindowDpi 每次动态解析 user32 句柄与 GetDpiForWindow 地址。调用密度：ITEMPREPAINT 每行 2 次（复选框 box/pad，MainWindow.cpp:576-577）+ DrawTabItem 每标签 1-2 次 + DrawPrimaryButton 2 次 + DrawInfoText 3 次 + LayoutWindow ≈40 次。30 行重绘 ≈ 60 次 ≈ 0.45ms；每行 15µs。
- **修复草案**：`static Fn fn = …` 缓存函数指针（一次解析），DPI 值可在 WM_DPICHANGED 时缓存到窗口。一行级改动。
- **预期收益**：绘制路径 −7.5µs/调用。

### U-8（Confirmed·微基准）v2.21a/v2.22/v2.23 新增的每行开销：双 SPI_GETHIGHCONTRAST（2×1.69µs）+ zebra 建刷（4.09µs）
- **代码**：MainWindow.cpp:547-563（zebra：每奇数行 CreateSolidBrush+FillRect+DeleteObject，且每行一次 SPI 查询）、595-599（风险色再做一次 SPI 查询——r6 U-7 的每行一次变成**每行两次**）；WM_ERASEBKGND（465-483，v2.23）每次整窗擦除再建刷+SPI 一次；DrawInfoText（257-291）每次绘制 2 次 SPI。
- **量级**：每行 ≈7.5µs 附加（不含 U-6/U-7）→ 30 行全重绘 ≈0.23ms。单项都小，但同帧叠加。
- **修复草案**：HC 状态进程缓存 + WM_SETTINGCHANGE 失效；zebra 刷静态化（两枚常量刷）；WM_ERASEBKGND 用预建刷。
- **预期收益**：微小（纯洁癖），但与 U-6/U-7 一起把"每行 183µs"压到 <5µs。

### U-9（Confirmed·微基准）历史页激活 = LoadAll 2.38ms + 142×VerifySignature 1.75ms ≈ 4.1ms（330 条 ≈8-9ms，千条 ≈25-30ms）
- **代码**：HistoryPresenter::Refresh（Presenters.cpp:951-973）→ LoadAll 全文件解析 + 每行 HistoryRiskLabel → VerifySignature（OperationLog.cpp:255-260）→ **每行** GetOrCreateMachineSecret（SecretStore.cpp:61-71，每次 RegOpenKeyExW+2×RegQueryValueExW = 17.3µs）+ HmacSha256Hex（每次 BCryptOpenAlgorithmProvider = 4.7µs）。UndoSelected（1021-1059）再全量验一遍。r6 U-6 **未修**，量级已实测：每条 ≈12-22µs。
- **修复草案**：secret 进程级一次读取（静态缓存）；VerifySignature 结果在 LoadAll 时算一次存入 OpRecord（渲染零加密）。
- **预期收益**：@142 −1.75ms；线性增长免疫（千条历史省 ~25ms）。

### U-10（Confirmed·复测）排序比较器 ToLower 堆分配未修：@1000 5.35ms（预计算 0.93ms，5.7×）、@5000 43ms（6.7ms）
- **代码**：Presenters.cpp:486-506（case 0 每比较最多 4 次 ToLower、case 2 每比较 2 次）。今日页规模 @181 实测 0.99ms（预计算 0.25ms）——LargeFiles 页排序无感；搜索页 @1000 排序点击 ≈5.4ms+RenderItems 6-8ms ≈12ms 勉强过帧预算。
- **修复草案**：snapshot_ 挂 lowerTitle/lowerCategory 预计算键（或排 uint32 索引）。S 工作量，r6 已给方案未实施。

### U-11（Confirmed·代码）窗口级重绘策略：CS_HREDRAW|CS_VREDRAW + 无 WS_CLIPCHILDREN + v2.23 整窗自绘擦除
- **代码**：MainWindow.cpp:336（wc.style）、350（CreateWindowExW 无 WS_CLIPCHILDREN）、465-483（WM_ERASEBKGND：每次 GetClientRect 整窗 + SPI + CreateSolidBrush + FillRect + DeleteObject）。交互拖尺寸时每个 WM_SIZE 步：父窗口全失效 → 整客户区擦除（含子控件下方）→ 全部子控件重绘（6 个 owner-draw 标签、30 行列表、info card、状态栏）+ OnSize→LayoutWindow≈20 次 SetWindowPos + DT_CALCRECT 测量。列表本身有 DOUBLEBUFFER 不闪，chrome 区会闪。
- **修复草案**：(a) 注册时加 WS_CLIPCHILDREN、去掉 CS_HREDRAW|CS_VREDRAW（改为 InvalidateRect 差分）；(b) chrome 刷静态化。
- **预期收益**：拖尺寸由"每步全窗重绘"降为增量；视觉更稳。

### U-12（Confirmed·微基准）切标签即同步写盘 1.69ms/次 + 双重布局
- **代码**：OnTabChanged 尾 SaveSettings（1672）→ Settings::Save（Settings.cpp:54-81：ofstream 写 .tmp + MoveFileExW，实测 1.69ms）。同路径：IDC_CHK_MATCHPATH(723)、IDC_CHK_ADVANCED(750)、WM_CLOSE(997)、OnChooseTarget(2478)、排除项(2423)。OnTabChanged 期间 OnSize 至少两次（1674 显式 + 各 ShowWindow 触发）。UpdateExecButton/OnScanDone 的 svc.Results(t) 全量拷贝仅为 empty() 判断（1827，@1000 ≈0.3ms）。
- **修复草案**：脏标记 + 500ms 合并定时器 + WM_CLOSE 兜底；Results 加 empty() 快捷接口。

### U-13（Confirmed 现象/归因 Speculative）8.7s 消息派发空隙：01:32:46.282 "Scan tab=2 done"（worker，锁内）→ 01:32:55.018 "OnScanDone"（UI）
- 前两次同型扫描间隔 1ms/10ms；第三次 8.7s。两者之间 worker 只做 SaveResultsCache(20 项 JSON+MoveFileExW) + Post。候选：磁盘当时被自身扫描/杀软占满导致 ofstream/MoveFileExW 停顿；或 UI 线程 8.7s 忙/硬缺页（工作集 ~900MB 时代换）。**无法从现有日志区分 worker 慢还是 UI 慢** —— 正是 §5 可观测性要解决的样本。
- **修复草案**：先上观测（泵计时 + SaveResultsCache 计时 span），再对症。

### U-14（Confirmed·代码+git）Ctrl+K 加速键失效（v2.22 回归）
- MainWindow.cpp:370-376：acc[] 现有 4 项（F5/Ctrl+A/**Ctrl+I**/Ctrl+K），`CreateAcceleratorTableW(acc, 3)` 只注册前 3 项。git 考证：484d774(v2.21a) 时 acc 为 3 项 count=3 正确；**d574af3(v2.22) 插入 Ctrl+I 未把 3 改 4** → Ctrl+K（命令面板）自 v2.22 起键盘不可达（cue banner/说明仍在宣传它）。与延迟无关但属交互回归，一行修复。

### U-15（Confirmed·代码+日志）快速筛选"Debug 文件夹"等 4 个预设永远 0 结果 —— 且它正是引发今日 49s 重建的导火索
- MainWindow.cpp:1119-1122 预设 token 为 `folder:debug` 等；但 ParseFilterTerms（VolumeIndex.cpp:988-991）只接受**整 token 等于** `folder:`（dirsOnly），`folder:debug` 落入自由文本 → 在**文件名**里找子串 "folder:debug" → 恒 0。今日日志：01:33:58 搜索 'ext:pdb folder:debug' → 0 命中 → 8 秒后用户点了"重建索引"（01:34:06 BuildFull）→ 49s walk。**功能 bug 直接制造了一次最大延迟事件**。
- **修复草案**：parser 支持 `folder:<名>`（目录名包含匹配）；或预设改为等价的 `*debug* folder:` 组合。

### U-16（Confirmed·代码）FolderTree 扫描 depth=2 双重全盘遍历（r6 遗留未修）
- FolderTreeScanner.cpp:69-149：level-1 每目录 `DirectorySizeParallel(dir,2)` 全子树，level-2 再对每个子目录全子树——level-2 的树是 level-1 已走过的子集（≈2× I/O）。实测整页 16.7s（01:56）。修复：单次遍历累加父子目录大小（自底向上聚合），或直接复用 VolumeIndex 的 walk 结果 + 按需精确定大小。

### 次级（不计编号，代码均亲读）
- **日志热路径**：RunSearch/OnScanDone 等 UI 线程 MS_LOG 每行 22.7µs（ofstream+flush+OutputDebugStringW，微基准）；每键 2 行 —— 常态可忽略，U-3 写盘/扫描占盘时被放大。
- **UpdateExecButton O(N)**：每次勾选翻转 CollectChecked 全表 ToLower+map（Presenters.cpp:410-420）@1000 ≈1ms；可增量维护计数。
- **RestoreViewState O(N×S)**（93-124）：全选+刷新时叠加几 ms；可哈希化。
- **OnIdleCheck 30s tick + 空闲倒计时 1s tick**：GetLastInputInfo/纯文本更新，<0.1ms，无问题。
- **PBM_SETMARQUEE 25ms**：仅扫描/索引期，进度条小面积重绘，无问题。
- **进度消息频率**：FastWalk 每 1024 文件、WalkBuild 每 20000 目录、JunkScanner 每目录 —— 每条 WM_APP_SCAN_PROGRESS → UpdateStatusBar（含 GetDiskFreeSpaceExW ≈39µs，r6 实测）≈ 0.1-0.2ms，19 次/s 级别，无问题。

---

## 3. 对任务书线索的逐条回应

| 线索 | 结论 | 证据 |
|---|---|---|
| "v2.17 起 asInvoker → USN 永远 Win32 5 → **任何触发刷新都会退化为全盘 walk**，假设周期性发生在空闲维护" | **部分成立，需拆开**：(1) USN 拒绝属实（今日 01:34:06.342 "cannot open volume C: (Win32 5)"）；(2) "**任何**刷新都退化"不成立——walk-built 缓存下 EnsureBuilt 直接 no-op（RefreshFromUsn:700），今日 01:29 启动载入缓存 5.5s 即可搜，无 walk；(3) **空闲维护对 walk-built 卷 = RefreshWalk 全量重走**（SessionService.cpp:458-469），条件：闲置≥5min+活动页数据龄 10-30min+15min 冷却，发生时 49s walk+267MB 写+4.7s Finalize——今日日志窗口内**未发生**（无第二次 walk 记录），但机制在位，属"周期性全盘 walk"的真实载体；(4) 今日 01:34 的 walk 是 **BuildFull 路径 = 手动"重建索引"**（RefreshWalk 不产生 "cannot open volume" 日志），诱因见 U-15（folder:debug 预设恒 0 命中） | VolumeIndex.cpp:327-333/495-502/699-700；SessionService.cpp:436-474；日志 1337/1358-1374 行 |
| "DrawInfoText 每帧打 INFO 日志，频率多高" | **不成立**：`static int loggedDraws; if (loggedDraws++ < 2)`（MainWindow.cpp:270-275）每进程只记前 2 次；今日两会话各恰好 2 条 | 代码 + 日志行数（2 条/会话）双证 |
| r6 遗留 B-4（冷启首搜 4.1s，Finalize DFS 惰性化） | **未实施**：TryLoadCache:1256 仍同步 Finalize；今日实测载入段 4.8s | U-1 |
| r6 遗留"键入→结果 470-520ms" | **已自然改善至 0.31-0.45s**：索引虽 3.4M，但 'ext:pdb' 满页 1000 后 pass1 提前截断（189-233ms 实测）+ 防抖不变 | U-2 |
| r6 遗留 FolderTree 双倍遍历 / SizePool 无取消 | **均未修**；且 v2.23 的 FolderChildrenAsync 让 SizePool 无取消成为交互级问题（U-4/U-16） | 亲读 PathUtils.cpp:174-283 |
| r6 遗留 内存 905MB→CSR/平铺 | 未实施（非本轮维度，旁证：dirPaths_+frnToDirPath_+childrenOf_ 仍全量驻留，Finalize 仍是 4.7s 的主因） | VolumeIndex.h:214-222 |
| "日志 01:31 tab=2 扫描 0/8/20 items in 24946/24241/9382 ms" | LargeFiles 页扫描 19-25s 属实（FastWalk 全盘+去重哈希）；"0 items in 24946ms" 为**中途取消**的形态（cancel → LargeFileScanner::Scan 直接 return 空 out，SaveResultsCache 跳过）——用户等不及取消的直接证据 | LargeFileScanner.cpp:178；RunScan SessionService.cpp:700-743 |
| "搜索 ext:pdb 命中 1000 项 0.1-0.25s" | 复核一致：189ms@3.09M / 233ms@3.43M / 96ms（带目录过滤提前跳过文件） | 日志 + VolumeIndex.cpp:1069-1121 |
| "02:00 二次启动正常" | 该会话**从未进入搜索页**（无 "loaded cache" 行），索引构建根本没触发；并非缓存变快。若进搜索页仍是 ~5s 载入 | 日志 1384-1388 行 |

---

## 4. 选中一个条目的时间线（搜索页 @1000 行，今日规模）

**单击选中一行**（无详情面板链，成本极低）：

| 阶段 | 代码 | 线程 | 耗时估计 |
|---|---|---|---|
| LVN_ITEMCHANGED（state 翻转但非 check） | MainWindow.cpp:509-525 checkFlipped=false → no-op | UI | <0.01ms |
| ListView 重绘新旧两行（含 2×复选框 320µs + zebra/SPI/UiScale ~50µs） | NM_CUSTOMDRAW | UI | ≈0.4ms |
| **合计** | | | **<1ms ✓**（无堆积：同步完成，无异步请求可堆积） |

**空格勾选一行**：

| 阶段 | 代码 | 线程 | 耗时 |
|---|---|---|---|
| WM_KEYDOWN Space（RunMessageLoop 拦截） | MainWindow.cpp:394-406 | UI | <0.01ms |
| ToggleCheckAt：map 写 + 单行 Redraw + UpdateWindow | Presenters.cpp:230-240 | UI | ≈0.35ms（1 行重绘含复选框） |
| LVN_ITEMCHANGED → UpdateExecButton → CollectChecked O(N) ToLower+map | Presenters.cpp:410-420 | UI | ≈1ms@1000 |
| 执行按钮文案 SetWindowText（owner-draw 重绘） | MainWindow.cpp:1974-2007 | UI | ≈0.2ms |
| **合计** | | | **≈1.5-2ms ✓**；Ctrl+A/反选 = O(N) map + 一次全表重绘 + U-6 全量复选框 ≈ 6-10ms ✓（60fps 内） |

**对比：一次键入搜索的时间线**（真正的延迟大头）：EN_CHANGE→(200ms 防抖)→RunSearch 22.7µs 日志→SearchAsync 排队→pass1 96-233ms→pass2+路径 ~3-5ms→1000 行属性补齐 20-60ms→StoreResults+节流缓存→Post→UI Refresh(拷贝 0.25+Render 6-8+RestoreViewState)→ShowHint→OnSize 全布局 ~1-2ms→**合计 ≈0.31-0.45s**。

---

## 5. 延迟预算表（现状@今日规模 → 目标 <100ms 命令 / <16ms 帧）

| # | 操作 | 现状（实测口径） | 目标 | 差距/责任 |
|---|---|---|---|---|
| 1 | 启动首帧 | **517-655ms**（日志） | <400ms | U-1（同步 JSON×5、图标、首写设置） |
| 2 | 启动→可搜索 | **5.7s**（日志） | <1.5s | U-1（fread 逐行+Finalize 3.7-4.8s） |
| 3 | 键入→列表 | **0.31-0.45s**（日志） | <0.12s | U-2（防抖 200ms+pass1 线性扫） |
| 4 | 滚动一档 | ≈0.5-1ms（3 新行×183µs+列表原生） | <16ms | ✓（U-6/U-7 再让 3-4× 裕量） |
| 5 | 整列表重绘（排序/刷新/改列宽） | ≈8-14ms@1000（Render 6-8 + 绘制 5.5） | <16ms | 临界 ✓；U-6 后 ≈4-9ms 稳过 |
| 6 | 排序点击 | ≈12ms@1000 / ≈50ms@5000 | <100ms | @1000 ✓；@5000 ✗ → U-10 |
| 7 | 切标签 | ≈10-14ms（Refresh 6-8 + SaveSettings 1.7 + 双 OnSize） | <50ms | ✓（U-12 锦上添花） |
| 8 | 勾选/全选/反选 | 1.5-2ms / 6-10ms | <100ms | ✓ |
| 9 | 右键菜单 / 命令面板弹出 | TrackPopupMenu/建窗 <5ms | <100ms | ✓（但 Ctrl+K 键盘入口坏：U-14） |
| 10 | 历史页激活 | **4.1ms@142**（微基准） | <16ms | ✓（千条时 ✗ → U-9） |
| 11 | 扫描期间切页/打字/排序 | **全禁用 19-25s**（日志） | 可浏览 | ✗✗ → U-5 |
| 12 | 文件夹展开（v2.23） | 秒~分钟级占满 worker，期间搜索被阻 | <200ms 出目录，大小异步 | ✗✗ → U-4 |
| 13 | 重建索引 | **54s**（49 walk+4.7 Finalize+写盘） | 后台化+可搜 | ✗ → U-3 |
| 14 | 空闲维护期回位打字 | 取消收敛 <1s（walk 逐目录查 cancel）+写尾 1-5s 无取消 | <300ms | 部分 ✗ → U-3(a) |
| 15 | 关闭 | 常规 <10ms；walk 写盘中 1-5s；FolderExpand 中=当前子树走完 | <300ms | ✗ 最坏 → U-3(a)+U-4(a) |
| 16 | 空闲 CPU | 30s tick + 1s 倒计时（仅armed）+ 25ms marquee（仅忙时） | ~0 | ✓ |

---

## 6. 卡顿可观测性方案（r6 §4 未落地，本轮再次给出，含新注入点）

1. **泵计时**：RunMessageLoop（MainWindow.cpp:378）GetMessageW→DispatchMessageW 前后 QPC，>50ms 记 `{msg, wp, ms}` 一条 WARN——今天的 8.7s 空隙（U-13）会立刻被这类行捕获并归到具体消息。
2. **worker 阶段 span**（一行改动级，先于一切）：EnsureBuilt 内 TryLoadCache/RefreshFromUsn/WalkBuild/SaveCacheIfWorthwhile/Finalize、SearchAsync 的 pass1/size-fetch、FolderChildrenAsync 每目录、SaveResultsCache 各记 `phase=xxx ms=yyy`。今天报告里 4.8s 载入与 4.7s walk 尾都是人肉从两条日志差值算出的。
3. **UI stall watchdog**：独立线程 250ms PostMessage(WM_APP_WATCHDOG_BEAT)，WndProc 首行原子回执；连续 2 拍未回执 → SuspendThread+CaptureStackBackTrace（复用 platform/CrashDump.cpp）写 `logs/stalls.log`，10 次/小时上限。
4. **启动打点**：OnCreate 末/首帧（DrawInfoText 首绘）各一条含 ms 的 INFO——首帧 517-655ms 目前靠日志差值。
5. **绘制计数器（debug 构建）**：NM_CUSTOMDRAW ITEMPREPAINT 每帧行数与耗时累计，校准 U-6 修复收益。
6. 开销预算：泵计时 ~50ns/消息；心跳 4 msg/s；抓栈仅事故态。

---

## 7. 修复顺序建议（收益/成本，S<半天 M<3天 L>3天）

| 序 | 项 | 规模 | 预期收益 |
|---|---|---|---|
| 1 | U-5 扫描期仅锁 exec/scan，列表/切页/排序保持只读浏览 | S-M | **"丝滑"最大单项**：25s 锁定→0 |
| 2 | U-4 FolderExpand：cancel 贯穿 SizePool + 浅层先行/深大小异步 | M | 展开即刻响应；搜索不再被劫持；关闭尾巴消除 |
| 3 | U-1 B-4 惰性化 + fread 整块读 + 非活动 tab 缓存懒加载 | M | 启动→可搜索 5.7→~1.5s |
| 4 | U-3 写循环取消点 + 块缓冲写 | S-M | 写尾 1-5s→<0.5s，可中断 |
| 5 | U-2 防抖 200→100ms + 大小补齐分批 | S | 键入延迟 −100~150ms |
| 6 | §6-1/2 泵计时+阶段 span（先于其余大改，校准一切） | S | 推算→实测 |
| 7 | U-6 复选框位图缓存 + U-7 UiScale 静态缓存 + U-8 HC/刷缓存 | S（合计） | 整列表重绘 −5ms，滚动 3-4× 裕量 |
| 8 | U-15 folder: 预设解析 + U-14 加速键 count=4 | S | 消灭"0 结果→用户点重建→54s"链 |
| 9 | U-10 排序键预计算、U-9 验签缓存、U-12 SaveSettings 节流 | 各 S | 各 1-25ms，规模免疫 |
| 10 | U-11 WS_CLIPCHILDREN/去 CS_HREDRAW + U-16 FolderTree 单遍聚合 | M | 拖尺寸增量重绘；树扫描 2×→1× |

---

## 8. 被推翻的怀疑 / 已验证无问题

| 怀疑 | 结论 | 证据 |
|---|---|---|
| DrawInfoText 每次绘制打日志 | **不成立**：每进程仅前 2 次 | MainWindow.cpp:270-275 + 日志每会话恰 2 条 |
| 任何刷新都退化为全盘 walk（含普通启动） | **不成立**：walk-built 缓存下 EnsureBuilt no-op；今日 01:29 启动无 walk | VolumeIndex.cpp:699-700 + 日志 1337 行后无 walk 直至 01:34 手动重建 |
| 空闲维护今日周期性全盘 walk | **今日未发生**（日志无第二场 walk/无 RefreshWalk 记录）；机制上成立（walk-built 卷的维护=全量重走），条件见 U-3 | SessionService.cpp:458-469 + 日志全文 |
| 每键 spawn 线程风暴（r6-03 曾证伪） | 维持**无问题**：CAS 门控 + 80ms 重试 + pool≤6、n<64 退 1（代码未变） | SessionService.cpp:327-359/636-664 |
| 快速键入导致结果错乱/堆积 | 无问题：六种 DONE 全带 gen 门槛；防抖每键重置 | MainWindow.cpp:829-930；SessionService.cpp:71-85 |
| 勾选/批量勾选消息风暴 | 无问题：map 权威 + 单次 RedrawItems（v2.22 反选同语义，实测代码一致） | Presenters.cpp:230-264 |
| 排序比较器是当前主要卡顿源 | **降级**：@今日页规模（181-1000 行）排序 1-5.4ms 可过帧预算；仅在缓存上限 5000 行时超标（43ms） | 微基准复测 |
| 历史页激活是当前卡顿源 | **基本无感**：@142 条全链 4.1ms（LoadAll 2.38 + 验签 1.75）；r6 的"93 条 10-15ms"偏保守 | 微基准 |
| 命令面板有重活 | 无问题：~20 条命令 ToLower 过滤 <0.1ms；仅 Ctrl+K 入口回归（U-14） | CommandPalette.cpp:25-42 |
| WM_APP_SCAN_PROGRESS×UpdateStatusBar 拖累 | 无问题：进度源都自带节流（1024 文件/20000 目录/每目录），每条 ≈0.1-0.2ms | FastWalk.cpp:131-134 等 + r6 复测值 |
| Logger 每键 2 次刷盘是显著延迟 | 常态不成立（22.7µs/行 微基准）；仅在 U-3 写盘并发时被放大 | 微基准 |
| FolderTree 缓存导致慢 | 无问题（今日两会话各 ~0.4s 内完成 5 个 JSON 载入+丢弃判断） | 日志 1335-1336/1385 行 |

---

## 9. 未验证 / 局限

1. **无活体 GUI 计时**：未运行 MiniSys.exe（约束）。所有 UI 侧耗时（首帧、落屏、绘制帧率）来自日志时间戳代理与微基准外推；§6 落地后可全部转实测。
2. **8.7s 派发空隙（U-13）无法归因**：现有日志无 worker/UI 分侧计时；两候选（SaveResultsCache 的 MoveFileExW vs UI 线程停顿）都无直接证据。
3. **FolderExpand 展开耗时无实测**（今日日志无展开事件）；16-50s 估计由 FolderTree 扫描 16.7s/全盘 walk 49s 类比。
4. **Finalize 与 save 的 4.7s 拆分**靠 r6 冷启测量（3.72s Finalize）外推；本轮无单独计时。
5. 微基准的 DrawFrameControl 数值取自内存 DC（无真实 uxtheme 主题上下文），绝对值可能有 ±30% 浮动，量级（百微秒级）可信；复选框位图化的收益区间据此保守表述。
6. 历史规模以文件实测 142 行为准（任务书"约 330 条"未获文件佐证——若按 330 外推：激活 ≈8-9ms，仍过预算）。
7. 版本归属：exe 哈希 `60ccce52…` 与 mtime 2026-10-15 02:15 匹配 v2.23 源（6621d92 02:16 提交、src 无未提交改动）；未做运行中进程模块校验（应用未运行）。
8. 键入→结果 0.31-0.45s 中 UI 落屏 8-12ms 为 r6 lvbench 外推（代码路径未变）；worker 段 96-233ms 为今日日志实测。
9. **数据完整性备注（给主审/后续子代理）**：评审期间（02:29:26）用户真实 history.jsonl 被追加了一条测试记录 `{"id":"20261029-022926-0","source":"X:\\TOCTOU\\swapme",…QUARANTINE…}`（文件 39450→39733 B）。溯源：这是并行评审实验 `.tmp-review/r7/exp/gate7.cpp`（QuarantineOp swapme 用例）写入的，**非本报告的 r7bench**（本 harness 只调 LoadAll/VerifySignature，历史路径第一行即重定向到 scratch 副本，且二进制不含 QuarantineOp）。本报告的 142 行基线取自该时间点之前；后续以 history 行数为基数的分析需剔除这条污染。
