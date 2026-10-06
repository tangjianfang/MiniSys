# MiniSys UI 稳定性复审报告（v2.2 / v2.3 新增路径）

> 审查人：UI 稳定性审查工程师 · 日期：2026-10-06 · 对象：HEAD `5eae1d2`（v2.3）+ `639a61f`（v2.2）
> 范围：v2.2/v2.3 新增/改动的 UI 代码（TaskMode 状态机、搜索防抖、加速键/Tab 键、ITEMCHANGED 动态按钮、NM_CUSTOMDRAW、右键菜单重派发、SearchPresenter、双击路由、FormatCount、TaskDialog 确认、UndoRecordsAsync 时序）
> 方法：纯代码审计（未运行/构建任何程序；MiniSys.exe 正在用户桌面运行，未触碰）。每条发现均亲读至 file:line。
> 置信度三档：**Confirmed**（代码机制亲读证实）/ **Likely**（机制证实，触发依赖时序）/ **Speculative**（推断）。视觉表现类结论标 [I]。

---

## 0. 上轮（v2.1, 04-ui-robustness.md R-1..R-11）修复回归表

| 上轮编号 | v2.2 声称 | 本次核验结论 | 证据 |
|---|---|---|---|
| R-1 results_ 无锁 | 快照契约（锁+副本+按路径移除） | **已修复，但被 v2.3 部分破环** → 本轮发现 2 | `SessionService.cpp:58-63` Results() 锁内拷贝 ✓；`SessionService.cpp:179-181/281-294` RunPlan 快照+按路径移除 ✓；但 `Presenters.cpp:260/266/306` SearchPresenter 在 **UI 线程无锁** 调 `MutableResults()`（`SessionService.cpp:65-67` 无锁） |
| R-2 确认框期间 SCAN_DONE 错位执行 | IsBusy 闸 | **已修复** | `MainWindow.cpp:720`（OnExecute 入口 IsBusy）+ 矩阵禁 exec（`MainWindow.cpp:646-650`）+ `SessionService.cpp:184` PlanMatches 复验。确认模态内唯一重入源（挂起防抖 WM_TIMER）重放同 query → 同内容同序，`items` 引用（`MainWindow.cpp:724`）指向替换后 vector，索引仍自洽，无实害（仅设计脆弱，见发现 3 备注） |
| R-3 CurrentTab 负值 | 已修 | **已修复** | `MainWindow.cpp:394` `if (t < TabId::Junk \|\| t >= TabId::Count) return nullptr;` |
| R-4 DISM 中关窗挂起 | （P1 清单） | 未在本轮范围逐行复核（DelegateOp 属后端），无新证据 | — |
| R-5 撤销 UI 冻结 | UndoRecordsAsync 后台化 | **已修复，时序基本正确**；新排序风险见发现 4 | `Presenters.cpp:570-604` → `SessionService.cpp:389-432`（records 按值捕获 ✓、进度/完成 Post ✓） |
| R-6 LastReport 解锁引用 | — | **未改**（仍脆弱但当前安全）：`MainWindow.cpp:870` 引用跨 MessageBox 模态 | `SessionService.cpp:53-56` |
| R-7 OnScanDone 用 CurrentTab | — | 未按原建议加 scanTab_，但禁用矩阵封住"扫描中切 Tab"入口，**等效修复**；`MainWindow.cpp:574` 仍用 CurrentTab() | `MainWindow.cpp:646-650`（tab 在 busy 态禁用） |
| R-8 CreateWindow 无检查 / RECT 未初始化 | — | 仍在：`Controls.cpp` 全文无检查；`Layout.cpp:26` `RECT srect;` 未零初始化 | 均 UI 降级不崩溃 |
| R-9 状态栏每 tick 读历史 | — | 仍在：`MainWindow.cpp:511` QuarantineUsageText→LoadAll 每条进度消息全量读文件 | 非崩溃 |
| R-10 杂项 | — | `MainWindow.cpp:109/122` GetMessage 返回 -1 时 msg 未初始化仍在 | 不可达级 |

---

## 1. 摘要：按崩溃/冻结可能性排序的 Top 风险

