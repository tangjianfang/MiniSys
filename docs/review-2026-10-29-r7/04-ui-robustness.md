# r7-04 UI 稳定性（崩溃风险 + 状态一致性）分报告

- 日期：2026-10-29 · 基线：master `6621d92`（v2.23，工作树仅 build_number.txt/docs 有未提交改动，src 与提交一致——已核对 git status 与逐文件读取）
- 方法：全量读码（MainWindow.cpp 2482 行 / Presenters.cpp 1061 行 / Controls / Layout / CommandPalette / SessionService.cpp 1193 行 / OperationLog / QuarantineOp / SecretStore / Icons / main / StringUtils）+ 最近 4 提交逐条 `git show` diff 审计 + **离线复现实验**（自建 vcvars64+cl 小工程，位于 `.tmp-review/r7/04-ui-exp/`）+ 用户真实数据只读取证（`%LOCALAPPDATA%\MiniSys` 的 history.jsonl / minisys.log / 注册表 / C:\MiniSys.Quarantine 目录快照）。
- 约束遵守：未启动/未结束 MiniSys.exe；未改源码、未提交、未跑黑名单脚本；用户数据只读；实验产物全部在 `.tmp-review/r7/`。

---

## 0. 结论：线索假设证实/证伪表

| # | 线索假设 | 结论 | 证据 |
|---|---|---|---|
| H1 | SessionService.cpp:919 RunEmptyQuarantine 标记后 status 仍 Success、HistoryTypeLabel/HistoryRiskLabel 仍显示"已隔离 · 成功"/"可还原" | **证实（次级缺陷 U-2）** | SessionService.cpp:963-965（UpdateStatusBulk 只改 note）；Presenters.cpp:1001-1019（两函数均不识别 `[已释放]` 前缀）；Presenters.cpp:1040-1043（UndoSelected 资格判定同样不看 note → 已释放记录仍算 eligible，撤销必失败"隔离区中找不到该项目" QuarantineOp.cpp:137-139） |
| H2 | （用户投诉的直接根因是标记/标签协议） | **部分证伪——主因另有其人（U-1）** | 用户真机 history.jsonl 共 142 条，`[已释放]` note = **0 条**；而 C:\MiniSys.Quarantine 实际只剩一个 0 字节尾点文件 `tdot.`（Oct 6 创建）。即"清空"在真机上**从未成功标记过任何记录**——根因是 U-1 的 fs::remove_all 部分失败→全盘不标记，标签协议（U-2）是标记成功后才会暴露的次级问题 |
| H3 | OnPlanDone 在非历史页（Ctrl+K 面板）触发清空后，历史页/状态栏最终是否一致 | **最终一致（代码路径核清）** | OnPlanDone→`ActivePresenter()->Refresh()` 只刷当前页，但 OnTabChanged→Refresh()（MainWindow.cpp:1667）保证切回历史页时重载；状态栏 pane1 由 OperationLog Generation 触发重算（SessionService.cpp:979-1008）+ OnPlanDone→UpdateStatusBar（MainWindow.cpp:2244）。此链无缺陷——**前提是标记发生（被 U-1 打断）** |
| H4 | v2.23 TVM_ITEMEXPANDING 里同步扫描目录 | **证伪** | Presenters.cpp:691-715：UI 线程只插"… 加载中"占位符，扫描在 worker（SessionService.cpp:531-582 FolderChildrenAsync，StartTask）。但派生出 U-4（该任务不进 busy 矩阵、不可取消） |
| H5 | 已修复项复核（v2.16d SupplyDispInfo / R-1 RootsDlg WM_QUIT / R-2 GetDlgItem / V-R6 pane2 残留 / v2.19a SS_OWNERDRAW 擦背景） | **全部确认在位** | Presenters.cpp:190-214、975-990（行/列/cells_ 三重界检）；MainWindow.cpp:1464-1474（WM_QUIT 转发）；MainWindow.cpp:1337/1377（GetDlgItem(h,1001)×2）；MainWindow.cpp:1670（切页 SetProgressText(L"就绪")）；MainWindow.cpp:261-267（DrawInfoText 每次自绘 FillRect+FrameRect） |
| H6 | （自查中途假设）"真机记录全部无签名，v2.20 签名链修复未生效" | **自我证伪** | 我初版脚本用了错误字段名 `sig`（实际 JSON 字段为 `_sig`）；复检 49/142 已签名，首条 20261015-004058（v2.20 之后），今日 01:58 的 25 条均带签名——签名链在真机有效 |
| H7 | Ctrl+K 命令面板入口 | **新发现回归（U-3）**：v2.22 起 Ctrl+K 加速键被静默截断失效（详见下） | MainWindow.cpp:370-376 与 d574af3 diff |

