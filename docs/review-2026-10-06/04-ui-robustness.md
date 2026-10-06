# MiniSys UI 稳定性审查报告(崩溃风险专项)

> 审查人:UI 稳定性审查工程师 · 日期:2026-09-30
> 对象:HEAD=ae3ce19(v2.0)+ 工作树未提交 v2.1 UI 变更
> 范围:`MiniSys/src/MainWindow.*`、`MiniSys/src/ui/*`、`res/resource.h`、`res/MiniSys.rc`、`app.ico`、vcxproj,以及 UI 直接依赖的 `core/SessionService.*`、`core/PlanBuilder.*`、`core/GuardRails.*`、`core/OperationLog.*`、`util/StringUtils.*` 的线程/生命周期契约
> 方法:纯代码审计(未运行任何程序/构建/测试;dump 目录不存在,与"无崩溃转储"陈述一致)

---

## 0. 版本归属核对(提交与时间戳)

- 仓库 HEAD = `ae3ce19`(git log 确认,非任务简报所写 efd9e63;efd9e63 是上一个提交)。
- 工作树未提交 v2.1 变更文件时间戳:Icons.cpp 23:59、resource.h/MiniSys.rc 23:59、Presenters.cpp 00:02、MainWindow.cpp 00:04。
- `build/Release/MiniSys.exe` mtime = 09-30 00:05(438KB,PDB 同刻),二进制内含 UTF-16 字符串 "v2.1" 与 "TaskDialog"(grep 命中)→ **v2.1 已成功编译链接**,任务简报"从未被构建"不成立;但 GUI 从未运行过该二进制:
  - `%LOCALAPPDATA%\MiniSys\logs\minisys.log` 最后一条 `MiniSys starting; elevated=1` 在 09-29 23:48:29(旧 v2.0 exe,build/Release 22:40 版),之后 00:04:44 与 00:05:54 两批日志(tab=0..3 快速扫描 + `Scanner threw: boom`)与 `MiniSysTests.exe`(mtime 00:02,SessionServiceTests.cpp:55-60 的 ThrowingScanner 消息即 "boom")吻合 → 是**单元测试进程写入**,非 GUI 会话。
- 日志中 16 条 `[ERROR]` 全部为 "Scanner threw: boom"(测试注入),**无产品故障 ERROR**;`no dumps dir` 确认无崩溃转储。
- 附带发现(核心层,超出 UI 范围仅记录):每次启动 `rules.json parse failed: bad number at offset 14`(rules.json `"version": 2` 整数解析失败,回退内置规则),以及 23:48 会话 `FSCTL_ENUM_USN_DATA failed (Win32 87)` → 索引回退 FastWalk( Junk 57s / LargeFiles 69s)。

---

## 1. 结论:审计线索证实/证伪表