1. **发现 1（P1，Confirmed）VolumeIndex 全程无锁：UI 线程 Search 与 worker 索引增量重建并发 → UAF**。索引重建（搜索页"扫描"按钮）走 `RefreshFromUsn→ApplyChanges→Finalize` 期间 `valid_/ready_` 保持 true，UI 防抖搜索同时在迭代正被清空/重建的 `nodes_` 与 `frnToDirPath_`（指向 `dirPaths_` key 的悬垂指针）。Finalize 在百万项规模耗时秒级（代码自注），竞争窗口大。
2. **发现 2（P1，Confirmed）SearchPresenter 在 UI 线程无锁写 `results_[Search]`，与执行中 RunPlan 的持锁读写构成数据竞争**。`SetTaskBusy` 禁用矩阵遗漏 `editSearch/chkMatchPath`（MainWindow.cpp:646-651），搜索结果计划执行期间输入/点"匹配完整路径"即触发 → 堆损坏/UAF。这是对 R-1 快照契约（"worker 是 results_ 唯一写者"）的实质回退。
3. **发现 3（P2，Confirmed 机制）模态泵内挂起的搜索防抖定时器可替换 presenter snapshot_，使 `ShowItemInfo` 的 `pszMainInstruction` 悬垂** → TaskDialog 重绘时 UAF 读。WM_TIMER 低优先级可被连续输入饿延迟，窗口比 200ms 宽得多。
4. **发现 4（P2，Likely）`WM_APP_TASK_STARTED` 无条件 `SetTaskBusy(Executing)`**：worker 的 OP_DONE 若先入队，UI 先 None 再 Executing → **矩阵永久锁死**（所有动作按钮禁用，仅重启恢复）。
5. **发现 5（P2，Confirmed 机制/Likely 低频）worker 任务体抛异常被 StartTask 吞掉且不补发 DONE** → taskMode_ 永久 busy，同样全矩阵锁死。
6. **发现 6（P2）UI 线程全索引过滤 + 150×GetFileAttributesExW 的击键级冻结**（当前用户索引仅 27 项故暂未显现；后端修复后 0.1~2s/键）+ OnTabChanged/OnScanDone 的双重重复搜索。
7. **发现 7（P2）LVN_ITEMCHANGED→UpdateExecButton 的 O(n²) 风暴**（Ctrl+A / 恢复勾选渲染，n=1000 时约 2×10⁶ 次 ListView 消息 ≈ 0.5~2s 卡顿）。

---

## 2. 发现清单

### 发现 1（P1）VolumeIndex 无锁：UI 线程 Search 与 worker EnsureBuilt 增量重建并发 → UAF / 堆损坏

- **位置**：
  - `core/VolumeIndex.h:121-136` —— 全部状态（`nodes_ / names_ / byFrn_ / childrenOf_ / dirPaths_ / frnToDirPath_ / valid_ / ready_`）**无任何 mutex/atomic 包裹**（`valid_` 为普通 bool）。
  - worker 侧变更：`core/VolumeIndex.cpp:421-453` ApplyChanges（`nodes_[..].frn=0` 墓碑、`names_ +=`、`AddNodeForTesting`→`nodes_.push_back` 可能重分配）→ `:124-186` Finalize（**125-128 清空全部 map**；150/172 `frnToDirPath_[frn] = &dirPaths_ key` —— 存的是指向 `dirPaths_` key 的裸指针）。
  - 关键路径：`core/VolumeIndex.cpp:188-207` EnsureBuilt —— `valid_ && drive==drive_ && serial 匹配` 时走 `RefreshFromUsn`（:199），**整条增量路径 `valid_` 从未置 false**；Finalize 期间 `ready_` 也保持 true（仅 :185 重设为 true，中途不置 false）。
  - UI 侧并发读：`ui/Presenters.cpp:257-275`（Refresh：`IsValid()`→`vi.Search`）；`core/VolumeIndex.cpp:608-687` Search（:622 迭代 `nodes_`；:635/650 解引用 `frnToDirPath_` 的 `const std::wstring*`）。
  - UI 触发链：`MainWindow.cpp:534-541`（搜索页 OnScan → `BuildIndexAsync`）或 `MainWindow.cpp:449-453`（OnTabChanged 自动建索引，仅在 invalid 时）；`SessionService.cpp:108-122` worker 调 `EnsureBuilt`。