**本轮无新崩溃级缺陷**（与"唯一 dump 为 10-12 v2.16d"一致）；核心问题全部落在**状态一致性**，且用户投诉的根因已定位并用实验复现。

---

## 1. 摘要（按风险排序 Top）

1. **U-1（P0·Confirmed·已实验复现）**：清空隔离区遇到任一"普通路径删不掉"的文件（现场为尾点名 `tdot.`）时，`fs::remove_all` 删光其余内容却返回 ec=145 → 整盘不标记 `[已释放]` → 历史列表、状态栏"隔离区: N 项 / X"全部不变，而磁盘空间实际已释放。**这就是用户投诉"清理隔离区，完成之后，下面的列表信息还未清空"的直接根因**，且在同一文件删除前每次清空都会复现。
2. **U-2（P0·Confirmed）**：即使清空成功，历史行仍显示"已隔离 · 成功 · 可还原"，撤销预检仍放行 → 撤销必失败。标记协议（note 前缀）与展示/撤销判定三处脱节。
3. **U-3（P1·Confirmed）**：v2.22 引入 Ctrl+I 时 `CreateAcceleratorTableW(acc, 3)` 没跟着改 4 → **Ctrl+K 命令面板自 v2.22 起失效**，而空态提示/页描述/About 对话框仍到处宣传 Ctrl+K。
4. **U-4（P1·Confirmed）**：v2.23 新任务种类 FolderExpand 未接入 busy 矩阵：展开大目录（全子树并行测尺寸，分钟级）期间 UI 呈"空闲态"，按钮全可用但所有动作弹"已有任务在进行中"，且无任何取消入口。
5. **U-5（P1·Confirmed·有现场实锤）**：单元测试 `TrustChain.AppendSignsRecordsAndTamperBreaksSignature` 未重定向历史路径，把假记录直接写进用户真实 history.jsonl——真机已出现幽灵行"回收站删除 · 成功 … C:\some\file.txt → C:\MiniSys.Quarantine\x（可还原）"（今日 02:11 落盘）。
6. 其余：U-6 树展开重复插入（窄竞态）、U-8 TREE_CHILDREN 无护栏+退出泄漏、U-10~U-12 低危观察项。

---

## 2. 发现清单

### U-1【P0】清空隔离区"部分失败→全盘不标记"，UI 全部滞留旧状态 — Confirmed（含实验复现）
- 位置：`MiniSys/src/core/SessionService.cpp:929-943`（RunEmptyQuarantine 的逐盘循环）、`:946-967`（标记逻辑）。
- 触发：`C:\MiniSys.Quarantine`（或任一盘的隔离根）里存在普通 Win32 路径删不掉的条目。现场实物：`tdot.`（尾点名，Oct 6 21:56 生成，0 字节）。本机实验（`.tmp-review/r7/04-ui-exp/`，MSVC 14.44 同工具链）：
  - `CreateFileW("\\\\?\\...\\tdot.")` 成功创建；`DeleteFileW("...\\tdot.")`（普通路径）err=2（路径规范化剥掉尾点→名字永远对不上）；`\\\\?\\` 前缀可删。
  - 对含 `a.txt`、`sub\b.txt`、`tdot.` 的目录执行 `fs::remove_all(root, ec)`：**返回 uintmax_t(-1)，ec=145（ERROR_DIR_NOT_EMPTY），普通文件全删、仅剩 `tdot.`** —— 与真机 C:\MiniSys.Quarantine 现状（只剩 `tdot.`）逐字节吻合。