| # | 线索(任务给定) | 结论 | 关键证据 |
|---|---|---|---|
| 1 | OnTabChanged 内调用 OnSize 布局重入 | **证伪** | MainWindow.cpp:295 OnSize→LayoutWindow 仅向子控件发消息,不回调主 WndProc;OnCreate(208)调用时 presenters_ 已于 192-196 就绪 |
| 2 | WM_APP_OP_DONE 在 OnPlanDone 弹窗时排队重入 | **证伪** | RunPlan 尾部先写 lastReport_(SessionService.cpp:229-232)后 Post(235);worker 仅剩 CoUninitialize+taskKind_.store;模态期间主窗口被 MessageBox 禁用,无法启动新任务 |
| 3 | OnTreeViewRClick 的 itemPaths_ 迭代器在 Refresh 后失效(it 在 TrackPopupMenu 后仍被使用/erase) | **证伪(当前版本)** | Presenters.cpp:267 `folderPath` 为拷贝;`it`/`hItem` 在 TrackPopupMenu(274)与 MessageBox(281)返回后不再使用;代码中无 erase;MessageBox 后按**路径**重查 ftItems(286-288)而非索引 |
| 4 | worker 在窗口销毁后 Post 到无效 HWND | **证实机制/无崩溃后果** | SessionService.cpp:42-44 Post 失败静默忽略;销毁后无消费者 |
| 5 | lastReport_ 的锁 | **有锁但模式脆弱** | SessionService.cpp:51-54 持锁返回 const 引用,锁随即释放;当前时序安全(写先于 Post、模态禁用主窗口),见 R-6 |
| 6 | Presenters 持 UiHandles 副本(HWND 悬垂) | **证伪** | 控件存活至 DestroyWindow;之后消息泵已退出,无 presenter 代码运行;presenters_ 随 MainWindow 在 wWinMain 末尾析构 |
| 7 | lParam=results 索引 与 RunPlan 移除时序 / results_ 跨线程读写契约 | **证实,本报告最高风险** | SessionService.h:102 `results_` 无任何锁;详见 R-1 |
| 8 | OnOpenLocation/OnExecute 的 lParam 越界防护 | **静态防护齐全,动态竞争未防护** | MainWindow.cpp:537、387、PlanBuilder.cpp:48 均有 `idx >= items.size()`;但检查与访问之间 worker 可缩表,见 R-1 |
| 9 | CreateWindow 返回值检查缺失 | **证实缺失/不构成崩溃** | Controls.cpp 全文 24 处 CreateWindowExW 无检查;后续调用全是 SendMessage 族(null HWND 安全失败),见 R-8 |
| 10 | LoadImage(IDI_APPICON) 失败后果 | **已处理** | 失败仅 hIcon=NULL(默认图标);app.ico 实际有效(魔数 `00 00 01 00`、6 个图像、23KB),且 v2.1 已编译进 exe |
| 11 | TaskDialogIndirect 可用性(comctl32 v6) | **已保证** | MainWindow.cpp:25-27 manifestdependency pragma 固定 v6;vcxproj 链接 comctl32.lib;主线程 STA(wWinMain CoInitializeEx) |
| 12 | SHGetStockIconInfo 失败路径 | **已处理** | Icons.cpp:15-19 FAILED/空 hIcon 双检查;AddIcon<0 与 BCM_SETIMAGELIST==0 均清理返回 false(按钮退化为纯文本) |
| 13 | COM 初始化 | **正确** | 主线程 STA(main.cpp:10);worker 每次 StartTask CoInitializeEx STA(SessionService.cpp:73-74);SHFileOperation/IFileOperation 均带 FOF_SILENT/FOF_NO_UI(无 shell 弹窗卡无泵线程) |
| 14 | /utf-8 与中文文案 | **已处理** | vcxproj Debug/Release 均 `/utf-8`(54/80 行);.rc 全 ASCII(LC_ALL=C grep 无非 ASCII 字节),BOM 移除无编码风险 |
| 15 | FormatW 格式串与参数匹配 | **逐一核对,全部匹配** | 见 §7 安全清单;vswprintf_s 失配返回错误而非越界(StringUtils.cpp:62-74) |
| 16 | ShowHint 临时 wstring 生命周期 | **证伪(无缺陷)** | 所有 `.c_str()` 临时均在完整表达式内消费;OnEmptyQuarantine 的三元分支(MainWindow.cpp:485)临时存活至 FormatW 返回,是安全写法(常见误报点) |
| 17 | OnEmptyQuarantine 确认框期间 OP 重入 | **证伪** | 无任务运行时才可达;确认框期间主窗口禁用 |
| 18 | FolderTree 右键确认框期间 OP_DONE→Refresh 重建树→hItem 悬垂 | **证伪(当前版本)** | 同 #3;MessageBox 返回后不触碰 hItem/it |
| 19 | 关闭路径 worker join 前长 IO(DISM 运行中关窗) | **证实** | DelegateOp::Execute `WaitForSingleObject(pi.hProcess, INFINITE)` 不检查 cancelScan_;见 R-4 |
| 20 | 图标 ImageList 生命周期(借用后未销毁) | **证实为有界有意泄漏** | Icons.cpp:37-43 注释明示;7 个 16x16 单图 ImageList,进程退出回收,无崩溃 |
| 21 | ListView 列点击排序与 RefreshListView 交互 | **证实为 R-1 的触发器** | 执行期间列点击/排序按钮未禁用 |
| 22 | v2.1 从未被任何测试或人工运行验证 | **部分证伪** | 已编译成功且 54/54 单测跑过(00:05:54,测试进程);**GUI 确实从未运行过 v2.1 二进制**(最后 GUI 会话 23:48 为 v2.0 exe) |

---

## 2. 摘要:按崩溃可能性排序的 Top 风险

1. **R-1(P0)`results_` 完全无锁的跨线程读写**——计划执行期间(迁移/DISM 可达分钟级)排序按钮、列头点击、Tab 切换、"打开位置"均未被禁用,与 worker 对同一 `std::vector<ScanItem>` 并发读写:撕裂 `std::wstring` 读、vector 尾部收缩时 UI 迭代旧缓冲 → 野指针/UAF/堆损坏。这是唯一具备"稳定复现路径"的崩溃级缺陷。
2. **R-2(P1)执行确认框模态泵内 SCAN_DONE 重入 → 旧勾选索引 × 新扫描数据**——错位隔离/迁移用户未确认的项目(数据安全事故,不崩溃但比崩溃严重)。
3. **R-3(P1)CurrentTab() 无负值防护**——`presenters_[(size_t)-1]` 越界;当前流程不可达,属埋雷。
4. **R-4/R-5(P2)两条挂起路径**:DISM 中关窗 → 无窗口挂起数分钟;历史页撤销迁移 → UI 线程同步 GB 级拷回 → 假死 → 用户强杀 → 中断态。
5. R-6~R-10(P2/P3)模式脆弱与体验问题,不直接崩溃。