- **触发场景**：会话内索引已有效（垃圾扫描或首次进搜索页建好）→ 用户在搜索页点"扫描"（v2.3 语义 = 重建索引）→ 卷序列号匹配 → `RefreshFromUsn` 发现日志增量（Windows 常态写入，几乎必有）→ `ApplyChanges→Finalize`（代码自注 :408-409：百万项规模 Finalize 耗时秒级）→ **期间用户在搜索框输入**（editSearch 未被 SetTaskBusy 禁用）→ 200ms 防抖到 → `SearchPresenter::Refresh` → `IsValid()` 为 true → `vi.Search` 迭代正被清空/重填的 `nodes_`/map → 节点 vector 重分配后旧指针解引用 / `frnToDirPath_` 指向已析构的 `dirPaths_` key → UAF 崩溃。
- **次级窗口**：`EnsureBuilt` 置 `valid_=false` 与 `BuildFull→ResetForTesting`（:212，含 `ready_=false` :91）之间存在纳秒级 TOCTOU，UI 恰在两者之间通过 IsValid 检查并进入 Search 同样竞争——窗口极小，仅记录。
- **后果**：堆上野指针读 → 随机崩溃（难归因）；或撕裂的 map 迭代 → 错误命中路径。
- **修复草案**：`VolumeIndex` 内加 `std::shared_mutex`（读：Search/IsValid/EntryCount/TryGetEntry/Collect*；写：EnsureBuilt 全程独占）；或最小改动——EnsureBuilt 进入增量路径前先把 `valid_/ready_` 置 false（ UI 侧会显示"构建中"并退避），完成后置回；UI 侧纵深：`WM_TIMER` 处理器加 `SessionService::IsBusy()` 拦截（见发现 2 一并修）。
- **置信度**：机制 **Confirmed**（所有调用点与可变状态亲读）；触发 **Likely**（需"重建期间输入"，重建按钮就在搜索框旁、Finalize 窗口秒级）。

### 发现 2（P1）SearchPresenter 在 UI 线程无锁写 `results_[Search]`，与执行中 RunPlan 数据竞争（R-1 契约部分回退）

- **位置**：
  - `ui/Presenters.cpp:260 / 266 / 306` —— `SessionService::Instance().MutableResults(tab_) = ...`（UI 线程、**无锁**赋值整个 vector）。
  - `core/SessionService.cpp:65-67` —— `MutableResults` 返回裸引用，无锁；头文件 :76 注释自认 "UI-idle-only live access"，但无任何强制。
  - worker 侧持锁访问同一元素：`core/SessionService.cpp:179-181`（RunPlan 开头持 `resultsMu_` 拷贝快照）、`:281-294`（结尾持锁 `live = std::move(kept)`）；`ExecutePlan` 预检 `:160` `Results(plan.tab)` 持锁读。
  - 矩阵缺口：`MainWindow.cpp:646-651` SetTaskBusy 禁用清单 = `{exec, undo, emptyQ, open, btnSortSize, btnSortTime, tab, list, tree, targetBtn, advancedChk, scan}` —— **不含 `editSearch / chkMatchPath / about`**。
  - 无闸入口：`MainWindow.cpp:321-332`（WM_TIMER 防抖 → SetQuery → Refresh → 无锁写）；`MainWindow.cpp:241-249`（IDC_CHK_MATCHPATH BN_CLICKED → **立即** SetQuery → 无锁写，连防抖都没有）。
- **触发场景**：用户在搜索页勾选若干结果 → 执行 → 确认 → RunPlan（tab=Search，QuarantineOp 逐项移动文件，秒级~分钟级）期间：
  1. 在搜索框继续输入 → EN_CHANGE → 200ms 后 WM_TIMER → Refresh → `MutableResults(Search) = items`；
  2. 或直接点"匹配完整路径"复选框 → 立即 SetQuery → 同上。
  UI 的无锁 vector 赋值（释放旧缓冲+拷贝新）与 worker 的持锁拷贝/移动赋值并发 → 撕裂 vector（size/capacity/data 不一致）→ UAF/堆损坏。
- **附带一致性**：执行中 Refresh 覆盖 `results_[Search]` 后，RunPlan 结尾按"计划快照路径"过滤移除（`SessionService.cpp:286-292` 以 `items[i].path` 比对新 live 表）→ 成功项基本不会残留（路径失配即保留），行为多为良性，崩溃风险才是主害。
- **修复草案**：① `WM_TIMER` 与 `IDC_CHK_MATCHPATH` 分支首行加 `if (taskMode_ != TaskMode::None) break;`（或 `svc.IsBusy()`）；② SetTaskBusy 的禁用清单补 `editSearch/chkMatchPath`（busy 时禁输入，语义也更正确）；③ 根治：`MutableResults` 删除或改为 `StoreResults(tab, items)`（锁内 move），恢复"worker 单写者+锁"契约。
- **置信度**：机制 **Confirmed**；触发 **Likely**（执行搜索结果计划期间继续输入/点复选框是自然操作）。