- 后果链：`if (ec) { ++rpt.failed; ... continue; }` → `emptiedDrives` 为空 → `UpdateStatusBulk` 不执行 → 0 条 `[已释放]`；历史行原样（"已隔离 · 成功"）；状态栏 pane1 仍"隔离区: 73 项 / X GB"（真机 142 条中 73 条 QUARANTINE SUCCESS 全部计数）；info 只显示"✓ 完成: 成功 0 · 跳过 0 · 失败 1"和一个"× 清空 … 失败（文件仍在，可稍后重试）"弹窗——但用户视角即"完成了、列表却没清空"。二次伤害：真机隔离区内容确已删光，状态栏虚报占用会**永久**持续（重试也永远失败在同一文件上）。
- 附注：该路径未走项目自己的 `LongPath()` 前缀（QuarantineOp 等其他路径都走了）。
- 修复草案：① 清空改逐条目删除（构造 `LongPath`/`\\\\?\\` 前缀的 `fs::remove` 循环），统计"剩余字节数/剩余条目"作为成败判据（剩余 < 阈值即视为已清空并标记，剩余项名写进 note）；② 或最低限度：`remove_all` 失败后重试 `LongPath(root)` 前缀版本；③ 无论哪种，把每盘"删除了什么、剩了什么"写入日志（本次取证全靠推断，日志里 0 行）。
- 置信度：**Confirmed**（代码 + 真机数据 + 同机实验三方一致）。

### U-2【P0】`[已释放]` 协议与展示/撤销判定脱节 — Confirmed
- 位置：`SessionService.cpp:963-965`（status 保持 Success、isReversible 不动，仅 note 加前缀）；`ui/Presenters.cpp:1001-1008`（HistoryTypeLabel → "已隔离 · 成功"）；`:1010-1019`（HistoryRiskLabel → "可还原"）；`:1037-1043`（UndoSelected 资格：`isReversible && status==Success` → eligible）。
- 触发：任何一次成功的清空隔离区。
- 后果：已永久删除的条目宣称"可还原"；用户点撤销 → `QuarantineOp::UndoPaths`（QuarantineOp.cpp:137-139）失败"隔离区中找不到该项目（可能已被清空）" → 报告成批失败项。状态语义错误正是用户"低级错误"观感的来源之一。
- 修复草案：集中谓词 `bool IsReleased(const OpRecord&)`（note 前缀判断一处定义），HistoryTypeLabel 输出"已隔离 · 已释放"、HistoryRiskLabel 输出"已释放（不可还原）"、UndoSelected 预检排除；更好的做法是给 OpStatus 加 `Released` 终态（需迁移旧 note，量小）。
- 置信度：**Confirmed**。

### U-3【P1】Ctrl+K 加速键自 v2.22 失效（数组 4 项、计数 3） — Confirmed
- 位置：`MiniSys/src/MainWindow.cpp:370-376`：
  ```cpp
  ACCEL acc[] = { {FVIRTKEY,VK_F5,IDC_BTN_SCAN}, {FVIRTKEY|FCONTROL,'A',IDC_ACCEL_SELECTALL},
                  {FVIRTKEY|FCONTROL,'I',IDM_LIST_INVERT}, {FVIRTKEY|FCONTROL,'K',IDC_ACCEL_PALETTE} };
  HACCEL hAccel = CreateAcceleratorTableW(acc, 3);   // ← Ctrl+K 被截断
  ```
- 引入：`d574af3`（v2.22）diff——只插入了 Ctrl+I 一行，计数 3 未改（v2.10~v2.21 时 3 项=3 正确，Ctrl+K 工作）。
- 后果：命令面板只剩无快捷键入口（Ctrl+K 无效）；而 `MainWindow.cpp:636/1624/2281` 等处的空态提示、页描述、About 仍承诺"Ctrl+K 命令面板"。注意 `WM_KEYDOWN` 消息循环没有 Ctrl+K 备份分支。
- 修复：`CreateAcceleratorTableW(acc, static_cast<int>(std::size(acc)))`；加 grep 闸禁止字面量计数（见 §5）。
- 置信度：**Confirmed**（diff + 现码）。