v2.1 未提交变更本身(见 §5)**未发现崩溃级缺陷**;真正的崩溃风险在 v2.0 已有的线程契约上,v2.1 只是把部分 MessageBox 改为非模态(方向正确,缩小了重入面)。

---

## 3. 发现清单

### R-1(P0)results_ 跨线程无锁读写:执行期间 UI 交互可致撕裂读 / UAF 崩溃

- **位置**:
  - `core/SessionService.h:102` `std::vector<std::vector<ScanItem>> results_;`(无 mutex/atomic 包裹)
  - `core/SessionService.cpp:56-62` `Results()/MutableResults()` 返回裸引用
  - `core/SessionService.cpp:143-227` `RunPlan` 在 worker 线程持续读 `items[pi.itemIdx]`(167-168 读 `si.title` 进 FormatW、178 读 `si.command`、188 传 `si` 给 GuardRails),尾部 `items = std::move(kept)`(226)销毁旧缓冲
  - `MainWindow.cpp:414-421` OnExecute Started 分支只禁 `h_.exec/h_.undo/h_.scan` 三个按钮
- **触发条件**(执行期间以下入口全部仍可用,且读写同一 vector):
  | UI 入口 | 代码 | 操作类型 |
  |---|---|---|
  | "按大小/时间排序"按钮 | MainWindow.cpp:145-150 → Presenters.cpp:90-108 `std::stable_sort(items,...)` | **UI 写** |
  | ListView 列头点击 | MainWindow.cpp:120-122 → OnColumnClick → 同上 | **UI 写** |
  | Tab 切换 | MainWindow.cpp:228 → Refresh → RenderItems(Presenters.cpp:40-56 迭代读 `it.category/title/detail`) | UI 读 |
  | "打开所在位置" | MainWindow.cpp:528-541 `idx >= items.size()` 检查后 `items[idx].path` | UI 读(检查-使用窗口) |
  | 再次"执行" | ExecutePlan → IsBusy 拦截(安全) | — |
- **后果**:
  1. worker 读 `si.title.c_str()`(FormatW 进度)时 UI stable_sort 移动该元素 → moved-from wstring 内部指针被并发解引用 → 野指针读崩溃;
  2. worker 尾部 `items = std::move(kept)` 释放旧缓冲瞬间,UI 正在 RenderItems 迭代 → use-after-free / 堆损坏(随机的后续崩溃,难归因);
  3. OnOpenLocation 的 size 检查与 `items[idx]` 之间 worker 缩表 → OOB 下标。
  - 附带:排序重排后 `pi.itemIdx` 与实际元素错位——经核对 `RunPlan` 所有破坏性操作均使用 **plan 冻结的 `pi.path`**(202 行 QuarantineOp、197 行迁移目标派生)而非 `si.path`,且 GuardRails::Validate(GuardRails.cpp:92)用 `pi.path` 查保护名单,故错位**不会**导致错误文件被删,只造成 size/策略字段错用 + 上述撕裂读。数据破坏风险由 R-2 承担。
- **修复草案**:
  1. 立即缓解(一行级):OnExecute Started 分支追加禁用 `h_.btnSortSize/h_.btnSortTime/h_.open/h_.tab` + `EnableWindow(h_.list, FALSE)`;OnPlanDone 恢复。注意现有 4 处按钮态管理分散(OnScan/OnExecute/OnPlanDone/OnEmptyQuarantine 各自为政),应收敛为单一 `SetTaskBusy(bool)`。
  2. 根治(契约):`results_` 改为 UI 私有快照——SessionService 完成时(Post DONE 前锁内)把该 Tab 结果**拷贝**成 `pendingResults_`,UI 处理 DONE 时 move 进 presenter 自持 `snapshot_`;排序只排 snapshot,不触碰服务端;`RunPlan` 收到的是 plan 自带的 const 副本。lParam → snapshot 索引天然一致,R-2/R-8 的索引类问题一并消失。退而求其次:`results_` 加 `std::shared_mutex`,`Results()` 改为 `ResultsSnapshot(tab)` 锁内拷出返回值。
- **置信度**:Confirmed(代码机制全部读到);崩溃复现概率 Likely(需用户在执行期间操作列表;迁移大应用/DISM 窗口长达分钟级,窗口不小)。

### R-2(P1)执行确认 MessageBox 期间 SCAN_DONE 重入:旧勾选索引作用于新数据 → 错位执行