### 发现 3（P2）模态泵内挂起的防抖定时器替换 snapshot_：ShowItemInfo 的 pszMainInstruction 悬垂（UAF 读）

- **位置**：
  - `ui/Presenters.cpp:439` —— `tc.pszMainInstruction = it.title.c_str();`，`it` 是 `const ScanItem&`（:396 签名），由 `OnItemActivated`（:388-391）传入 **指向 `snapshot_` 元素的指针**；`ui/Presenters.cpp:446` 同步 `TaskDialogIndirect`（模态，运行自己的消息泵）。
  - 替换源：`MainWindow.cpp:321-332` WM_TIMER（模态泵会分发）→ `SetQuery` → `SearchPresenter::Refresh` → `ui/Presenters.cpp:307` `snapshot_ = std::move(items);` —— vector **移动赋值释放旧缓冲**，`it` 所指元素被析构。
  - 触达路径：搜索页右键 → `IDM_LIST_INFO`（`MainWindow.cpp:281-285`）→ `OnItemActivated(sel)` → ShowItemInfo。（双击路径在搜索页被改路由为 OnOpenLocation：`MainWindow.cpp:161-162`，故仅右键菜单可触达。）
- **触发场景**：搜索页输入关键词 → 200ms 内右键打开菜单（WM_TIMER 低优先级，可被鼠标移动等连续输入**饿延迟**，实际窗口远大于 200ms）→ 点"说明（这是什么？）…" → TaskDialog 打开 → 用户停手、队列排空 → 挂起的 WM_TIMER 在 TaskDialog 泵内分发 → Refresh 释放旧 snapshot_ → TaskDialog 重绘读取悬垂 `pszMainInstruction` → UAF。
- **同类暴露（实际无害但脆弱）**：`MainWindow.cpp:724` OnExecute 的 `auto& items = lp->Snapshot();` 引用跨确认 MessageBox/TaskDialog（:841/:836）存活——模态内同定时器触发 Refresh 后，:850 `PlanBuilder::Build(t, items, selected, ...)` 读到的是**新分配的同内容 buffer**（引用指向 vector 对象本身，未失效），且 query 文本在模态期间不可能变（属主窗口被禁用）→ 同 query 同序，无实害；但这依赖"搜索结果确定性"这一隐含前提，应一并加固。
- **后果**：悬垂宽字符串指针被对话框绘制解引用 → 随机崩溃/乱码。
- **修复草案**：ShowItemInfo 在构建 `tc` 前把 title 拷贝为局部 `std::wstring`（与 `content` 同待遇）；或 TaskDialog 改用 `pszMainInstruction` 指向局部副本。系统性修法：给 presenter 加代际号（generation），跨模态后校验；或 WM_TIMER 分支加"模态打开中"标志。
- **置信度**：机制 **Confirmed**（指针生命周期链完整亲读）；触发 **Likely偏低**（需要定时器恰在 TaskDialog 泵内到期——依赖饿延迟时序）。

### 发现 4（P2）WM_APP_TASK_STARTED 无条件 SetTaskBusy(Executing)：OP_DONE 先入队的排序可致矩阵永久锁死

- **位置**：`MainWindow.cpp:312-317`（`case WM_APP_TASK_STARTED: SetTaskBusy(TaskMode::Executing);` —— 无 IsBusy/时序校验）；投递方 `ui/Presenters.cpp:499`（FolderTree 右键）、`:603`（批量撤销）——都在 `ExecutePlan/UndoRecordsAsync` 返回 Started **之后**才 Post；而 worker 线程一启动就可能先 Post `WM_APP_OP_DONE`（如 `SessionService.cpp:184-192` 计划过期路径：拷贝快照+哈希+写报告+Post，仅数十 µs）。
- **触发场景**：消息队列 FIFO：worker 的 OP_DONE 先入队、UI 的 TASK_STARTED 后入队 → UI 先处理 OP_DONE（`SetTaskBusy(None)`，`MainWindow.cpp:868`）再处理 TASK_STARTED（`SetTaskBusy(Executing)`）→ 此后无任何 DONE 会再来 → `taskMode_` 永久 Executing：scan/exec/undo/tab/list 全禁用（`MainWindow.cpp:649-651`：Executing 态 scan 也禁用），进度条常显 → **整个 UI 死锁，只能重启**。数据无损（worker 已结束）。
- **概率评估**：需要 worker 在线程启动延迟（约 30~100µs）内跑完整个快路径并抢先入队——FolderTree 的"计划过期"分支最短；撤销路径至少含一次真实文件 IO（ms 级），几乎不可能。低概率、高影响。
- **修复草案**：`WM_APP_TASK_STARTED` 处理器加闸：`if (SessionService::Instance().IsBusy()) SetTaskBusy(TaskMode::Executing);`（worker 已结束则忽略）；更简单：Presenters 两个调用点本就在 UI 线程，直接同步调 `SetTaskBusy`（经 MainWindow 指针/消息携带）替代 PostMessage，消除排序问题。
- **置信度**：机制 **Confirmed**；触发 **Speculative→Likely 下限**（排序竞争窗口实测难度大，标记为低频高害）。