### U-4【P1】FolderExpand 任务游离于 busy 矩阵之外（v2.23 回归） — Confirmed
- 位置：`ui/Presenters.cpp:691-715`（OnItemExpanding 直接调 `FolderChildrenAsync`，既不 `NotifyTaskStarted` 也不通知 MainWindow）；`SessionService.cpp:531-582`（FolderExpand 任务体：`DirectorySizeParallel(kid, 4)` 对每个子目录**全子树**测尺寸，大目录分钟级）；`MainWindow.cpp:1883-1948`（SetTaskBusy 只认 Scanning/Indexing/Executing）。
- 触发：文件夹分析页展开一个大目录（如 C:\Users、node_modules 级目录）。
- 后果：worker 被占用期间 UI 完全呈空闲态（按钮可点、进度条隐藏、标题无"⟳"）；点扫描/执行/搜索全部落到"已有任务在进行中，请稍候"分支；`OnScan` 的取消分支只认 `IsScanning()`（MainWindow.cpp:1734），FolderExpand 无法取消——一个意外展开的大目录会把所有功能锁死数分钟且无提示。TIMER_SEARCH_RETRY 等"taskMode_==None 才动"的逻辑也会因此空转重试。
- 修复草案：OnItemExpanding 成功启动后 `SessionService::Instance().NotifyTaskStarted()` + MainWindow 为 FolderExpand 增加轻量模式（禁树/扫描/执行，可切换页，状态栏显示"展开文件夹…"，把扫描按钮变取消——`FolderChildrenAsync` 的循环已检查 `cancelScan_`，具备取消条件）；或将 DirectorySizeParallel 换成"先返回条目、尺寸惰性补"的两段式。
- 置信度：**Confirmed**。

### U-5【P1】单元测试写真实用户 history.jsonl（已有现场实锤） — Confirmed
- 位置：`MiniSysTests/tests/GuardRailsAdversarialTests.cpp:264-288`（`AppendSignsRecordsAndTamperBreaksSignature` 全程无 `SetHistoryPathForTesting`，`OperationLog::Instance().Append(r)` 直写 `%LOCALAPPDATA%`）；同文件 `:290-358`（`SignUnsignedLegacyQuarantineBoundary` 在 **C:\ 根**真实创建 `MiniSys.Quarantine\__minisys_unit_test__` 与 `MiniSys.Quarantine-evil\b.txt`，断言失败路径上清理代码位于 ASSERT 之后执行顺序存疑）。
- 现场实锤：真实 history.jsonl 尾部有 `20261029-021106-5 DELETE SUCCESS source=C:\some\file.txt target=C:\MiniSys.Quarantine\x` —— 与测试第 272-273 行的字面量逐字一致，今日 02:11 落盘（当时正在跑 MiniSysTests，minisys.log 无对应会话）。该行已签名且 `reversible:true`（OpRecord 默认，Operation.h:29），在历史页显示为**"回收站删除 · 成功 …（可还原）"的幽灵行**——既是数据污染又是状态谎报。
- 后果：用户可见假记录；`信任并签名旧记录`/统计类功能被污染；与 v2.13c V-R5（同类污染已修过一次）复发同型。
- 修复：该测试同样使用 temp 目录 override（RAII 恢复）；C:\ 根目录测试改到 `%TEMP%` 模拟卷；测试 main 里加"AppData 真实文件快照对比"守卫（见 §5）。
- 置信度：**Confirmed**。