- **位置**:`MainWindow.cpp:367-413`——379 `lp->CollectChecked()` 与 399 确认框之间,`selected`(旧列表索引)跨模态泵存活;`OnScanDone`(345-358)在模态泵内被分发,`Refresh()` 重置勾选并可能已用新扫描替换 `results_[t]`;413 `PlanBuilder::Build(t, items, selected, ...)` 用旧索引 × 新列表。
- **触发**:扫描进行中点击"执行选中操作"(执行按钮在扫描期间**未**被禁用——OnScan:339 只禁 `h_.scan`)→ 57-69 秒扫描在确认框打开期间完成 → 用户点"确定"。
- **后果**:planHash 对新列表自洽计算,`PlanMatches` 必然通过 → **隔离/迁移用户从未确认的项目**。GuardRails::Validate 只复验 `pi.path` 自身的存在性/大小/mtime,无法发现"索引错位"(pi.path 本身就取自错位后的新元素,自洽)。这是数据安全事故级缺陷。
- **修复草案**:
  1. `svc.IsBusy()` 时禁用执行按钮(OnScan 中一并 `EnableWindow(h_.exec, FALSE)`),从入口掐断;
  2. 纵深:CollectChecked 时同步记录路径集合,确认框返回后比对当前 `items` 路径集合,不一致则 ShowHint"扫描结果已变化"并放弃(FolderTree 右键流程的按路径重查模式 Presenters.cpp:284-289 是正确范本,推广到 ListTab)。
- **置信度**:Confirmed(代码路径完整);触发概率 Medium(扫描长、执行按钮可点)。

### R-3(P1)CurrentTab() 无负值防护:TabCtrl_GetCurSel==-1 时 presenters_[(size_t)-1]

- **位置**:`MainWindow.cpp:218-226`——`if (t < TabId::Count)` 不排除负数;`TabId` 底层 int,`static_cast<TabId>(-1) < TabId::Count` 为 true → `presenters_[static_cast<size_t>(-1)]` → 巨大偏移读 unique_ptr → 解引用崩溃。
- **触发**:tab 控件零项目时 GetCurSel 返回 -1。当前流程 OnCreate 先插 5 个 tab(183-187)再 OnTabChanged(208),不可达;若未来调整顺序、或 TabCtrl_InsertItem 全部失败(极端内存压力)即引爆。
- **修复草案**:`if (t < TabId::Junk || t >= TabId::Count) return nullptr;`(一行)。
- **置信度**:Confirmed(缺陷代码);触发 Speculative(当前不可达)。

### R-4(P2)DISM 委派执行中关窗:窗口已毁、进程无 UI 挂起数分钟

- **位置**:`core/DelegateOp.cpp` Execute 主循环 `WaitForSingleObject(pi.hProcess, INFINITE)`(不检查 cancelScan_);`SessionService.cpp:316-319` Shutdown join;`MainWindow.cpp:84-92` RunMessageLoop 退出后才 Shutdown。
- **触发**:勾选 WinSxS/hiberfil(Advanced)执行 → DISM 运行中点关闭。
- **后果**:WM_DESTROY→PostQuitMessage→循环退出→Shutdown join 阻塞至子进程退出;期间无窗口无响应,用户极可能任务管理器强杀 → 委派计划中断(DelegateOp 无逐条原子性,历史无 Interrupted 记录路径)。
- **修复草案**:DelegateOp 改 `WaitForSingleObject(pi.hProcess, 500)` 轮询 + `cancelScan_` 检查退出;或 WM_CLOSE 时 `IsBusy()` 则提示"任务运行中"并最小化等待/提供取消(顺带解决 R-003 无取消入口)。
- **置信度**:Confirmed。

### R-5(P2)历史页撤销在 UI 线程同步执行 GB 级拷回:界面假死 → 强杀 → 中断态

- **位置**:`ui/Presenters.cpp:337-367` UndoSelected → `svc_.UndoRecord`(SessionService.cpp:298-314,同步)→ `MoveJunctionOp::Undo`(MoveJunctionOp.cpp:213-236,`ShCopyDirectory` 全量拷贝回源)。
- **后果**:撤销一次迁移 = 删除 junction + 全量复制目标目录(GB 级,分钟级)+ 删目标,全程 UI 线程无泵 → "(未响应)" → 用户强杀 → 停在"junction 已删、拷回一半"(Interrupted 无 UI 恢复入口,仅历史 note 提示人工处置,ARCHITECTURE R-005 的残留)。
- **修复草案**:UndoRecord 与 EmptyQuarantine 同路径——投递 SessionService worker 队列(新增 TaskKind::Undoing),完成 Post WM_APP_OP_DONE 刷新历史列表。
- **置信度**:Confirmed。