### 发现 5（P2）worker 任务体抛异常被吞且不补发完成消息 → taskMode_ 永久 busy（矩阵锁死，与发现 4 同后果）

- **位置**：`core/SessionService.cpp:76-89` StartTask 包装器：`try { body(); } catch (...) { MS_LOG_ERROR(...); }` —— body 内任何一点抛出（如 `RunPlan` 中 `fs::path` 构造/`FormatW` 的 bad_alloc、`SessionService.cpp:259` `fs::path(...)` 可抛）即跳过 body 尾部的 `Post(WM_APP_OP_DONE/SCAN_DONE)`（`SessionService.cpp:121/153/302/370/430` 均在各 body 末尾）→ UI 侧 `SetTaskBusy(None)` 永不执行。
- **与 v2.1 的差异**：v2.1 该 catch 被评为"异常隔离正确"；v2.2 引入 TaskMode 矩阵后，同一模式的代价从"按钮态不一致"升级为"全 UI 锁死"——这是状态机新引入的失败放大。
- **修复草案**：包装器 catch 后统一 `Post(hwnd, 任务对应的 DONE 消息)`（需要把 hwnd/kind 传进包装器，现有 lambda 已捕获 hwnd，可在 catch 里补发 `WM_APP_OP_DONE` 并在报告里注明"任务异常中断"）；或 UI 侧对 busy 态加看门狗（进度文本长时间不变时允许复位）。
- **置信度**：机制 **Confirmed**；触发 **Speculative**（需 worker 内异常，现实主因是 OOM/文件系统异常路径）。

### 发现 6（P2）搜索全在 UI 线程：全索引过滤 + 150×GetFileAttributesExW 的击键级冻结；另有两处双重搜索

- **位置**：
  - `ui/Presenters.cpp:271-302`：`vi.Search` 在 UI 线程遍历整卷索引（冷查询/生僻词需扫全部节点，百万项 × `IFindView` + 每命中构造 3 个 wstring 的 ScanItem），随后 `:295-302` 对前 `kSizeFetchRows=150` 行逐个 `GetFileAttributesExW`（冷缓存、路径随机分布，HDD 单次数 ms 级）。
  - 双重搜索①：`MainWindow.cpp:454-458`（OnTabChanged 进入搜索页 SetQuery→Refresh 全量搜索）后 `:482` 又显式 `p->Refresh()` 再搜一次。
  - 双重搜索②：`MainWindow.cpp:575`（OnScanDone→默认 OnScanDone→Refresh 全量搜索）后 `:577-583` SetQuery 又搜一次。
- **量级估计**：索引百万项时，单次击键（防抖后）UI 停止泵消息约 0.1~2s（SSD/HDD、冷/热缓存而定）；期间输入无回显、窗口拖动迟滞。**当前用户实测索引仅 27 项**（见 §5 移交），此冻结暂未显现——后端修复索引后即暴露。
- **防抖内重入**：无——搜索全程不泵消息，EN_CHANGE/WM_TIMER 只会排队，无重入风险（已核实 `MainWindow.cpp:321-332` 无中间泵）。
- **修复草案**：搜索移 worker（防抖后投递任务，结果经消息回 UI，天然顺带修发现 1/2/3 的竞争面）；短期：OnTabChanged/OnScanDone 去重（Refresh 与 SetQuery 二选一）；kSizeFetchRows 的属性抓取移后台或惰性到滚动/排序时。
- **置信度**：代码路径 **Confirmed**；卡顿时长为推算（无活体，[I] 属性能表现推断）。

### 发现 7（P2）LVN_ITEMCHANGED→UpdateExecButton 的 O(n²) 勾选风暴