### U-6【P2】树展开重复插入（ApplyChildren 无去重护栏） — Likely
- 位置：`ui/Presenters.cpp:718-730`（ApplyChildren 无条件插入，`childrenLoaded_.insert` 不阻止二次插入）；`OnItemExpanding` 的防抖只看 `childrenLoaded_`（:696），无"在途请求"集合。
- 触发（窄窗口）：同一节点的展开点击消息先于 worker 的 TREE_CHILDREN post 入队、但派发发生在 worker `taskKind_.store(None)` 之后（消息 FIFO + 派发时序交错，如 UI 正忙于插入/重绘）→ 第二次 `FolderChildrenAsync` 成功启动 → 两条 TREE_CHILDREN 都插入 → **全量子节点重复两份**。`loadingItem_` 单槽位（:705,722）在交错时还会提前删掉另一节点的占位符（同根因，随本项一并修）。
- 后果：树内容错误（重复行）；用户以此右键"移入隔离区"时可能对同一路径出两份 plan 项（PlanBuilder 有 planHash/同路径拒绝兜底，风险有限）。
- 修复：`std::set<std::wstring> pendingFetches_`（进入时插入、ApplyChildren/失败时擦除，作为二次展开的短路条件）；ApplyChildren 开头 `if (childrenLoaded_.count(parentLower)) return;`；loadingItem_ 改 map<parent,HTREEITEM>。
- 置信度：**Likely**（机制读码成立，时序窗口未活体复现）。

### U-7【P2】WM_APP_TREE_CHILDREN：无 gen/taskMode 校验 + 失败/退出泄漏路径 — Confirmed（低危）
- 位置：`MainWindow.cpp:858-872`（不校验生成号也不看 taskMode_）；`SessionService.cpp:579-580`（`new TreeChildren` 后 `Post` 返回值不检查）；退出路径：WM_DESTROY→PostQuitMessage 后队列里未处理的 TREE_CHILDREN 连同堆负载一起丢弃（Shutdown 只 join 线程，SessionService.cpp:1188-1191）。
- 后果：正常时序下任务串行使陈旧消息罕见（与 U-6 交错时才会出现向重建后/禁用中的树插入）；泄漏量级为每条消息一个 vector，仅退出/Post 失败时发生——不崩溃、不增长。
- 修复：投递封装 `PostOwned`（失败自删，见 §5）；消息处理加 `wp` gen（把 TREE_CHILDREN 加进 SessionService::Post 的 gen-stamped 集合并携带）+ `taskMode_` 早退。
- 置信度：**Confirmed**（存在性），影响评级低。

### U-8【P3】OnPlanDone 弹窗顺序与模态泵重入 — Confirmed（体验级）
- 位置：`MainWindow.cpp:2204-2245`：ShowHint 后**先** `MessageBoxW`（:2238）**后** `p->Refresh()`（:2243）——弹窗关闭前列表一直是旧数据；且模态泵期间 TIMER_IDLE_REFRESH/COUNTDOWN 照常运转（WM_TIMER 不被模态泵挡），可在完成弹窗背后武装/启动空闲刷新，任务矩阵在模态下翻转（无崩溃）。
- 修复：先 Refresh 再弹窗；空闲计时器在 Executing→None 的弹窗期间延迟武装（如弹窗前记时间戳，OnIdleCheck 跳过刚完成的窗口期）。
- 置信度：**Confirmed**。

### U-9【P3】静态标签底色与 v2.23 浅色 chrome 可能不一致 — Speculative（需截图）
- 位置：v2.23 `WM_ERASEBKGND` 把客户区刷成 #F5F6F8（MainWindow.cpp:465-483），但 WndProc 无 `WM_CTLCOLORSTATIC`——搜索行/大文件行的 SS_RIGHT/SS_LEFT 标签（Controls.cpp:107-132）默认底色为系统 BTNFACE 灰，理论上在浅色 chrome 上呈灰块。未启动应用，无法像素确认；v2.23 提交的像素验证只覆盖 chrome 与 pill。
- 修复：WM_CTLCOLORSTATIC 返回 chrome 刷子（HC 主题回退系统色）。
- 置信度：**Speculative**。

### U-10【P3】一次性 GDI/字体资源不回收（记录备查）
- `Controls.cpp:164`（`CreateFontIndirectW(&ncm.lfMessageFont)` 主字体）、`:44-47`（1×22 ImageList）进程级一次创建不销毁；`MainWindow.cpp:104-118` BoldUiFont、`:266` hairline 静态刷同理。均为进程生命周期常驻，非循环泄漏，无稳定性影响。owner-draw 热路径（DrawTabItem/DrawPrimaryButton/zebra）的画笔/刷子全部成对删除，**已核对无泄漏**。
- 置信度：**Confirmed**（无害）。