### R-6(P2)LastReport() 返回解锁后的引用:靠时序而非锁保护

- **位置**:`SessionService.cpp:51-54`;`MainWindow.cpp:441` `const auto& rpt = svc.LastReport();` 引用跨 MessageBox 模态泵存活。
- **现状安全**:worker 写 lastReport_ 先于 Post(OP_DONE)(229-235);模态期间主窗口禁用无法启动新任务。任何未来改动(如非模态完成通知、允许模态中排队新任务)都会把它变成真实竞争。
- **修复草案**:`ExecuteReport LastReport() const`(值返回,结构小)。
- **置信度**:Confirmed(模式脆弱)/当前无触发路径。

### R-7(P2)OnScanDone 用 CurrentTab() 而非扫描发起 Tab:切 Tab 后误报

- **位置**:`MainWindow.cpp:352-356`——`auto t = CurrentTab();` 扫描 A 期间切到 B,A 完成时 `svc.Results(B).empty()` → 在 B 页显示"✓ 未发现可处理项,系统状况良好"(误导性文案);`ActivePresenter()->OnScanDone()` 刷新的也是 B。
- **修复草案**:SessionService 记录 `scanTab_`(RunScan 已持有 tab,加 atomic 成员暴露),OnScanDone 用之;不匹配则只更新状态栏。
- **置信度**:Confirmed(逻辑缺陷,无崩溃)。

### R-8(P3)CreateControls 24 处 CreateWindowExW 无返回值检查;两处未初始化 RECT 被读

- **位置**:`ui/Controls.cpp` 全文;`ui/Layout.cpp:10-11` `RECT srect; GetClientRect(ui.status,&srect)`(status 为 NULL 时 srect 未初始化即被读);`MainWindow.cpp:300` 同型。
- **后果**:控件创建失败时全部后续调用是 SendMessage 族(null HWND 安全失败),仅 UI 降级不崩溃;未初始化 RECT 产生垃圾布局值(MSVC /W4 C4700 级),仍不崩溃。
- **修复草案**:创建后统一断言/日志;`RECT rc{};` 零初始化。
- **置信度**:Confirmed(缺失)/崩溃 Speculative。

### R-9(P3)UpdateStatusBar 每条进度消息做磁盘查询 + 全量读解析 history.jsonl

- **位置**:`MainWindow.cpp:298-326`——`QuarantineUsageText()`(SessionService.cpp:284-296 → OperationLog::LoadAll 读文件)在 `migrateTargetRoot_` 为空时(常态)每次调用;WM_APP_SCAN_PROGRESS/OP_PROGRESS 均触发(160-163)。LargeFileScanner 每 1024 个文件发一条(LargeFileScanner.cpp:137-143 节流),RunPlan 每项 1-2 条。
- **后果**:百万文件扫描 ≈ 千次文件读+JSON 解析在 UI 线程;历史增长后迟滞放大。无崩溃。
- **修复草案**:隔离区占用文案缓存,仅在 OP_DONE/EmptyQuarantine 完成时刷新。
- **置信度**:Confirmed。

### R-10(P3)杂项(均 Confirmed,非崩溃)

- `SessionService.cpp:266-271` RunEmptyQuarantine 循环内 `UpdateStatus` 每次全量重写历史文件,O(n²);建议批量化后一次重写。
- `ui/Presenters.cpp:54` RenderItems 重渲染只恢复 `recommended` 勾选 → 排序/切 Tab 丢失用户手动勾选(向安全方向偏差,但与用户预期不符)。
- `ui/Icons.cpp:37-43` 7 个 ImageList 有意泄漏(注释明示,进程退出回收)——量级可忽略,建议 WM_DESTROY 时统一销毁以正名。
- `MainWindow.cpp:84-92` `GetMessageW` 返回 -1(错误)时 `msg.wParam` 读取未初始化 MSG——实践中不可达,建议 `while (GetMessageW(...) > 0)` 外再判 -1。
- `MainWindow.cpp:63-66` hIconSm 固定 16x16,高 DPI 下小图标应取 `GetSystemMetrics(SM_CXSMICON)`(仅视觉)。

### R-11(移交核心审查,仅记录现象)

`rules.json parse failed: bad number at offset 14` 每次启动两条 WARM(日志 16 处);文件头部 `"version": 2`,offset 14 恰在数字附近 → 自研 `util/Json` 对整数值的解析缺陷,Junk 规则表回退内置默认(有回退设计,不崩溃)。根因在 `util/Json.cpp`,超出 UI 范围未深查。

---

## 4. v2.1 未提交变更逐文件复核