- **位置**：`MainWindow.cpp:151-156`（`LVN_ITEMCHANGED && taskMode_==None && uChanged&LVIF_STATE` → `UpdateExecButton`）；`MainWindow.cpp:669-700` UpdateExecButton → `lp->CollectChecked()`（`ui/Presenters.cpp:107-118`，每行一次 `ListView_GetCheckState`=LVM 消息 + `ListView_GetItem`）→ 全列表 O(n)；`ui/Presenters.cpp:88`（RenderItems 的 `ListView_SetCheckState` 恢复勾选，每行发 1~2 条 ITEMCHANGED）与 `MainWindow.cpp:1011-1013`（OnSelectAll 循环 SetCheckState）为风暴源。
- **量级**：n=1000（搜索结果上限，`ui/Presenters.cpp:271` kMaxResults=1000）且勾选/恢复多行时，约 2×10⁶ 次 ListView 跨调用（进程内 SendMessage 约 0.2~0.5µs）→ 0.4~2s 无泵卡顿；任务执行完成瞬间（`MainWindow.cpp:868` 先 SetTaskBusy(None) 再 :905 Refresh）勾选恢复同样触发。
- **taskMode_ 守卫评估**：守卫方向正确——busy 态抑制（渲染/勾选变化不重算）；但 None 态的批量勾选路径（Ctrl+A、全选菜单 `MainWindow.cpp:287-295`、RenderItems 恢复）无批量豁免。
- **修复草案**：批量操作前后 `SetInternalUpdate(true)` 抑制 ITEMCHANGED 处理，结束调一次 UpdateExecButton；或 UpdateExecButton 加节流（脏标记 + 空闲时重算）。
- **置信度**：机制 **Confirmed**；卡顿时长为推算 [I]。

### 发现 8（P3）无 IsDialogMessage：Edit 内 Enter 无默认按钮、Ctrl+A 被加速键劫持

- **位置**：`MainWindow.cpp:100-119` 消息循环——只有 `TranslateAcceleratorW`（:110）与手写 Tab 处理（:112-116），**没有 IsDialogMessage**（:102 注释自称 "IsDialogMessage gives Tab-order"，与实现不符——Tab 顺序实为手写 GetNextDlgTabItem）。
- 后果（均 UX，非崩溃）：
  1. 焦点在 `editSearch/editMinSize/editFileType/editDrives` 时按 Enter：无默认按钮语义（对话框特性），无动作；
  2. **Ctrl+A 在编辑框内被加速键表（`MainWindow.cpp:105`）截走 → 触发 `OnSelectAll` 切换列表勾选**（`MainWindow.cpp:276-278/1001-1014`），编辑框文本无法用 Ctrl+A 全选——高频误触（用户在搜索框按 Ctrl+A 想全选关键词，结果勾选了 1000 行文件）；
  3. Esc 无行为（无 IsDialogMessage 的 IDOK/IDCANCEL 路由），可接受。
- **Tab 键正确性**（清单项 2）：`GetNextDlgTabItem` 对非 dialog 窗口可用（按 WS_TABSTOP 子控件序遍历，`Controls.cpp` 中 scan/exec/undo/emptyQ/open/targetBtn/editMinSize/editSearch/chkMatchPath/btnSortSize/btnSortTime/about 均有 WS_TABSTOP，list/tree/tab 无）；焦点在无 WS_TABSTOP 的 list 上时返回下一个有样式的控件；`if (next)` 判空兜底（:115）正确；GetKeyState 取 Shift 实时状态可能与排队消息错位（理论边角）。[I] 实际循环顺序未活体验证。
- **加速键在模态弹窗期归属**（清单项 2）：安全——模态期主循环不运行，加速键表不参与模态泵，F5/Ctrl+A 不会注入被禁用的处理器（亲读 :109-119 确认无其他泵入口）。
- **修复草案**：消息循环里对 Tab/Enter/Escape 改用 `IsDialogMessageW(hwnd_, &msg)`（注意它会吞掉部分按键消息，需放在 TranslateAccelerator 之后）；或加速键处理前检查 `GetFocus()` 是否 EDIT 且 Ctrl+A 转发 `EM_SETSEL`。
- **置信度**：Confirmed（代码）；视觉后果 [I]。

### 发现 9（P3·移交后端）“VolumeIndex: C: indexed 27 entries” —— 索引构建实质失效