### U-11【P3】历史页复用带 LVS_EX_CHECKBOXES 的共享列表 — Confirmed（无害残留）
- 历史页 SupplyDispInfo 不回状态（Presenters.cpp:989）、X-41 自绘只对 ListTabPresenter（MainWindow.cpp:572-590）→ 历史行左侧留一个不渲染的死复选框槽；Space 键在历史页列表焦点下被消息循环吞掉（MainWindow.cpp:394-406 dynamic_cast 失败仍 `continue`）。`SetBatchUpdate`/`InBatchUpdate` 自 v2.15 后无置位者（守卫残留）。均无功能危害，清理即可。

---

## 3. 「操作 × 应刷新 UI 元素 × 实际是否刷新」矩阵

图例：✓=已刷新 ✗=未刷新/错误 △=条件性/顺序问题 −=不适用

| 操作↓ \ 元素→ | 活动页列表 | 非活动页列表(快照) | 历史页列表 | 状态栏 pane1 隔离区用量 | pane2 进度 | 执行按钮计数/勾选数 | info strip | 备注 |
|---|---|---|---|---|---|---|---|---|
| 执行（隔离/迁移/委派/回收站） | ✓ OnPlanDone→Refresh，RunPlan 已按路径剪除成功项（SessionService.cpp:887-900） | △ 快照滞留，切页时 OnTabChanged→Refresh 补上 | △ 非历史页发起时靠切页补；历史页在活动时 ✓ | ✓ Append→gen++ →重算 + UpdateStatusBar | ✓ | ✓ SetTaskBusy(None) 内 UpdateExecButton | ✓ ShowHint | **△ 顺序：失败/跳过弹窗在 Refresh 之前（U-8），弹窗开着时列表仍旧** |
| 清空隔离区（成功路径） | − | − | ✓ 刷新但**标签错**（U-2："已隔离 · 成功/可还原"） | ✓ 变空 | ✓ | − | ✓ | |
| **清空隔离区（部分失败路径=真机现状）** | ✗ | ✗ | ✗ **完全不刷新**（U-1：0 条标记） | ✗ **仍显示 N 项/X** | ✓ | − | △ 只报"失败 1" | **用户投诉点** |
| 撤销（批量） | −（被还原项不自动回列表，需重扫——设计如此） | − | ✓ Refresh（Reverted 标签正确） | ✓ gen++重算 | ✓ | − | ✓ | |
| 文件夹右键移入隔离区 | ✓ OP_DONE→Refresh 重建树 | − | △ 切页补 | ✓ | ✓ | − | ✓ | NotifyTaskStarted 锁矩阵 ✓（gen 门在位） |
| 搜索完成 | ✓（SEARCH_DONE gen 门 + Refresh） | △ 切页补 | − | − | ✓ | ✓（仅活动页） | ✓ UpdateSearchStatus | |
| 校验/空闲维护剪枝 | ✓ 仅活动页渲染（R-1 守卫在位） | ✓ 静默剪枝+落盘缓存 | − | − | ✓ | ✓ 仅活动页 | ✓ | |
| **树懒展开（v2.23）** | ✓ ApplyChildren | − | − | − | △ SetProgress"展开文件夹…"，完成置空 | **✗ 全程无 busy 态（U-4）** | △ 仅失败时提示 | 按钮全可用但一切动作被拒 |
| Ctrl+K 面板 | − | − | − | − | − | − | − | **入口本身失效（U-3）** |
| 复选状态（勾选/反选/全选） | ✓ map 驱动+RedrawItems；跨排序/刷新/剪枝按路径保留（L-1 契约在位） | − | − | − | − | ✓ UpdateExecButton | − | |

---

## 4. 近期 4 提交逐条复核