| 文件 | 变更 | 结论 |
|---|---|---|
| `MiniSys.vcxproj`(+2) | Icons.cpp/h 纳入编译 | **未发现缺陷**;与 00:05 构建成功事实一致 |
| `MainWindow.cpp`(+156/-58 区段) | LoadImageW×2 图标 / IDC_BTN_ABOUT 路由 / SetStockButtonIcon×7 / OnAbout TaskDialog / UpdateStatusBar 三分栏 / ShowHint 非模态化 / OnPlanDone 弹窗条件化 | **未发现崩溃级缺陷**。逐项:LoadImage 失败仅 NULL 默认图标;TaskDialogIndirect 由 v6 manifest 保证(pszMainIcon=MAKEINTRESOURCEW(IDI_APPICON) 且 hInstance 已设,资源已在 rc 中);三分栏 `edges[3]={e0,e1,-1}` 为惯用法,W 极小时 {0,0,-1} 无害,窄窗(>W-160)已降级为比例分;OnAbout 返回值未查(失败仅 pressed=0,无后果);**MessageBox→ShowHint 的非模态化缩小了模态泵重入面,对稳定性是正向改动**;执行/扫描按钮态与 R-1/R-2 相关的缺口属 v2.0 既有,非本次引入 |
| `MainWindow.h`(+2) | OnAbout/ShowHint 声明 | 未发现缺陷 |
| `MiniSys.rc` | BOM 移除 / `IDI_APPICON ICON "app.ico"` / 版本 2.1 | **未发现缺陷**。app.ico 魔数有效(6 图像);rc 现为纯 ASCII(BOM 移除无编码风险);"app.ico" 相对 rc 所在目录解析正确;已验证编译进 exe |
| `resource.h`(+1) | IDC_BTN_ABOUT 2019 | 未发现缺陷;无 ID 冲突 |
| `Controls.cpp` | GRIDLINES→DOUBLEBUFFER / SS_NOPREFIX / about 按钮 | **未发现缺陷**;DOUBLEBUFFER 与 LVS_EX_CHECKBOXES 兼容;SS_NOPREFIX 防止 `&` 转义 |
| `Layout.cpp` | about 定位 W-84 / info 高 36 / contentY+16 | 未发现缺陷;W<84 时负坐标无害;两行静态文本 36px 足够 |
| `Presenters.cpp` | 3 处 MessageBox→SetWindowTextW | **未发现缺陷**;`(L"✓ 已撤销: " + r.source).c_str()` 临时存活至调用返回,安全 |
| `UiHandles.h`(+1) | about 句柄 | 未发现缺陷 |
| `Icons.cpp/.h`(新) | SHGetStockIconInfo+BUTTON_IMAGELIST | **未发现崩溃级缺陷**。失败路径完备:FAILED/空 hIcon→false;ImageList_AddIcon<0→DestroyIcon+Destroy;BCM_SETIMAGELIST==0→Destroy;DestroyIcon 后 imagelist 持副本正确;高 DPI 小图标(20/24px)入 16x16 列表会 AddIcon 失败→优雅降级纯文本;imagelist 借用不销毁(有意,见 R-10) |
| `app.ico`(新) | 应用图标 | 未发现缺陷;有效 ICO(23KB/6 图像,含 16/24/32/48/256) |

---

## 5. 系统性加固方案

### 5.1 results_ 读写契约(R-1 根治)

**单一写者 + UI 快照**:
- worker 是 `results_` 唯一写者;写入仅发生在任务尾(Post 之前)。
- UI 永不直接持有跨消息的引用:SessionService 增加 `std::vector<ScanItem> TakeResults(TabId)`(锁内 move 出,或拷贝),`WM_APP_*_DONE` 处理器一次性取走,存入 presenter 自持 `snapshot_`。
- 排序只作用于 `snapshot_`(纯 UI 状态);`RunPlan` 消费 plan 内冻结的 `PlanItem`(现状已冻结 path/size/mtime,保持)。
- 备选(改动最小):`results_` 加 `std::shared_mutex`,`Results()` 改锁内拷出返回值;`MutableResults()` 仅 worker 可调(注释+断言约束)。

### 5.2 模态重入守卫(推广 FolderTree 的正确模式)

- FolderTree 右键流程已是范本:**跨模态泵只允许持有值拷贝(路径),返回后按值重查,不持索引/句柄/迭代器**。将此立为 UI 层规约(注释进 Presenters.h)。
- ListTab 流程补齐:CollectChecked 返回 `{idx, path}` 对;MessageBox 返回后先比对路径集合与当前列表,失配即放弃并提示重扫(顺带修复 R-2)。
- 通用守卫:MainWindow 增加 `taskGeneration_`(每次 WM_APP_*_DONE 自增);任何"打开模态前记 g,关闭后 g 不等则丢弃后续按索引的动作"。