- **证据**（用户日志只读核查 `%LOCALAPPDATA%\MiniSys\logs\minisys.log`）：`2026-10-06 14:45:48 [INFO] VolumeIndex: C: indexed 27 entries (serial b22dc436, journal 1d89e0c952048a2)`（v2.2 会话，JunkScanner 触发，其后 tab=0 扫描仍花 30s——索引名存实亡、逐规则回退 FastWalk）与 `2026-10-06 15:55:38` 同样 27 entries（v2.3 会话，**UI 触发链成立**：`MainWindow.cpp:449-453` OnTabChanged 自动建索引 → `BuildIndexAsync`）；构建耗时约 31s（15:55:07 启动 → 15:55:38 完成），27 项对应 31 秒，枚举循环疑似整卷跑完但记录解析/推进异常（`VolumeIndex.cpp:262-307`，如 `med.StartFileReferenceNumber = nextFrn` 推进或内层 :280 记录长度校验 break 跳批）。
- **UI 侧影响**：搜索页显示"索引就绪：27 项"（`MainWindow.cpp:561-562`）、搜索基本无结果——功能级失效但无稳定性风险。根因在 VolumeIndex 枚举/解析（后端），**移交 03 后端/性能线深查**；本轮仅确认 UI 触发路径与现象一致性。

---

## 3. 已检查且安全清单（本轮逐项核实）

| 审计清单项 | 结论 | 证据要点 |
|---|---|---|
| 定时器生命周期/销毁 | 安全 | `MainWindow.cpp:238` SetTimer（同 id 重设即防抖重置，EN_CHANGE 风暴不累积）；`:324` 触发即 KillTimer；WM_DESTROY 不需显式 KillTimer（窗口销毁自动回收，TimerProc=nullptr 无悬挂回调）；窗口销毁后 WM_TIMER 不会派发 |
| NM_CUSTOMDRAW 返回值约定 | 正确 | `MainWindow.cpp:171-172` CDDS_PREPAINT→CDRF_NOTIFYITEMDRAW；`:173-188` ITEMPREPAINT 改 clrText 后 return CDRF_DODEFAULT；其余 stage 落到 `:233` return 0（=CDRF_DODEFAULT，对 POSTPAINT 类正确）；每 item `dynamic_cast<ListTabPresenter*>` 空指针安全（History/FolderTree 下 → nullptr → 默认色）；`ItemAtRow`（`Presenters.cpp:45-51`）lParam 越界返回 nullptr |
| 双击路由 iItem=-1 | 安全 | 搜索页 → `OnOpenLocation`（`MainWindow.cpp:957` GetNextItem 返回 -1 即 return）；其余 → `OnItemActivated(-1)` → `ItemAtRow(-1)` row<0 返回 nullptr（`Presenters.cpp:46`） |
| 右键菜单重派发 | 安全 | `MainWindow.cpp:1101` cmd==0 已挡；IDM_LIST_*(3002-3007) 与 IDC_*(2001-2023)/IDM_CTX_DELETE(3001) 无冲突（resource.h:38-49）；MAKEWPARAM(cmd,0) 的 HIWORD=0 不会误入 EN_CHANGE/BN_CLICKED 前置分支（:237/241）；SendMessage 重入各处理器均有界检查 |
| FormatCount 千分位 | 正确 | `MainWindow.cpp:52-61`：0→"0"；100→"100"；1234567→"1,234,567"（逐位验算）；入参 size_t 无负数；无格式串失配 |
| FormatW 参数匹配 | 正确 | 本轮新调用点逐一对账（`MainWindow.cpp:561-566/600/626/784-785/795/802/875-899`、`Presenters.cpp:353/368/397/479/1046-1050`）——%zu/%llu/%d/%s 与实参全匹配；`StringUtils.cpp:62-74` 失配返回空串不越界；所有 `.c_str()` 临时均在完整表达式内消费（含 `MainWindow.cpp:914` 三元分支，v2.1 已认定安全写法） |
| Apps 迁移确认 TaskDialog 字符串生命周期 | 安全 | `MainWindow.cpp:820-836`：mainInstr/confirm 为块内局部 wstring，TaskDialogIndirect 同步调用期间存活（唯一例外是 ShowItemInfo，见发现 3） |
| TDF_USE_COMMAND_LINKS 与默认按钮 | 无冲突 | `MainWindow.cpp:825-831`：自定义按钮 IDOK/IDCANCEL，nDefaultButton=IDCANCEL 指向有效按钮 ID（非 MB_DEFBUTTON2 语义混用）；验证勾选 TDF_VERIFICATION_FLAG_CHECKED 用法正确 |
| LVN_ITEMCHANGED 的 taskMode_ 守卫 | 方向正确 | `MainWindow.cpp:153` busy 态全抑制；None 态风暴归发现 7（性能），无崩溃 |
| SetTaskBusy 入口覆盖 | 基本完备 | OnScan(:544)/OnExecute(:854)/OnPlanDone(:868)/OnScanDone(:572)/OnEmptyQuarantine(:925)/WM_APP_TASK_STARTED(:315)——各 Started 均有对应 DONE 复位（除发现 4/5 两缺口） |
| OnScanDone 用 CurrentTab（R-7） | 等效安全 | 扫描期 tab 被矩阵禁用（:649），无法错页（见 §0 回归表） |
| 撤销消息时序（R-5 新路径） | 正确 | `Presenters.cpp:599-603` records 按值捕获；进度 (done-1)*100/size 无除零（records 非空先验）；完成 Post OP_DONE；除发现 4 的排序边角 |
| OperationLog 并发 | 安全 | Append/UpdateStatus/UpdateStatusBulk/LoadAll 全部 `mu_` 互斥（`OperationLog.cpp:214-262`），UI 与 worker 并发安全 |
| 单实例 | 安全 | `main.cpp:12-20` CreateMutex+GetLastError 模式正确；提前返回路径句柄由进程退出回收（良性） |
| 消息路由/布局重入 | 安全 | StaticWndProc WM_NCCREATE→GWLP_USERDATA 不变；WM_SIZE 早于 OnCreate 时 h_ 为空 HWND 仅致 SendMessage 族静默失败（R-8 残留级）；OnTabChanged→OnSize（:486）不回调主 WndProc |
| 加速键模态期归属 | 安全 | 模态期主循环不运行（见发现 8 第 3 点） |
| Icons/Layout（v2.1 已审，本轮抽查 v2.2 DPI 改动） | 无新崩溃级问题 | `Layout.cpp:12/17-23` GetDpiForWindow 动态加载有 96 兜底；负宽度 SetWindowPos 无害 |