| 提交 | 内容 | 结论 |
|---|---|---|
| `3c5a8f4` v2.21 | 单强调色按钮(BS_OWNERDRAW)、工具栏 8px 网格重排、info card、23px 行距(1×22 ImageList)、删死按钮 | **未发现缺陷**。自绘里 GetWindowTextW/Wide 文本、GDI 成对释放；布局纯 SetWindowPos 无消息回环；删除的 IDC_BTN_SORT_* 无残留引用（UiHandles/Layout/资源三处同步删）。遗留 U-9/U-10（一次性资源，无害） |
| `484d774` v2.21a | NM_CUSTOMDRAW zebra + 提示文案 | **未发现缺陷**。zebra 画刷成对删除、HC 豁免、`dwItemSpec&1` 奇偶正确；ITEMPREPAINT 返回 CDRF_DODEFAULT 与 X-41 自绘复选框共存无冲突 |
| `d574af3` v2.22 | 反选（IDM_LIST_INVERT/Ctrl+I/菜单/面板）+ owner-drawn tab strip | **1 个缺陷：U-3（acc 计数 3 截断 Ctrl+K，本提交引入）**。InvertChecked 语义与 ToggleCheckAt 一致（Presenters.cpp:254-264）；DrawTabItem 内 TCM_GETITEMW 为只读查询无重入；移除旧 NM_CUSTOMDRAW bold 分支无残留 |
| `6621d92` v2.23 | 浅色 chrome + 惰性展开树 + FolderTree 缓存 stale 判定 | **2 个缺陷：U-4（FolderExpand 不进 busy 矩阵）、U-6/U-7（TREE_CHILDREN 无护栏/泄漏路径）**；U-9 疑似视觉待验。缓存 stale 判定（TryLoadCachedResults 的 pre-v2.18 剔除）逻辑正确且真机日志已生效（"FolderTree cache is pre-v2.18 … discarding"，10-29 01:29 实录） |

---

## 5. 系统性加固方案

1. **统一任务注册表**（治 U-3/U-4 型"新增时漏接线"）：`struct TaskSpec { TaskKind kind; TaskMode uiMode; UINT doneMsg; bool lockList; const wchar* busyTitle; }` 一张表；StartTask/WM_APP_TASK_STARTED/SetTaskBusy/DoneMessageFor 全部从表驱动。新增任务种类不改五处，改一处。
2. **安全投递封装**（治 U-7）：`template<class T> void PostOwned(HWND, UINT msg, std::unique_ptr<T> p)` —— `PostMessageW` 失败自删；`Shutdown()` 时对在途 payload 登记回收（或负载持有 shared_ptr、窗口侧 weak 判活）。
3. **单一 UiRefresh 入口**：`RefreshUi(int scope)` 位集（ActiveList/HistoryRows/StatusBar/ExecButton/InfoStrip），所有 *_DONE 处理统一调用；规则："弹窗永远最后"。可把 §3 矩阵固化为断言表。
4. **`[已释放]` 集中谓词或显式状态**（治 U-2）：`IsReleased(const OpRecord&)` 一处定义供 label/risk/undo 三处共用；中期加 OpStatus::Released。
5. **删除一律过 LongPath**（治 U-1）：清空改逐条目 `LongPath` 删除 + 剩余量判据；对 `fs::remove_all`/`DeleteFileW` 的裸调用建 grep 闸。
6. **测试闸**（治 U-5）：MiniSysTests main 启动时对 `%LOCALAPPDATA%\MiniSys`（history/cache/settings）做快照，结束逐字节对比，污染即 FAIL；grep 闸禁 `CreateAcceleratorTableW(…, <数字字面量>)`（强制 `std::size(acc)`）。
7. **编译器加固**：`/WX /W4 /analyze`（r6 已提，仍未落地——本次 U-3 类"参数与数组不一致" analyze 可抓）；UI 回调跨 C 边界禁止异常（现状靠 StartTask 全 catch，已核）。
8. **无头回归思路**：① 直接 `SendMessageW(hwnd_, WM_APP_OP_DONE/WM_APP_TREE_CHILDREN…)` 打乱序消息驱动 WndProc，断言 busy 矩阵终态收敛（测 gen 门/重入）；② history 协议测试夹具里放尾点/保留名文件，断言"清空后标记数>0 且剩余项入 note"（直接回归 U-1）；③ 加速键表大小=数组大小的静态断言（`static_assert` 或单测读表）。