### 5.3 任务态按钮矩阵(收敛散落禁用)

新增 `void SetTaskBusy(bool busy)`:统一禁/启用 `{scan, exec, undo, emptyQ, open, btnSortSize, btnSortTime, tab, list}`(list 禁用即封列点击与勾选)。OnScan/OnExecute/OnPlanDone/OnEmptyQuarantine 四处调用点替换现有逐个 EnableWindow——消除"漏禁一个控件 = 数据竞争入口"这类缺口。

### 5.4 关闭与取消路径

- WM_CLOSE:`if (SessionService::Instance().IsBusy())` → 提示框("任务运行中:等待完成 / 取消并退出");取消即 `CancelScan()`。
- DelegateOp 轮询化(500ms)+ cancelScan_ 检查(R-4)。
- UndoRecord 线程化进 worker 队列(R-5)。

### 5.5 回归测试思路(无头为主)

1. **既有无头基建**:SessionServiceTests 已用 `SetWindow(nullptr)` 关闭投递(测试文件 72 行)——扩展:
   - 竞争压力测试:UI 模拟线程循环调 ApplySortAndRefresh 等价逻辑,worker 并发 RunPlan(FakeScanner/FakeOp),持续 30s 断言无崩溃 + plan 项路径集合不变(在快照方案下直接断言 results_ 只被 worker 写)。
   - 索引一致性:CollectChecked 返回路径对后,模拟"确认期间结果被替换"(直接调用 OnScanDone 等价逻辑),断言执行计划被拒。
2. **纯逻辑单测**:PlanBuilder 路径集合校验、CurrentTab 边界(-1)、SB_SETPARTS 边缘宽度计算(提为纯函数)。
3. **UI 冒烟(需 GUI,一次性)**:加 `--selftest` 参数:创建窗口→程序化触发全部按钮/Tab/右键/排序→退出,配合 Application Verifier(Handles/Heaps)跑一轮;无法进 CI 时作为发布前手检脚本。
4. 崩溃观测:CrashDump 已就绪(M4),补充:向用户索取 `%LOCALAPPDATA%\MiniSys\dumps` 目录约定,出现即归因。

---

## 6. 修复顺序

| 优先级 | 项 | 工作量预估 |
|---|---|---|
| **P0** | R-1 快照契约 + SetTaskBusy 矩阵(先上"禁用矩阵"止血,再做快照根治) | 禁用矩阵 0.5h;快照 0.5-1 天(含 SessionServiceTests 扩展) |
| **P1** | R-2 路径级确认校验(或 IsBusy 禁执行按钮,两者都做);R-3 负值防护(一行);R-4 DelegateOp 轮询取消 | 各 ≤0.5 天 |
| **P2** | R-5 撤销线程化;R-6 LastReport 值返回;R-7 scanTab 归属;R-9 状态栏缓存 | 合计约 1 天 |
| **P3** | R-8 创建检查/RECT 零初始化;R-10 杂项;R-11 移交 core/Json 审查 | 择机 |

---

## 7. 已检查且安全清单(证据要点)

- **消息路由**:WM_NCCREATE→GWLP_USERDATA(MainWindow.cpp:94-106)正确;WM_NCCREATE 前后 self 为 null 走 DefWindowProc;WM_CREATE 期间子控件 WM_COMMAND 不可达(无泵窗口)。
- **模态重入逐点核过**:OnPlanDone 的条件 MessageBox(459-474)、OnExecute 确认框(399)、OnEmptyQuarantine 确认框(486)、OnChooseTarget 的 SHBrowseForFolderW(548)、OnAbout 的 TaskDialog(525)、OnContextMenu 的 TrackPopupMenu+MessageBox(274/281)——模态泵会分发 WM_APP_*(已证),但各处后续代码只用值拷贝/按路径重查/锁保护数据,无崩溃路径。
- **StartTask 并发不变量**(SessionService.cpp:64-86):taskKind_ CAS 忙检查正确;join 旧 worker 先于建新;`Shutdown` 幂等(joinable 检查),RunMessageLoop 与静态析构双重调用安全;异常隔离(try/catch 全捕获)。
- **RunScan 写→Post 定序**:results_ 写入(116)先于 Post(DONE)(123),消息队列保证 UI 侧读取在写后(实践上 PostMessage 内核调用提供全屏障;C++ 标准意义的形式竞争由 §5.1 快照根治)。
- **OperationLog 全方法 `mu_` 互斥**(OperationLog.cpp:189/215/227/243),UI 与 worker 并发 LoadAll/UpdateStatus 安全;Logger 有 mutex(Logger.cpp:15/50)。
- **字符串/格式**:FormatW 全部 UI 调用点格式串与参数逐一对账匹配(%d/%s/%zu/%llu 均正确);`_vscwprintf`+`va_copy` 实现正确,失配返回空串不越界;所有 `.c_str()` 临时均在完整表达式内消费(含 OnEmptyQuarantine 三元、UndoSelected 撤销消息、UpdateStatusBar 三临时)。
- **Icons.cpp 失败路径**:FAILED/空 hIcon/AddIcon<0/BCM==0 四分支全部清理(无泄漏无悬垂);DestroyIcon 时序正确(imagelist 持副本)。
- **comctl32 v6 链路**:manifestdependency pragma + comctl32.lib 链接 + /utf-8 双配置(vcxproj 54/60/80/88 行)。
- **文件操作 UI 抑制**:SHFileOperationW(MoveJunctionOp.cpp:60-95)与 IFileOperation(DeleteOp.cpp:38-40)均带 FOF_SILENT/FOF_NO_UI/FOF_NOERRORUI,worker 无泵线程不会被 shell 弹窗卡死;pfo->Release() 存在(DeleteOp.cpp:67)。
- **lParam 静态越界防护**:OnExecute(MainWindow.cpp:387)、OnOpenLocation(537)、PlanBuilder::Build(48)三处齐全。
- **v2.1 全部 11 个变更文件**(§4)逐条复核,无崩溃级缺陷;v2.1 编译产物已验证(exe 含 v2.1/TaskDialog 字符串)。