---

## 4. 被推翻的假设 / 已验证无问题

1. **"OnExecute 确认模态期间防抖定时器触发 → 计划基于悬垂 items 引用构建（UAF）"**——机制上 `items`（`MainWindow.cpp:724`）是对 vector **对象**的引用，Refresh 的移动赋值只换内部缓冲，引用不失效；且模态期属主被禁用，query 文本不可变 → 同 query 确定性重放同内容同序，索引又不可能并发变更（无任务可启动）→ **无实害**。仅 ShowItemInfo（发现 3）真悬垂。
2. **"LVN_ITEMCHANGED 风暴会触发 worker 竞争"**——证伪：矩阵在 busy 态禁用 list，风暴只发生在 None 态，纯粹是 UI 性能问题（发现 7）。
3. **"OnTabChanged 自动建索引会与 OnScan 的扫描竞争 SetTaskBusy"**——证伪：tab 在 busy 态禁用，TCN_SELCHANGE 不可达；worker 存 None 与 UI 处理 SCAN_DONE 之间的空档被消息队列 FIFO 封死（SCAN_DONE 先于任何后续用户消息入队）。
4. **"搜索防抖 EN_CHANGE 风暴导致 SetTimer 重入/堆积"**——证伪：同 id SetTimer 重置计时，单定时器；且搜索期间不泵消息，无重入。
5. **"IDM_LIST_* 与 IDC_* 存在命令 ID 冲突"**——证伪（数值域隔离，§3 表）。
6. **"执行期间列表右键菜单可重入危险路径"**——证伪：busy 态 list 禁用，NM_RCLICK 不可达。

---

## 5. 未验证 / 局限

1. **无 GUI 活体**：所有卡顿时长（发现 6/7）为机制推算；Tab 循环顺序、Ctrl+A 劫持的实际观感标 [I]。未运行任何程序/构建/测试（MiniSys.exe 正在用户桌面运行，全程未触碰）。
2. **发现 1/2/3 的竞争窗口**：机制均 Confirmed，但未做 TSAN/ASAN 或压力复现（工具链与只读约束）；触发概率基于代码路径与时序推理。
3. **发现 4 的消息排序竞争**：无法在不运行的情况下测定线程启动延迟与 worker 快路径的相对时序，标 Speculative→Likely 下限。
4. **27 entries 根因未深查**（属 VolumeIndex 枚举/解析，后端范围）：仅确认 UI 触发链、日志证据与"31 秒建 27 项"的异常量级，移交 03 后端线。
5. DelegateOp/SystemRestore/QuarantineOp 内部（R-4 后端部分）本轮未复核；Icons.cpp/Settings/Json 等 v2.2 改动中非本次清单焦点部分仅抽查。