## 6. 建议修复顺序
- **P0**：U-1（清空逐项删除+剩余量判据+日志）、U-2（[已释放] 三处统一）——直接回应今日投诉。
- **P1**：U-3（一行修+闸）、U-4（FolderExpand 进矩阵+可取消）、U-5（测试隔离+快照闸）。
- **P2**：U-6/U-7（pendingFetches_+PostOwned+gen 校验）、U-8（弹窗顺序）。
- **P3**：U-9（WM_CTLCOLORSTATIC，先截图证实）、U-10/U-11（资源清理与残留代码）。

## 7. 已检查且安全清单（抽样记录）
- SupplyDispInfo 双守卫（v2.16d）在位且含 cells_ 界检；`ActivePresenter()` 对 TabCtrl_GetCurSel==-1 的界检（04-R3）在位。
- StartTask 串行 + 异常兜底必发 DONE（04-5）+ CoInitializeEx 配对；taskGen 戳对各 *_DONE 的陈旧丢弃；Shutdown join；-takeover 的 RootsDlg/命令面板双 WM_QUIT 转发（R-1/R-2）。
- 确认弹窗（ConfirmTask/TaskDialog/ShowItemInfo）字符串生命周期安全（04-3 的拷贝修复在位）；OnEmptyQuarantine 的三元临时指针合法（临时活到全表达式末）。
- FormatW 的 `vswprintf_s(r.data(), n+1, …)` 写入 r[n] 的终止符合法（写 null 到 wstring 尾部合法）；全程 %s 均宽字符无宽窄混用。
- VerifyPaths/IdleMaintenance 的活动页渲染守卫（review-04 R-1）与 per-tab VERIFY_DONE（R-4）在位；planHash/逐项复验双闸在位。
- v2.19a SS_OWNERDRAW 擦背景修复在位（DrawInfoText 每次自绘底）；V-R6 切页进度复位在位。
- 历史页 UndoSelected 的 rows_ 与 LoadAll 同步性：所有 Append/Update 均伴随 OP_DONE→Refresh，未发现错位撤销窗口。

## 8. 被推翻的假设 / 已验证无问题
1. "清空后状态栏 pane1 刷新缺失"——**不成立**：gen 缓存机制正确；真机 pane1 不清零是因为标记根本没发生（U-1）。
2. "签名链在 v2.23 又断了"（我方自查中途误判）——**不成立**：字段名读错；49/142 已签名，含今日全部新记录。
3. "TVM_ITEMEXPANDING 同步扫描目录卡 UI"——**不成立**：扫描在 worker；真实问题是 U-4（busy 态缺失）。
4. "ApplyChildren 跨节点占位符误删是常态"——单槽位覆盖需消息交错窗口，正常 FIFO 下难触发，降为 U-6 附带项。
5. "zebra/tab 自绘有 GDI 泄漏或重入"——逐一核对，成对释放、无 SendMessage 引发绘制回环。
6. "v2.21 布局重排引发 resize 递归"——OnSize→LayoutWindow→SB_SETPARTS/SetWindowPos 无回流路径。

## 9. 未验证 / 局限
- 未启动 MiniSys.exe（约束），U-9（静态标签底色）与其余视觉结论待实机截图；U-6/U-7 竞态未活体复现（窗口极窄）。
- 用户"清空隔离区"动作的精确时刻无法从日志确定（RunEmptyQuarantine 零日志）；时间线为推断：01:58 执行 25 项隔离 → 某刻点清空（部分失败）→ 02:00 重启 → 02:11 测试进程写入幽灵行并触碰隔离区目录 mtime。`tdot.` 的最初创建者（Oct 6）无法溯源。
- `build/Latest` 与 v2.23 源的一致性以行为证据（"FolderTree cache is pre-v2.18" 日志行为 v2.23 独有 + exe 内含 RecordKey/HKCU 字符串）佐证，未做逐字节哈希比对（build/ 不在 git 内，无可比对基线）。
- 本轮并行评审代理共用 `.tmp-review/r7/`（exp/ 目录 02:21-02:23 有他人产物）；我的实验隔离在 `.tmp-review/r7/04-ui-exp/`（源码 tdot_probe.cpp、脚本 run*.bat、输出见本报告引文）。
- SessionService 之外的 core（VolumeIndex/FastWalk 等）仅按需抽读，非本轮职责范围。