---

## 8. 被推翻的假设 / 已验证无问题

1. **"OnTabChanged→OnSize 布局重入"**——证伪:LayoutWindow 不向主窗口回调。
2. **"OnPlanDone 弹窗期间新 OP 消息排队致重入"**——证伪:worker 完成后不再产生消息,模态期间无法启动新任务。
3. **"FolderTree 右键流程的 itemPaths_ 迭代器/TVI 句柄在 TrackPopupMenu 返回后仍被使用(erase)"**——证伪:当前代码无 erase,`it`/`hItem` 不跨模态存活,`folderPath` 为拷贝,后续按路径重查。这是任务线索块中的最大误报。
4. **"ShowHint/三元表达式中临时 wstring 悬垂"**——证伪:完整表达式生命周期规则覆盖全部调用点(OnEmptyQuarantine 的 `usage.empty() ? L"" : (L"当前占用: " + usage).c_str()` 是安全写法,不是经典悬垂模式——那需要把指针存入变量)。
5. **"Presenters 的 UiHandles 副本导致 HWND 悬垂"**——证伪:控件存活期覆盖 presenter 全部使用点。
6. **"v2.1 从未被构建/测试"**——部分证伪:已编译成功且 54/54 单测在 00:05:54 运行过(测试进程写入同一日志);仅 GUI 人工运行确实缺失。
7. **"日志 16 条 ERROR 指向产品故障"**——证伪:全部为测试注入的 `Scanner threw: boom`(SessionServiceTests ThrowingScanner),时间戳与 MiniSysTests.exe 运行窗口吻合。
8. **"LoadImage(IDI_APPICON) 会失败"**——实际 app.ico 有效且已编译进资源;即便失败也只是默认图标。

---

## 9. 未验证 / 局限

1. **未运行任何程序/构建/测试**(任务约束);v2.1 "编译成功"是从 exe 二进制字符串 + 时间戳链推断,未亲自执行 msbuild。
2. **崩溃概率为机制推算**:R-1 标注 Likely(代码机制 Confirmed、复现需用户操作时序),无 dump、无复现器佐证量级。
3. **形式内存模型 vs 实践**:RunScan 的"写 results_ → Post → UI 读"路径在 C++ 标准下是数据竞争,但 x64 Windows 上 PostMessage 内核调用提供事实全屏障;本报告仅将**执行期间**的并发读写(R-1)定为实际风险,扫描完成路径归入 §5.1 根治范围。未做 TSAN/ASAN 验证(MSVC 工具链未含,且约束禁止构建)。
4. **rules.json "bad number at offset 14" 根因未查**(`util/Json.cpp` 超出 UI 范围),仅记录现象与回退行为;建议移交 core/Json 审查(若 rules.json 永远解析失败,外置规则功能实际失效——这本身是功能级问题)。
5. **IFileOperation 在 STA 无泵 worker 线程的 COM 单元语义**(封送/进度回调需要泵的场景)未深究:本地 in-proc 删除通常不需封送,未观察到挂起;列入观察项。
6. **高 DPI(>150%)下 SHGSI_SMALLICON 与 16x16 ImageList 的尺寸失配**只推断了降级路径,未实测(需 GUI)。
7. Dump 分析:无转储文件,本报告未做任何栈符号归因。
