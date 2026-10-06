# MiniSys UI 稳定性审查(崩溃风险专项)——04 分报告

> 日期:2026-10-06 · 对象:HEAD `b6dbe39`(v2.8)工作树
> 必读文档已读:docs/REVIEW_2026-10-06-UI.md(含 v2.4–v2.8 附录)、docs/REVIEW_2026-10-06.md、docs/ARCHITECTURE.md
> 方法:全部发现均亲自读码定位(main.cpp / MainWindow.cpp/h / ui/* 全文、core/SessionService.cpp/h 全文、PlanBuilder / OperationLog.h / DirSizeCache / DevBuildCache / VolumeIndex.h 相关段),并逐条读 5 个近期提交 diff(git show)。无 GUI 活体、无 TSAN(见 §8 局限)。
> 事实核验(只读):`C:\Users\tjf\AppData\Local\MiniSys\` 无 dumps 目录;logs/minisys.log 155 行,唯一异常行是单测注入的 "Scanner threw: boom"(2026-10-06 13:56–14:04,与测试时间吻合),无真实崩溃记录 —— 与"0 崩溃"目标现状一致。

---

## 1. 结论:审计清单线索 证实 / 证伪表

| # | 线索(来自审计清单/历史修复) | 裁定 | 依据(file:line) |
|---|---|---|---|
| 1a | OnTabChanged→OnSize→UpdateStatusBar 递归 | **证伪** | OnSize 只调 Layout+UpdateStatusBar(MainWindow.cpp:487-495),无回边 |
| 1b | IDM_LIST_SELECTALL/SELECTNONE 批量循环漏 SetBatchUpdate | **证实**(R-2,非崩溃) | MainWindow.cpp:330-338 无 `lp->SetBatchUpdate(true)`;对照 OnSelectAll(MainWindow.cpp:1397-1402)有 |
| 1c | TrackPopupMenu 模态泵期间 WM_TIMER 重入 OnScan/OnVerifyList | **证伪(崩溃级)** | OnVerifyList 入口 `taskMode_ != None` 闸(MainWindow.cpp:576);快速筛选菜单期间 verify 启动后,选过滤器走 RunSearch→SearchAsync 失败→TIMER_SEARCH_RETRY 80ms 优雅重试(MainWindow.cpp:563-569, 1026-1031) |
| 2a | 自定义消息无堆负载 | **证实** | 全部 Post 仅传 kind/进度数值(SessionService.cpp:66-68, 302, 314, 385, 432, 469, 581, 649, 725, 774, 826) |
| 2b | 窗口销毁后 worker Post 的后果 | **证伪(无害)** | RunMessageLoop 退出后才 `Shutdown()` join(MainWindow.cpp:156);PostMessage 对已销毁句柄仅失败返回;进程随即退出,句柄重用误投窗口不存在 |
| 3a | OnVerifyDone 用 LastVerifyTab 索引 presenters_ 越界 | **证伪** | MainWindow.cpp:601 `if (tab < TabId::Junk \|\| tab >= TabId::Count) return;`;且 lastVerifyTab_ 只来自合法 CurrentTab() |
| 3b | 模态期间 ApplyDeadPaths→PlanStale | **证实但属设计内安全网** | planHash 兜底:PlanBuilder.cpp:63-66;用户可见后果=「计划已过期」提示(MainWindow.cpp:1237-1239) |
| 3c | dynamic_cast 失败路径 | **证伪** | 所有 6 处 dynamic_cast 结果均判空后使用(MainWindow.cpp:197, 218, 239, 247, 582, 602, 852, 1042, 1345 等) |
| 4a | RenderItems lParam 与 snapshot_ 对齐 | **证实安全** | RenderItems 以 i 设 lParam(Presenters.cpp:105);排序/清剪前先 CaptureCheckState(Presenters.cpp:296, 247) |
| 4b | HistoryPresenter 行号与 recs 对齐 | **证伪(当前对齐,属潜在脆弱)** | 行号==lParam==recs 下标(插入序一致,Presenters.cpp:683-684);所有写日志的 worker 完成后 OnPlanDone→Refresh 重渲染(MainWindow.cpp:1282) |
| 4c | ApplyDeadPaths 的 checkStateByPath_ 保留 | **部分证实有问题** | 保留逻辑本身正确(Presenters.cpp:247),但在非活动页调用时 CaptureCheckState 读到的是**别的页的行**(见 R-1) |
| 5 | 空指针/失败路径 | **基本证伪** | TrackPopupMenu 返回 0 已处理(MainWindow.cpp:545, 1477);GetWindowRect 未检查但 rc{} 零值无害(MainWindow.cpp:540-541);SB_GETRECT 失败则跳过 SetWindowPos(MainWindow.cpp:812-820);无 stoi/at;_wfsopen 判空(VolumeIndex.cpp:958, 1043);CreateWindow 失败未检查属 P3(R-7) |
| 6 | 字符串/编码 | **证伪** | FormatW 两段式 _vscwprintf+vswprintf_s 正确(StringUtils.cpp:62-74);emoji VS16 为宽字符字面量;info 标签 SS_NOPREFIX(Controls.cpp:64);菜单无裸 & |
| 7 | 模态泵重入(确认框期间 idle 重扫/校验) | **机制证实、后果被 planHash+CAS 拦截** | OnExecute 确认期间 taskMode_=None,TIMER_IDLE_REFRESH 可入 OnIdleCheck(MainWindow.cpp:634-663,需 5min 空闲);重扫替换 results_ 后确认→PlanStale 拒绝,不会错位执行 |
| 8a | WM_DESTROY 不 KillTimer | **证伪** | 定时器归窗口所有,DestroyWindow 自动回收;TIMER_SEARCH_DEBOUNCE/RETRY/VERIFY_LIST 均在触发时 Kill(MainWindow.cpp:405, 411, 419) |
| 8b | OnPlanDone 弹窗期间关窗的销毁顺序 | **证伪(无崩溃)** | win 在栈上,RunMessageLoop 返回(内部已 Shutdown join)后才析构 presenters(main.cpp:31-38, MainWindow.cpp:156);弹窗期间 DestroyWindow→弹窗被系统销毁→后续 SendMessage 到已毁控件仅返回 0 |
| 9 | 历史修复 04-1..04-5 是否被 v2.5–v2.8 破坏 | **全部存活**(逐一核码) | 见 §7.1 |
| 10 | 新线索:ghost-DONE 双处理 | **机制存在、被门闸拦截到"无崩溃"级**(R-4) | 见 R-4 分析 |

**总体判断:当前代码未发现内存不安全级(UAF/悬垂/数据竞争导致未定义行为)缺陷;残留问题集中在"共享 ListView 跨页渲染"(R-1)与状态机竞态(R-4),均为错误展示/状态错乱级,与"无崩溃转储"的观测一致。**

---

## 2. 摘要(Top 风险排序)

| 排名 | 发现 | 等级 | 类型 |
|---|---|---|---|
| 1 | R-1 v2.8 异步校验完成后对**非活动页 presenter** 调 ApplyDeadPaths→RenderItems,渲染进共享 ListView,造成跨页显示/lParam 错位,极端序列下可"执行到用户没选的项"(仍受 GuardRails+隔离区兜底) | P1 | 错误数据/误操作面(v2.8 引入) |
| 2 | R-2 右键"全选/全不选"批量循环漏 SetBatchUpdate → LVN_ITEMCHANGED 风暴 → O(N²) 冻结(千行 0.4–2s) | P2 | UI 冻结(审计清单假设证实) |
| 3 | R-4 ghost-DONE 消息序竞态:旧任务 DONE 迟到时把新任务的矩阵提前解锁 | P2 | 状态机(低概率) |
| 4 | R-3 Icons.cpp CopyImage(LR_COPYDELETEORG) 成功路径 double DestroyIcon | P2 | 句柄卫生(崩溃概率极低) |
| 5 | R-5 SearchPresenter::Refresh 覆盖丢失 CaptureCheckState → 搜索页勾选在切页后丢失(L-1 类回退,仅搜索页) | P2 | 勾选保持回退 |
| 6 | R-6/R-7/R-8 小项:TreeView_GetItem 未检查、CreateControls 未检查失败、checkStateByPath_ 无界增长 | P3 | 健壮性 |

---

## 3. 发现清单

### R-1(P1 · 置信度:高 · v2.8 引入)VERIFY_DONE 对非活动页渲染共享 ListView → 跨页显示与 lParam 错位

- **位置**:`MiniSys/src/MainWindow.cpp:593-610`(OnVerifyDone)、`MiniSys/src/ui/Presenters.cpp:245-268`(ApplyDeadPaths)、`Presenters.cpp:65-75`(CaptureCheckState)、`Presenters.cpp:85-123`(RenderItems)。
- **触发条件**:verify/preview 为只读任务,**不置 taskMode_、不锁 Tab**(v2.8 设计,SessionService.cpp:748-776 无 SetTaskBusy)。序列:① 在垃圾页,窗口重获焦点 → WM_ACTIVATE 置 400ms 定时器(MainWindow.cpp:423-430);② 定时器到点 OnVerifyList 用**当时活动页**快照发起 VerifyPathsAsync(MainWindow.cpp:575-589);③ 校验运行期间(千行约百毫秒)用户切到"大文件"页——Tab 可点;④ VERIFY_DONE 到达 → OnVerifyDone 取 `svc.LastVerifyTab()`(=垃圾页)→ 对垃圾页 presenter 调 ApplyDeadPaths。
- **后果**(三层):
  1. ApplyDeadPaths→RenderItems 把**垃圾页的行渲染进当前显示"大文件"页的共享 ListView**(单实例 IDC_LISTVIEW,六页复用),显示与活动 presenter 的 snapshot_ 脱节,直到下次切页/扫描才自愈;
  2. CaptureCheckState 先执行,读到的是当前页(大文件)的行与 lParam,越界过滤后仍把**别的页的路径→勾选**写进垃圾页的 checkStateByPath_(数据污染);
  3. 脱节期间用户勾选行 → LVN_ITEMCHANGED→UpdateExecButton/OnExecute 用**活动页(大文件)snapshot_** 解释垃圾页渲染出来的 lParam → CollectChecked 索引落在错误元素上 → PlanBuilder::Build 与 RunPlan 两边用同一份(错误但自洽的)items,planHash 校验通过 → **可能隔离用户并未在列表中看到的项**。安全兜底仍在(GuardRails 拒系统路径、隔离区可还原),但"所见即所选"被破坏。
  4. 附带:非活动页渲染时活动 presenter 的 `InBatchUpdate()` 为 false(MainWindow.cpp:198 用的是活动页的标志),每行 SetCheckState 触发一次 O(N) UpdateExecButton,叠加 O(N²) 开销。
- **v2.7 对比**:v2.7 的 VerifyRows 在 OnVerifyList 内同步执行(该提交 diff 证实),UI 线程占用期间不可能切页,故无此窗口;v2.8 移 worker 后引入。作者注释"correct even if the user switched tabs meanwhile"只考虑了 presenter 寻址正确,漏了共享 ListView 与 CaptureCheckState 的读侧。
- **修复草案**(任选其一,建议 a):
  a) OnVerifyDone 中 `if (CurrentTab() != tab)` 时不立即应用——把 dead paths 暂存到该 presenter(如 `pendingDead_`),在其 OnActivate/下次 Refresh 时应用;或
  b) RenderItems/CaptureCheckState 增加"仅活动 presenter 可渲染共享 ListView"的断言闸,ApplyDeadPaths 检查 `ui_.list` 归属;
  c) 最小补丁:`if (CurrentTab() != tab) { /* 延迟 */ return; }` + OnTabChanged 里补一次应用。

### R-2(P2 · 置信度:高)右键菜单"全选/全不选"未设 batchUpdate_ → LVN 事件风暴

- **位置**:`MiniSys/src/MainWindow.cpp:330-338`(IDM_LIST_SELECTALL / IDM_LIST_SELECTNONE 分支)。对照:OnSelectAll(MainWindow.cpp:1397-1402)与 RenderItems(Presenters.cpp:96, 122)均正确设置。
- **触发**:列表右键 → 全选(或全不选),行数 N。每个实际翻转的行触发 LVN_ITEMCHANGED(MainWindow.cpp:186-200:taskMode_==None 时 checkFlipped→UpdateExecButton),每次 UpdateExecButton 做 O(N) CollectChecked + O(选中) 汇总(MainWindow.cpp:1035-1066)。
- **后果**:O(N²) 条 ListView 消息。按既有实测口径(U-2:Ctrl+A 千行 0.4–2s),右键全选同级冻结;期间输入无响应。非崩溃,但属"运行稳定性"目标内最易复现的卡顿。verify/preview 运行期间 taskMode_ 仍为 None,风暴同样发生。
- **修复草案**:与 OnSelectAll 相同,循环前后 `lp->SetBatchUpdate(true/false)`,结束后单次 UpdateExecButton(约 6 行)。

### R-3(P2 · 置信度:高[代码路径] / 崩溃概率:低)Icons.cpp double DestroyIcon

- **位置**:`MiniSys/src/ui/Icons.cpp:26-29`。
```cpp
HICON icon = static_cast<HICON>(CopyImage(sii.hIcon, IMAGE_ICON, cx, cy, LR_COPYDELETEORG));
if (!icon) icon = sii.hIcon;
else       DestroyIcon(sii.hIcon);   // LR_COPYDELETEORG 已删除 sii.hIcon → 二次销毁
```
- **触发**:每次 SetStockButtonIcon 走 CopyImage 成功路径——含扫描按钮 find↔stop 图标每次任务切换(v2.5 L-22 引入的翻转)。
- **后果**:对已释放句柄 DestroyIcon。实践中 DestroyIcon 对失效句柄校验后返回 FALSE,且两次调用之间无其他 GDI 分配,句柄槽被复用的概率趋近于零;但这是明确的句柄卫生缺陷,审计口径应记。
- **修复草案**:删除 else 分支的 DestroyIcon(LR_COPYDELETEORG 已负责),或改用 `LR_COPYFROMRESOURCE`/先 Copy 后手动 Destroy 一次。

### R-4(P2 · 置信度:中 · 低概率)ghost-DONE:旧任务完成消息迟到,解锁新任务的矩阵

- **位置**:worker 侧 `SessionService.cpp:268-273`(先 `Post(DONE)` 后 `taskKind_.store(None)`);UI 侧 `MainWindow.cpp:881-883`(OnScanDone 无条件 `SetTaskBusy(TaskMode::None)`)、`MainWindow.cpp:1243-1245`(OnPlanDone 同)。
- **触发**(窄窗):用户点击入队一条"可启动任务"的命令(目前唯一现实入口:扫描按钮在扫描中=取消,恰在扫描结束瞬间点击→该点击被处理时 taskKind_ 已置 None→变成**新扫描**),而旧任务的 DONE 消息排在该点击之后。顺序:点击(队列)→worker Post(DONE)+store(None)→UI 处理点击→StartTask CAS 成功→UI 处理旧 DONE→SetTaskBusy(None) 在新任务运行中被执行。
- **后果**:新扫描期间矩阵被解锁(按钮/列表/进度条状态错乱);OnScanDone 还会用旧数据 Refresh 一次(闪烁);用户再点"扫描"会 Cancel 掉新扫描。**无崩溃/无 UAF**(ExecutePlan 被 IsBusy 拦,results_ 全部锁拷贝)。
- **已排除的放大器**:WM_TIMER 不可越过已排队消息(WM_TIMER 最低优先级),idle 重扫不会成为触发器;Executing 期间全部入口禁用,OP_DONE 的 ghost 仅存于 UndoRecordsAsync 的 TASK_STARTED 倒置(已被 04-4 的 IsBusy 闸吸收,MainWindow.cpp:378)。
- **修复草案**:任务代计数——SessionService 每次 StartTask 递增 `generation_`(atomic),DONE 消息 wParam 携带代数,UI 端记录"已见代数",旧代 DONE 只做无副作用清理(或仅 `if (svc.IsBusy()) return;` 早退)。

### R-5(P2 · 置信度:高)SearchPresenter::Refresh 丢失 CaptureCheckState —— L-1 修复在搜索页回退

- **位置**:`MiniSys/src/ui/Presenters.cpp:442-446`(override 直接 `snapshot_ = Results(tab_); RenderItems();`),对照基类 `Presenters.cpp:53-61`(先 CaptureCheckState)。
- **触发**:搜索页勾选若干行 → 切到其他页再切回(OnTabChanged→Refresh,MainWindow.cpp:766),或一次新搜索完成(WM_APP_SEARCH_DONE→Refresh,MainWindow.cpp:389)。
- **后果**:RenderItems 落到 `check = recommended`(搜索项恒 false,Presenters.cpp:117-120 + SessionService.cpp:358)→ 用户勾选全部丢失。与 L-1(v2.4 P0-1 修复)同类的勾选意志丢失,仅限搜索页。非崩溃;对"勾选即确认意志"的安全工具有交互伤害。
- **修复草案**:SearchPresenter::Refresh 开头补 `CaptureCheckState();`(与基类一致),或在基类提供 `RefreshFromResults()` 模板方法。

### R-6(P3 · 置信度:高)TreeView NM_DBLCLK 中 TreeView_GetItem 返回值未检查

- **位置**:`MiniSys/src/MainWindow.cpp:262-273`。tvi 零初始化,失败时 lParam=0 → 误把 0 当有效下标下钻到 items[0](有 `idx < items.size()` 边界,不越界)。修:检查返回值并与 -1 判别。

### R-7(P3 · 置信度:高)CreateControls 未检查任何 CreateWindowExW 失败

- **位置**:`MiniSys/src/ui/Controls.cpp:10-157` 全部裸创建。若某控件创建失败(资源枯竭),后续 SendMessage/SetWindowPos 收到 NULL 句柄——Windows 对 NULL 目标的消息发送实践上安全失败,不会崩溃,但行为不可预期(如 h_.status 为 NULL 时 UpdateStatusBar 的 SB_SETPARTS/SB_GETRECT 全部落空)。修:创建后统一校验,失败即 MessageBox+返回失败(与 MainWindow::Create 的 RegisterClass 失败处理对齐)。

### R-8(P3 · 置信度:高)checkStateByPath_ 会话内无界增长

- **位置**:`Presenters.cpp:72-74`——每次 CaptureCheckState 把当前全部行写入 map,只增不减。千行级列表多次排序/切页后为数千键,内存量级 KB–MB,无崩溃风险;长会话+大结果集时可观。修:RenderItems 后可裁剪到当前快照的路径集合。

---

## 4. 近期 5 个提交逐条复核

| 提交 | 范围 | 复核结论 |
|---|---|---|
| `94ea77c` chore | .gitignore + 删临时 bat | 无代码面,**未发现缺陷** |
| `2360d1c` v2.5 | TaskDialog 统一、列头排序箭头、状态栏 4 分格+进度覆盖、TryLoadCachedResults×4、空闲重扫定时器、icons imagelist 销毁 | **未发现崩溃级缺陷**。逐点:① 定时器 TIMER_IDLE_REFRESH 挂 hwnd_、DestroyWindow 自动回收;OnIdleCheck 双闸(taskMode_+5min 空闲+30min 过期,MainWindow.cpp:634-663),idle 重扫只能经 OnScan→StartScan CAS,确认框模态期间的侵入被 planHash 兜底为 PlanStale;② TryLoadCachedResults 锁内判重+Json 深度限制(v2.2 已硬化),riskLevel/strategy 越界即拒(SessionService.cpp:142-147);③ 状态栏 4 分格 SB_GETRECT 失败即跳过覆盖(MainWindow.cpp:811-820);④ icons 复用销毁逻辑引入 **R-3**(见上);⑤ 进度条覆盖第 4 格随 OnSize 重定位,链路无递归。⑥ cache 落盘 temp+rename(MOVEFILE_REPLACE_EXISTING,SessionService.cpp:176-183) |
| `0612e22` v2.6 | 快速筛选菜单、进页不自动搜索、WalkBuild 回退 | **未发现崩溃级缺陷**。① OnQuickFilterMenu:CreatePopupMenu/TrackPopupMenu 失败路径均安全(返回 0→return,MainWindow.cpp:542-545);ToggleQueryToken/ParseFilterTerms 为纯静态字符串函数(VolumeIndex.cpp:764-815),UI 线程调用不触索引容器;菜单模态泵期间 verify 可启动但被任务闸/重试定时器消化;② 进页不自动搜索:buf 判空(MainWindow.cpp:760-763, 892-895);③ WalkBuild 仅 worker(EnsureBuilt 调用链:BuildIndexAsync/JunkScanner::Scan 均在 worker) |
| `f0c94ab` v2.7 | WM_ACTIVATE→TIMER_VERIFY_LIST、DirSizeCache、ApplyDeadPaths 前身 VerifyRows→StoreResults | **未发现崩溃级缺陷**(v2.7 形态下 verify 同步在 UI 线程,无跨页窗口)。① WM_ACTIVATE 去抖 400ms+一次性 Kill(MainWindow.cpp:402-430);② VerifyRows 的 StoreResults 锁内替换,planHash 语义一致;③ DirSizeCache 双检锁+只缓存 size>0(DirSizeCache.cpp:13-31),仅 worker 使用 |
| `b6dbe39` v2.8 | VerifyPathsAsync/PreviewAsync 移 worker、WM_APP_VERIFY_DONE/PREVIEW_DONE、DevBuildCache 并行池、DoneMessageFor | **发现 R-1**(VERIFY_DONE 跨页渲染,见 §3)。其余核过:① DoneMessageFor 全 kind 覆盖+异常路径补发(SessionService.cpp:34-44, 268-271)——04-5 契约扩展正确;② VerifyPathsAsync/PreviewAsync 的 lambda 按值拷贝 paths/items/selected,worker 只写 verifyMu_/previewMu_ 保护成员;③ LastPreview()/LastReport() 返回 const 引用但 UI 在无模态窗口前完成字符串拷贝(OnPreviewDone 先拼 content 再 MessageBox;OnPlanDone 同),期间无法启动新任务(所有启动入口需用户输入,owner 被 modal 禁用)→ 无竞态;④ DevBuildCache 2-8 线程池:found 只读、sized[i] 按 fetch_add 独占、progress 在 pmu 下回调(DevBuildCache.cpp:240-266),Post 多线程安全;⑤ 校验/预览不锁矩阵的设计使 Tab 在任务期间可切换——这正是 R-1 的窗口来源(设计取舍,需要补渲染归属闸) |

---

## 5. 系统性加固方案(适配 Win32)

1. **任务代数(generation)消息契约**:SessionService 每次 StartTask 递增 atomic `taskGen_`,所有 DONE 消息 `wParam=taskGen`;MainWindow 记录 `appliedGen_`,旧代 DONE 直接丢弃。一并消灭 R-4 与未来同族消息序问题(04-4/04-5 的结构性后继)。
2. **共享 ListView 渲染归属闸**:`ListTabPresenter::RenderItems()` 断言/检查"本 presenter 是活动页"(由 MainWindow 注入 `IsActive()` 回调或UiHandles 加 owner 查询);非活动页的清剪结果挂 `pendingDead_`,OnActivate 时应用。根治 R-1。
3. **ScopedReentrancyGuard**:RAII 型 `class ReentrancyGuard { bool& f_; ... }`,入口处置位、析构复位;用于 OnTabChanged/OnVerifyDone/OnPlanDone 等"会被模态泵重入"的处理器,配合 `if (guard_) return;` 早退。Win32 下模态泵(TaskDialog/MessageBox/TrackPopupMenu)必然分发 WM_TIMER/WM_APP,该守卫是 04-3 家族的通用解。
4. **定时器集中管理**:单一 `TimerHub`(id 表、语义注释、启动/杀死日志、WM_DESTROY 统一 KillTimer + 断言无泄漏)。当前 4 个定时器散在 3 处 Set/Kill,新增时易漏闸。
5. **UI 线程断言**:main 里记录 `g_uiThreadId`;所有 presenter/MainWindow 方法首行 `MS_ASSERT_UI_THREAD()`(Debug 下触发 CrashDump 日志)。Worker 误触控件类 API 时在开发期即暴露。
6. **模态单飞门**:MainWindow 加 `modalActive_` 标志(ConfirmTask/ShowItemInfo/MessageBox 包装统一置位);WM_TIMER 与 DONE-applier 在门内只入队不执行。消除"确认框期间 idle 重扫/verify"整类重入(当前靠 planHash 事后兜底)。
7. **grep 闸(CI/pre-commit)**:禁止 ① `results_[` 出现在 SessionService.cpp 之外;② `MutableResults`(防回归);③ `ListView_SetCheckState` 循环体外无 `SetBatchUpdate`(可用简单脚本查 \*.cpp 中 SetCheckState 出现次数>3 且同函数无 batch 调用);④ `PostMessageW` 带指针参数;⑤ `TaskDialogIndirect`/`MessageBoxW` 直呼(必须经 dialogs:: 包装以便置 modalActive_)。
8. **回归测试思路**(纯逻辑层,不起 GUI):① checkStateByPath_ 不变量:任意排列(prune/sort/replace)后按路径恢复勾选;② ApplyDeadPaths 对"非活动页"语义(把 presenter 与 list 归属抽象成可注入接口);③ 消息序:模拟 DONE 乱序序列断言 taskMode_ 一致性;④ ToggleQueryToken/ParseFilterTerms 已有 4 项,补 CaptureCheckState 与 ghost-DONE 代数闸的单测。UI 冒烟:一个最小 Win32 壳进程按脚本投递消息序列(激活→切页→DONE),断言无未定义行为(配合 Application Verifier 句柄检查跑 R-3)。

---

## 6. 修复顺序建议

- **P0**:R-1(跨页渲染闸/延迟应用)——唯一具有"用户可见错误数据 + 误隔离面"的缺陷,且触发窗口(v2.7 起的自动校验 + v2.8 异步化)在正常使用中可达。
- **P1**:R-2(6 行补 SetBatchUpdate);R-4(任务代数);§5-1/§5-2 一并落地。
- **P2**:R-3(删一行)、R-5(补一行 CaptureCheckState)、§5-3/5-4/5-6。
- **P3**:R-6、R-7、R-8 与 §5-5/5-7/5-8。

---

## 7. 已检查且安全清单(本轮逐项核码)

1. **04-1 存活**:搜索全链 worker 化;VolumeIndex 容器仅 worker 触碰(BuildIndexAsync/JunkScanner::Scan/SearchAsync 均经 SessionService 单 worker 串行);UI 仅读 `valid_`/`entryCount_` 两个 atomic(MainWindow.cpp:752, 889, 942-950;Presenters.cpp:429-432;VolumeIndex.h:158-159)。
2. **04-2 存活**:WM_TIMER 四分支中 debounce/retry 有 `taskMode_ == None` 闸(MainWindow.cpp:408, 412),idle/verify 走 OnIdleCheck/OnVerifyList 自身闸;MutableResults 已删净(grep 仅注释一处)。
3. **04-3 存活且经受住 v2.8**:ShowItemInfo 所有对话框字符串(titleCopy/content/what/consequence/undoLine)在 TaskDialogIndirect 前完成拷贝(Presenters.cpp:579-584);模态泵内即使 VERIFY_DONE 替换 snapshot_,对话框不再读 `it`。
4. **04-4 存活**:WM_APP_TASK_STARTED 有 IsBusy 闸(MainWindow.cpp:378)。
5. **04-5 存活且扩展**:DoneMessageFor 覆盖 Scanning/Searching/Verifying/Previewing/Executing;各 body 的 DONE 覆盖全部返回路径(RunPlan 的 PlanStale 提前返回也 Post,SessionService.cpp:469);wrapper catch 兜底补发(268-271)。
6. 单 worker 串行 + StartTask CAS + join-before-start(SessionService.cpp:245-251):无 double-thread、无 std::thread terminate 路径。
7. results_ 全链锁拷贝(Results/StoreResults/RunScan 落盘);RunPlan 对 worker 私有快照执行,成功项按 path 从 live 剔除(索引漂移安全,SessionService.cpp:560-573)。
8. planHash(Build/PlanMatches 双侧 + RunPlan 复核)拦截"确认框期间数据被换"类错位执行。
9. Shutdown 双重调用安全(join 清 joinable);析构序:消息循环退出→Shutdown→presenters 析构(main.cpp:31-38)。
10. PostMessage 均值参数无堆指针;worker Post 到已销毁句柄安全失败。
11. LastReport/LastPreview 的 const 引用在模态前完成消费,期间无新任务可启动(owner 被 modal 禁用)。
12. FormatW 两段式格式化、全部调用点参数匹配(逐处抽查含 %zu/%llu/%hs/%s)。
13. LVN_ITEMCHANGED 的 checkFlipped 判定 + 活动 presenter batchUpdate_ 豁免(除 R-2 两处)。
14. NM_CUSTOMDRAW ItemAtRow/OnOpenLocation/CollectChecked/CaptureCheckState 的 lParam 一律 `idx < snapshot_.size()` 边界检查。
15. ActivePresenter 对 TabCtrl_GetCurSel==-1 的范围检查(04-R3 修复存活,MainWindow.cpp:505)。
16. OnExecute 前置闸:IsBusy/空列表/空勾选/纯 InfoOnly/未选迁移目标,Apps 分支 TaskDialog 默认取消。
17. 历史页行号-lParam-recs 对齐(当前成立,见 §1 4b 的条件说明)。
18. 日志线程安全(Logger mutex)、OperationLog 单例 mutex + Generation 缓存(U-5)。
19. 磁盘缓存读写:temp+rename、大小上限 kMaxCachedItems、riskLevel/strategy 越界拒收。
20. WM_NCCREATE/CREATESTRUCT 路径、单实例互斥、加速键归属(main 循环内 TranslateAccelerator,模态内不生效——反而阻断了模态期加速键触发任务的一类重入)。

**被推翻的假设/已验证无问题**(汇总):§1 表中 1a、1c、2b、3a、3c、4b、5(除 R-6/R-7 小项)、6、7(崩溃级)、8a、8b,以及"ghost-DONE 致崩溃"(R-4 仅状态错乱)。

---

## 8. 说明与局限

- 静态读码 + 消息序推演为主,**未运行** MiniSys.exe(遵守约束),未做 TSAN/Application Verifier 动态验证;R-1/R-4 的触发概率基于代码路径与消息队列语义(WM_TIMER 最低优先级、modal 泵分发 posted 消息、禁用窗口不产生按钮点击)推演,置信度已分别标注。
- 用户数据目录仅做只读列表与日志 grep;黑名单脚本未触碰;仓库零修改(仅本报告写入指定 Temp 目录)。
- 未覆盖:core 扫描器算法内部、GuardRails 名单完备性(属安全角色)、DelegateOp/QuarantineOp 执行细节(已在 03/05 角色范围);VolumeIndex.cpp 仅审线程归属与纯函数段。
- 结论口径:以"运行稳定 0 崩溃"为目标,**无内存不安全级发现**;R-1 是唯一建议立即修的正确性缺陷。
