# 08 功能完整性与交互完备性评审（第七轮 r7）

> 角色：产品经理 + 资深工程师 · 日期：2026-10-29 · 基线：master `6621d92`（v2.23 build 107）
> charter 原话："全量 review，精细化打磨，不要漏掉一个细节。极致交互体验……100% 测试用例的覆盖，包括 UI"——**重点不是堆新功能，而是把现有功能打磨到交互完备、状态永远一致**。
> 方法：通读 docs/（ARCHITECTURE / DESIGN-v2-实施报告 / REVIEW_2026-10-14-r6 及其 08 分报告 / evolve-log / 需求.txt）+ 全量读 MainWindow.cpp（2482 行）、Presenters.cpp（1061 行）、CommandPalette.cpp、Controls.cpp、SessionService.cpp/.h、QuarantineOp.cpp、OperationLog.h、GuardRails.h、Settings.h、resource.h、rules.json + git diff `2a9d653..HEAD`（v2.20→v2.23 五提交逐个核对）+ 只读读取用户真实数据（history.jsonl 142 条 / minisys.log 1388 行 / settings.json / cache\*.json）。**每条"已核实缺失"均 grep 过**；仓库只读，未启动应用，未运行任何黑名单脚本。

---

## 0. 一页结论

v2.20→v2.23 五个提交做了：r6 修复包（签名链/Logger/复选框/缓存/抢占）、三次视觉迭代、**反选**（用户点单）、**懒展开文件夹树**（用户点单）。r6-08 的 P0 提议（N1 规则包/N2 释放预测/N3 导出/N4 硬链接）**零落地**，N5 只落了"反选"半件。本轮以"完成后用户的下一个动作是什么"为尺子逐场景走查，发现：

1. **一个静默回归**：v2.22 给加速键表加了第 4 项却仍传 `CreateAcceleratorTableW(acc, 3)`——**Ctrl+K 命令面板入口自 v2.22（build 104）起已死**，而 About、搜索空态提示、面板自身文案仍在教用户按 Ctrl+K（MainWindow.cpp:370-376，git diff 铁证）。
2. **一条状态谎言**：清空隔离区后历史记录 note 标 `[已释放]`，但状态列仍显示"**可还原**"、撤销预检仍把它计入 eligible——用户点撤销只会得到运行期失败。这正是用户投诉的"清空隔离区后状态不同步"家族里**仅剩的一条未收口线**（用量归零、历史 note、列表刷新已修好）。
3. **一个诚实性缺口**：取消的扫描以"扫描完成: N 项"收场、异常中断的空结果页显示"✓ 未发现可处理项，系统状况良好"。
4. **一处测试再次污染真实数据**：v2.20 新增的 `TrustChain.AppendSignsRecordsAndTamperBreaksSignature` 未重定向 history 路径，把 `C:\some\file.txt` 测试记录写进了用户真实 history.jsonl（2026-10-29 02:11:06 实锤在案）；Logger 无重定向机制，历史日志混有测试期 "Scanner threw: boom" ERROR。
5. v2.23 懒展开**无进度、无取消**：展开 C:\Users 这类目录时全 UI 只剩"已有任务在进行中"。

核心判断不变：这个产品缺的不是功能，是**把已有零件接完**。Top 10 修复全部是 S 级接线。

---

## 1. 现有功能清单表

成熟度：**成熟**（端到端可用）/ **可用**（能跑，覆盖或体验有缺口）/ **骨架** / **孤儿**。"状态同步完整性"= 该功能的动作完成后，所有相关视图（列表/状态栏/历史/缓存/磁盘）是否同步到位。

### 1.1 六 Tab 功能（v2.23 现状）

| # | 功能 | 入口 | 成熟度 | 状态同步完整性 |
|---|---|---|---|---|
| 1 | 垃圾规则扫描（rules.json 27 条：15 Safe + 7 Cautious + 3 Advanced/委派 + 2 InfoOnly；外置+校验+降级可见化） | 垃圾清理 Tab「扫描」/F5 | 成熟 | ✅ 仪表盘/降级提示/缓存齐全 |
| 2 | 开发构建缓存扫描（bin/obj/.vs/ipch…；项目标记约束、24h 跳过、DirSizeCache、用户自定根 RootsDlg） | 垃圾清理 Tab | 成熟 | ✅（今日实测 66 artifacts/3s） |
| 3 | 回收站清空（伪项 `$RECYCLE.BIN`，不可逆、默认不勾） | 垃圾 Tab 特殊项 | 成熟 | ✅ 有独立 OpRecord |
| 4 | 多卷索引（USN+Walk 回退+磁盘缓存+增量；asInvoker 下 C: USN 拒绝→45s walk，今日日志实证） | 搜索 Tab 自动/「重建索引」 | 成熟 | ✅ |
| 5 | 即时搜索（多词 AND、通配符、folder:/file:/ext:、防抖 200ms、1000 行预算、全行并行补大小） | 文件搜索 Tab | 成熟 | ⚠️ 执行后磁盘缓存不回写，幽灵行重启后短暂复活（见 G-12） |
| 6 | 快速筛选 16 预设（勾选态标记当前项；索引 busy 时挂起并自动应用） | 「快速筛选 ▾」+ 命令面板 | 成熟 | ✅（busy 挂起提示是本轮最佳交互范例） |
| 7 | 搜索结果执行（勾选→隔离）+ 双击定位 | 文件搜索 Tab | 成熟 | ✅ |
| 8 | 大文件扫描（minMB/扩展名/盘符、topN 200、6 列排序、虚拟列表） | 大文件 Tab | 成熟 | ✅ |
| 9 | 三级去重（size→头 64KB→全量 SHA-256，[保留]最新/预选其余） | 大文件 Tab | 成熟 | ⚠️ 硬链接盲（N4 继承未做） |
| 10 | 应用迁移（复制+删源+Junction/Symlink、四步预检、还原点、回滚、迁移后自检、UWP 排除） | 应用迁移 Tab | 可用 | ⚠️ Interrupted 无恢复向导；T-B5 迁移闸决策悬置（r6 遗留） |
| 11 | 文件夹分析（v2.23 真·懒展开树：cChildren=1 → FolderChildrenAsync 4 线程子树定大小） | 文件夹分析 Tab | 可用 | ⚠️ **无进度无取消**（G-9）；切页丢载荷留"加载中"占位（自愈） |
| 12 | 右键"移入隔离区…"（PlanBuilder+GuardRails 全链） | 文件夹分析 Tab | 成熟 | ✅ NotifyTaskStarted 锁矩阵 |
| 13 | 操作历史（JSONL+签名、中文类型/风险标注、虚拟列表） | 操作历史 Tab | 可用 | ⚠️ 无排序/无过滤/无右键；**清空后状态列谎报"可还原"**（G-3） |
| 14 | 批量撤销（worker 线程、签名校验、预检分类提示） | 历史页「撤销选中」 | 成熟 | ⚠️ 撤销后无"打开还原位置"（G-4） |
| 15 | 清空隔离区（逐盘删除、逐盘判定成败、`[已释放]` 批量标注、用量归零） | 历史页按钮 + 命令面板 | 可用 | ⚠️ 用量归零✅、历史 note✅、磁盘 pane✅；**状态列未改**、**清空动作自身无审计行**（G-3/G-2） |
| 16 | 反选 InvertChecked | 右键/Ctrl+I/命令面板 | 成熟 | ✅（v2.22 新增，map 语义与全选一致） |

### 1.2 横切能力

| # | 功能 | 入口 | 成熟度 | 状态同步完整性 |
|---|---|---|---|---|
| 17 | GuardRails（规范化/TOCTOU/云占位/排除清单/双上下文） | 内部 | 成熟 | ✅ |
| 18 | PlanBuilder + planHash 整单中止 | 内部 | 成熟 | ✅ |
| 19 | 预览执行 dry-run | 右键/命令面板 | 成熟 | ✅ |
| 20 | 列表校验（右键手动 + WM_ACTIVATE 归来 + 空闲修剪 + 缓存持久化修剪） | 右键 | 成熟 | ✅（"扫描结果重新验证"**已存在**，见 §9 被推翻假设 #2） |
| 21 | 命令面板 Ctrl+K（**19 条**：9 操作 + 6 切页 + 4 筛选） | **Ctrl+K（已死，G-1）**/无按钮入口 | 可用 | ⚠️ 唯一入口断线 |
| 22 | 快捷键 F5（扫描/取消）、Ctrl+A（全/无切换）、Ctrl+I（反选）、Ctrl+K（**断**） | 全局 | 可用 | ⚠️ |
| 23 | 设置持久化（11 字段）+ 会话恢复（六 Tab 结果/查询/勾选/页面） | 自动 | 成熟 | ✅ |
| 24 | 空闲维护（5min 空闲→10s 倒计时重扫；10-30min→USN 增量+幽灵修剪；可抢占可取消） | 自动 | 成熟 | ✅ |
| 25 | 单实例 + `-takeover` 轮换、`-tour`、`-probe` | 命令行 | 成熟 | ✅ |
| 26 | 虚拟列表 + 自绘复选框 + 斑马纹 + 风险染色 | 全列表 | 成熟 | ✅ |
| 27 | v2.21-2.23 视觉系（主按钮强调色/信息卡/浅色 chrome/胶囊 Tab） | — | 成熟 | ✅（07 范畴） |
| 28 | 测试覆盖 99 例（core 侧 14 文件；**UI 侧 0**） | MiniSysTests | 骨架 | charter"含 UI 的 100% 覆盖"现状：UI 无任何自动化用例，-tour/-probe 是评审工具非回归测试 |

### 1.3 孤儿清单（写了没接线；逐条 grep 核实）

| 项 | 现状 | 证据 |
|---|---|---|
| `DeleteOp` | 仍无构造点（仅测试引用） | grep `DeleteOp(` 仅 DeleteOp.cpp:15 自身 |
| `OpType::MoveFilePath` | 仅序列化往返，无实现 | OperationLog.cpp:381/392 |
| `IDC_BTN_REFRESH/DEDUP/SORT_SIZE/SORT_TIME` | resource.h:16/20/30-31 定义，零引用 | grep 零命中 |
| **`TabPresenter::SortBySize/SortByTime`** | **v2.21 删除排序按钮后成为新孤儿**（声明+实现仍在，无调用者） | Presenters.h:43-44、Presenters.cpp:453-465，grep 零调用 |
| `ScanItem.dangerous` | 写入+持久化，UI 零读取（"额外危险确认"承诺未兑现，确认弹窗实际用 riskLevel+Advanced 判定） | ui/ 下 grep `dangerous` 0 命中 |
| `AppInfo.isUWP` | 已赋值（AppScanner.cpp:94）仅作排除标记，零读取 | grep 2 命中均为赋值侧 |
| Ctrl+K 入口 | 严格说是"接线断了"而非孤儿（见 G-1） | §2 |

---

## 2. 上轮（r6-08）提议落地复核

git diff `2a9d653..HEAD`：core 侧仅 OperationLog/SessionService/VolumeIndex/SecretStore/Logger 有改动（修复+v2.23 FolderChildrenAsync），**扫描器/操作/GuardRails/rules.json 零改动**。

| r6 提议 | 状态 | 证据 |
|---|---|---|
| N1 开发/游戏缓存规则包 | **未落地** | rules.json 仍 27 条，无 vcpkg/conda/pnpm/uv/Go/Docker/Unity/UE/Steam |
| N2 释放预测三层口径 | **未落地** | grep `SummarizeRelease` 0；确认弹窗仍是分组后果文案（无立即释放/隔离/积压三层字节） |
| N3 CSV/剪贴板导出 | **未落地** | grep Clipboard/CSV/导出 = 0（全仓仍无任何导出通道） |
| N4 去重硬链接感知 | **未落地** | grep nNumberOfLinks/BY_HANDLE_FILE_INFORMATION = 0（仅 docs 命中） |
| N5 批量选择扩展 | **落地 1/4**：反选 ✅（v2.22，Ctrl+I+右键+面板）；"只勾安全级/只勾>N MB/按分类" 未做 | MainWindow.cpp:785-798、Presenters.cpp:254 |
| N6 隔离区浏览器 | **未落地** | History 无右键（MainWindow.cpp:2432 早退）、无 PurgeQuarantined |
| N7 自定义筛选预设 | **未落地** | Settings 无 searchViews 字段；kPresets 静态 |
| N8 扩展名空间报表 | **未落地** | 无 ExtStat |
| N9 磁盘空间趋势 | **未落地** | grep space-trend = 0 |
| N10 排除清单管理 UI | **未落地** | 仍只有右键添加；移除=手改 settings.json |
| N11-N17（P2/Backlog） | 全部未落地 | — |
| r6 综合报告 P1"08-N5+N3" | 半落（N5 一件） | — |

**结论**：功能管线连续第二轮近乎静止，但本轮不同——v2.22/2.23 两件用户点单（反选、文件夹树）+ 三件视觉迭代说明节奏在"按用户反馈打磨"，与本轮 charter 方向一致。r6 P0 提议继续有效，优先级继承，下文不重复设计、只引用。

---

## 3. 交互完备性缺口表（本轮重点）

走查方法：对每个"完成动作"问三问——用户下一个自然动作是什么？应用是否引导？各处状态是否同步？

| # | 场景（动作完成时点） | 用户下一个动作 | 应用现状 | 完备性 |
|---|---|---|---|---|
| G-1 | 按 Ctrl+K 想开命令面板 | 打开面板 | **无反应**。`ACCEL acc[]` 4 项但 `CreateAcceleratorTableW(acc, 3)`——最后一项 Ctrl+K 被截掉；About 文案、搜索空态 `（Ctrl+K 命令面板）`、面板标题仍宣传该键 | **断线（回归）** |
| G-2 | 清空隔离区完成 | 确认释放了多少/去历史核对 | 用量归零✅、note `[已释放]`✅、磁盘 pane✅；但**清空动作本身不产生任何历史行**（对比：清空回收站有独立 OpRecord）——何时清的、释放了多少，历史页无顶层事件可查 | 部分缺 |
| G-3 | 清空后看历史行 | （了解该项已永久删除） | 详情列有 `[已释放]` note，但**状态列仍"已隔离 · 成功"、风险列仍"可还原"**（HistoryRiskLabel 不识别 note 前缀，Presenters.cpp:1010）；UndoSelected 预检同样把它计入 eligible（Presenters.cpp:1041），点撤销才在运行期失败"隔离区中找不到该项目" | **状态谎言** |
| G-4 | 撤销完成 | 确认文件回到了原位（打开看看） | 行状态"已还原"+note"已还原到 X"✅；但 History **无右键、无双击、无"打开还原位置"**；`.restored` 冲突只藏在 note 文本里 | 部分缺 |
| G-5 | 扫描被取消 | 知道结果不完整，决定重扫或凑合用 | 进度文案仍"扫描完成: N 项"（RunScan 不区分 cancelled，SessionService.cpp:736）；**部分结果无任何"不完整"标记**；对比 BuildIndexAsync 有专门"已取消"文案 | **不诚实** |
| G-6 | 扫描异常中断 | 重试 | pane2 一行"⚠ 任务异常中断（详见日志）"；若 buffer 为空，info 首行反而是"**✓ 未发现可处理项，系统状况良好**"（MainWindow.cpp:1827-1829 只判 empty） | **误导** |
| G-7 | 执行失败（文件占用） | 关掉占用进程，重试失败项 | 错误文案点名进程并给建议（QuarantineOp DiagnoseLock，实测 6 条 node/npm 占用记录文案在案）✅；失败项保留在列表且**保持勾选**（succeeded 才被移除）✅——但报告里**没有一句话告诉用户"失败项已保留勾选，关闭程序后直接再点执行即可"** | 差一句引导 |
| G-8 | 执行成功（隔离） | 去历史页看看/还原/继续下一批 | ShowHint"✓ 完成…已隔离 X（可还原）"+确认弹窗里有"可在操作历史还原"字样 ✅；无直达跳转（信息卡不可点击）——可接受，但可在 hint 里加"→ 切到操作历史" | 基本支持 |
| G-9 | 展开文件夹树的一个大目录 | 等待/取消 | v2.23 懒展开：子目录逐个 `DirectorySizeParallel(kid,4)` 串行定大小，进度永远停在"展开文件夹…"；**无取消**（FolderExpand 不是 Scanning，扫描按钮走"已有任务在进行中"分支把用户挡死）；busy 时再点其它展开只得到 hint"后台忙，稍后再次展开" | **新功能裸奔** |
| G-10 | 文件夹树右键一个文件夹 | 移入隔离区（有）；在资源管理器里看看（**无**） | 右键菜单仅 1 项（Presenters.cpp:900）；`itemPaths_`/explorer /select 零件现成 | 缺 |
| G-11 | 历史页任意行 | 还原（✅ 选中+按钮）；打开位置/看详情/排序/过滤（**全无**） | History 无 OnColumnClick、无右键、双击走 TabPresenter 默认 no-op | 缺 |
| G-12 | 搜索结果里隔离了 30 项后直接关应用 | （下次打开不看到幽灵） | RunPlan 只改内存 results_，**不回写 results-1.json**；重启后幽灵行复活（fromCache=true），直到 WM_ACTIVATE 校验/空闲维护/下一次搜索修剪。其它 Tab 同理但自愈路径相同 | 部分缺（自愈） |
| G-13 | F5 在历史页 | （预期：刷新或提示） | 静默 no-op（BuildScanner 返回 nullptr 且非 Search 直接 return） | 微缺 |
| G-14 | 加错一条排除项 | 移除它 | **只能手改 settings.json**（无管理 UI，N10 继承） | 缺 |
| G-15 | 索引构建中想搜索 | 等待并自动应用 | hint"索引构建中，完成后将自动应用该筛选"+OnScanDone 自动 RunSearch ✅ | **完备（范例）** |
| G-16 | 搜索命中 1000 上限 | 看更多/收窄 | "最多显示前 1,000"提示有；无继续加载（N11 继承） | 部分缺 |
| G-17 | 勾了 InfoOnly 项点执行 | （理解为何没执行） | "所勾选项目均为仅提示项，不可执行" + 弹窗自动排除行 ✅ | **完备（范例）** |
| G-18 | 迁移失败回滚（Interrupted） | 按指引人工恢复 | note 文本指引；无恢复向导（R-005，v1 遗留至今） | 长期缺 |
| G-19 | 测试套件跑完 | （用户数据不被污染） | **污染**：`TrustChain.AppendSignsRecordsAndTamperBreaksSignature` 未调 `SetHistoryPathForTesting`，把 `C:\some\file.txt` 写进真实 history.jsonl（2026-10-29 02:11:06 在案，GuardRailsAdversarialTests.cpp:263-286）；Logger 无重定向机制，真实 minisys.log 混入历史测试期 "Scanner threw: boom" ERROR 12 条 | **卫生缺口** |

**入口 × 状态同步抽查（任务点名场景）**：命令面板执行"清空隔离区"（任意页）→ 确认弹窗（含当前占用）→ worker 逐盘删 → OP_DONE → 状态栏用量即时归零（generation 缓存失效）✅、磁盘 pane 刷新 ✅、当前页 info 显示报告 ✅、History 行延迟到切页刷新（切页必 Refresh，可接受）；唯 G-2/G-3 两条没闭环。

---

## 4. Top 10 交互完备性修复（优先于一切新功能；全部 S 级）

| # | 修复 | 位置/方案 | 验收 |
|---|---|---|---|
| **T1** | **恢复 Ctrl+K**：`CreateAcceleratorTableW(acc, 3)` → `_countof(acc)`（或 `std::size(acc)`） | MainWindow.cpp:376，一行 | 按 Ctrl+K 弹面板；新增静态断言防再犯 |
| **T2** | **清空隔离区状态链收口**：① HistoryRiskLabel 识别 note `[已释放]` → 显示"已释放"；② UndoSelected 预检跳过已释放记录并提示"该项已永久删除"；③ RunEmptyQuarantine 追加一条自身审计 OpRecord（如 type=EmptyRecycleBin 复用或新枚举，note 记"释放 X GB/N 项"） | Presenters.cpp:1010/1037、SessionService.cpp:919 | 清空后历史可见事件行、旧记录状态列"已释放"、撤销预检拒绝且提示清晰；QuarantineUsageText 不受新行影响 |
| **T3** | **取消/异常的诚实收场**：RunScan cancelled → 进度与 info 标"已取消（结果不完整，N 项）"，结果列表加"不完整"标记行；异常路径（StartTask threw）→ OnScanDone 不再拼"系统状况良好"，改"扫描中断，点「扫描」重试" | SessionService.cpp:700-743、MainWindow.cpp:1827 | 取消后 UI 三处（pane2/info/缓存标记）口径一致；异常空结果不再出现"状况良好" |
| **T4** | **测试污染收口**：该测试补 `OperationLog::SetHistoryPathForTesting`（与同文件 SignUnsignedLegacyQuarantineBoundary 一致）；Logger 增加 `SetPathForTesting` 并在 tests/main.cpp 统一重定向；顺带清理用户 history 中的 `C:\some\file.txt` 行（需用户确认或由"信任签名"旁的清理入口处理） | GuardRailsAdversarialTests.cpp:263、Logger.cpp、tests/main.cpp | 再跑全套测试，用户 history.jsonl 与 minisys.log 零新增外来行 |
| **T5** | **懒展开的进度与取消**：FolderChildrenAsync 每个子目录 SetProgress("展开 %d/%d: %s")；FolderExpand 期间扫描按钮兼作"取消"（CancelScan_ 在 FindNext 循环已检查）；取消后占位改"已取消展开" | SessionService.cpp:531-582、MainWindow.cpp OnScan/SetTaskBusy | 展开 C:\Users 级目录时进度实时、可取消、取消后树不自锁 |
| **T6** | **History 右键 + 双击**：右键 = 打开来源位置 / 打开隔离区位置 / 还原此行 /（T2 后）详情；双击 = 项目说明复用 ShowItemInfo 风格展示 OpRecord 全字段 | 新 OnHistoryContextMenu（零件：OnOpenLocation 的 /select、UndoSelected、rows_） | 历史页行级动作零按钮点击可达 |
| **T7** | **FolderTree 右键补"在资源管理器中打开"** | Presenters.cpp:897 菜单加一项 + explorer /select（零件现成） | 右键两項：打开 / 移入隔离区 |
| **T8** | **执行报告的"下一步"引导**：OnPlanDone 的 ShowHint 按结果分支追加：有失败 → "失败 N 项已保留勾选，关闭占用程序后可再次执行"；有隔离 → "→ 操作历史页可还原；清空隔离区后才真正释放"；有还原 → "已还原到原位置，可在列表查看" | MainWindow.cpp:2204-2245 | 每种终态都有一句指向下一个动作 |
| **T9** | **失败项聚焦**：执行含失败时，报告弹窗后把列表滚动到首个失败项并选中（零件：viewFocusPath_/RestoreViewState 机制） | OnPlanDone + presenter | 用户视线直达需要善后的行 |
| **T10** | **历史列排序**（X-35 继承）：History 接入 OnColumnClick（时间/大小/状态/类型可排；虚拟列表管线现成，只差 rows_ 排序+箭头） | Presenters.cpp HistoryPresenter | 点任意列头排序，箭头与主列表一致 |

T1-T5 建议随下一个 build 一次交付（合计 ≈1 天）；T6-T10 随后。

---

## 5. 候选功能卡片总表

价值 1-5；成本 S≈1-2 天 / M≈3-5 天 / L>1 周。"继承"= r6 已设计，本轮复核仍成立不重复展开。

| ID | 候选 | 价值 | 频次 | 成本 | 风险 | 依赖 | 优先级 |
|---|---|---|---|---|---|---|---|
| R1 | 清空隔离区审计行+状态列修复（=T2） | 5 | 每次清空 | S | 低（纯标注层） | — | **P0** |
| R2 | 隔离区管理页（列表/按龄提示/单条永久删除/按盘分组）（=r6 N6） | 4 | 清空决策前 | M | 低（走既有确认链；单条 Purge 后记录重签需核对） | T2 | **P1** |
| R3 | 历史页过滤（只看：可还原/已释放/失败需关注/今日）+排序（=T10） | 4 | 每次善后 | S | 低 | T2 的"已释放"识别 | **P0**（排序）/P1（过滤） |
| R4 | 列表/报告导出（TSV 剪贴板 + CSV 文件）（=r6 N3） | 4 | 每周复盘 | S | 低 | — | **P0** |
| R5 | 批量选择谓词补全：只勾"安全"级 / 只勾>N MB / 按分类（=r6 N5 余量） | 4 | 每次大列表 | S | 低 | — | **P0** |
| R6 | 去重硬链接感知（=r6 N4，正确性：[DELETE] 虚报可释放字节） | 4 | 每次去重 | S | 低-中 | — | **P0**（继承） |
| R7 | 开发/游戏缓存规则包（vcpkg/conda/pnpm/uv/Go/Docker vhdx/Unity/UE/Steam/着色器）（=r6 N1） | 5 | 每次清理 | S（数据为主） | Docker vhdx 需 InfoOnly 化 | rules.json 管线现成 | **P0**（继承） |
| R8 | 空间释放预测三层口径 + 执行对照（=r6 N2；含"当前隔离区已积压 Z GB"一句） | 4 | 每次执行 | S | 低 | R1 | **P1**（继承） |
| R9 | 扫描取消/异常的"重试"入口与不完整标记（=T3） | 4 | 偶发 | S | 低 | — | **P0** |
| R10 | 磁盘空间概览仪表（全盘卡片：各盘可用/占比/较上次变化） | 3 | 每次启动 | S-M | 低 | r6 N9 趋势采样 | P1 |
| R11 | 自定义快速筛选预设（=r6 N7） | 3 | 高频 | S-M | 低 | Settings 扩展 | P1（继承） |
| R12 | 扩展名空间报表（=r6 N8 两档） | 3 | 瘦身决策 | S | 低 | v2.19 大小池 | P2（继承） |
| R13 | 搜索"继续加载"突破 1000（=r6 N11） | 3 | 命中>1000 时 | S-M | 中（全量计数需可取消） | — | P2 |
| R14 | 空间趋势 jsonl + "较上次 +X GB"（=r6 N9） | 3 | 每次启动 | S | 低 | — | P2（继承） |
| R15 | 排除清单管理 UI（=r6 N10；列表+移除+测试排除） | 3 | 一次配置 | S | 低 | RootsDlg 模板 | P1（继承） |
| R16 | 清理计划导出/回放（=r6 N12，红线：回放=勾选记忆） | 2 | 例行人群 | M | 中 | — | P2 |
| R17 | 空闲预备计划（=r6 N13） | 2 | 每日 | M | 中 | — | P2 |
| R18 | CLI 伴侣（-scan/-report/--dump-report）（=r6 N15） | 2 | 脚本化 | M/L | 低 | 公共代码抽取 | Backlog |
| R19 | 迁移后深度校验（=r6 N14） | 2 | 罕见 | S | 低 | — | P2 |
| R20 | 云占用只读报告（=r6 N16） | 2 | OneDrive 用户 | S-M | 低 | — | Backlog |
| R21 | 规则导入/导出（=r6 N17） | 2 | 低频 | M | 低 | R7 后价值上升 | Backlog |
| R22 | UI 自动化回归测试（charter"含 UI 的覆盖"）：把 -tour 截图 + 断言（复选框像素计数、列头箭头、按钮标签）纳入可重复脚本 | 3 | 每次构建 | M | 低（screenshot.ps1 已有，需接 CI/rotate） | 构建轮换 | P1（工程） |
| R23 | 定时自动清理（自动执行版） | — | — | — | 违反需求 #3 确认制 | — | **不做** |
| R24 | 托盘常驻/开机自启 | — | — | — | 连续四轮否决；告警由应用内形态承接 | — | **不做** |
| R25 | 硬链接"合并释放" | — | — | — | 与安全可逆承诺冲突 | — | **不做** |
| R26 | 多语言 / AI 风险解释 / 启动项·DLL 扫描 / 回收站逐项清理 / pHash / $MFT / SMART | — | — | — | 维持 r6 §6 全部理由 | — | **不做** |
| R27 | "一键加速/注册表清理"类系统优化 | — | — | — | 定位漂移至系统优化器域，与"文件操作先经安全闸"的产品边界冲突 | — | **不做**（新增） |

---

## 6. Top 5 深入

### #1 R1 清空隔离区状态链收口（P0，S）
- **接口**：① `HistoryPresenter::HistoryRiskLabel` 增加 `if (r.note.rfind(L"[已释放]",0)==0) return L"已释放";`（在签名校验之后、Failed 判定之前）；② `UndoSelected` 预检加同一识别 → 归入 notReversible，提示"该项已随清空隔离区永久删除"；③ `RunEmptyQuarantine` 成功盘>0 时 `OperationLog::Instance().Append` 一条审计记录（复用 `OpType::EmptyRecycleBin` 语义不妥——建议新增 `OpType::PurgeQuarantine`，isReversible=false，note="释放 X / N 项"；HistoryTypeLabel 加中文标签"清空隔离区"）。
- **数据流**：现状 `UpdateStatusBulk(releasedIds, Success, L"[已释放]…")` 已把信息写进 note——三处消费方（UsageText/HistoryRiskLabel/UndoSelected）只有第一处读了它。补齐另两处即闭环；审计行走既有 Append+签名链。
- **UI 入口**：无新入口；历史页自然出现"清空隔离区 · 成功"事件行。
- **测试策略**：OperationLogTests 增：Append 审计行→签名有效→UsageText 不计数；HistoryRiskLabel 单测（note 前缀×状态矩阵）；手工：清空→状态列/撤销预检/用量三处一致。
- **护栏**：标注层零 I/O。

### #2 R3 历史页完备化（P0 排序 + P1 过滤，S）
- **接口**：`HistoryPresenter` 实现 `OnColumnClick(col)`：对 rows_ 稳定排序（类型/时间字符串/大小数值/详情/状态序），复用 `UpdateHeaderSortArrows`；过滤 = presenter 内一个 enum 掩码 + `Refresh()` 时套用，入口放在历史页 info 行下方的组合下拉（或右键"只看…"子菜单——零新控件）。
- **数据流**：rows_ 每次从 `OperationLog::Instance().LoadAll()` 全量拉取（142 条，微秒级），过滤纯内存。
- **UI 入口**：列头（排序）；右键子菜单：只看 可还原 / 已释放 / 需关注 / 今日。
- **测试策略**：排序比较器单测（时间字符串 lexicographic==chrono 已由 yyyy-MM-dd 保证）；过滤谓词单测；手工核对"已还原/未签名"分组计数。
- **与 R2 的关系**：过滤是管理页的 80% 价值、20% 成本；先做过滤，R2 缓行。

### #3 R4 导出（r6 N3 原样继承，P0，S）
设计不变（snapshot_/cells_ → TSV 剪贴板 / GetSaveFileNameW CSV，原子写）；新增落点建议：右键菜单"复制为 TSV / 导出 CSV…"两項 + 历史页右键（T6）同步获得。1000 行内存遍历 <50ms 预算。**全仓至今零导出通道，用户是开发者，这是真实空白。**

### #4 R5 批量选择谓词补全（r6 N5 余量，P0，S）
- **接口**：`ListTabPresenter` 增 `SetCheckedBy(predicate)`（map 批量写+单次重绘，复用 SetAllChecked 模式）；右键"选择"子菜单：只勾"安全"级 / 只勾 >100 MB / 勾选当前分类 / 反选（已有）。
- **测试策略**：谓词纯函数单测 + 1000 行 <50ms 断言。
- **理由**：反选落地后这是同一零件的第三、四次复用，边际成本最低。

### #5 T5+T3 懒展开与扫描的"可控性"（P0，S）
- **接口**：FolderChildrenAsync 循环体每子目录 `SetProgress(FormatW(L"展开 %zu/%zu: %s", …)) + Post(WM_APP_SCAN_PROGRESS)`；MainWindow OnScan 分支扩展：`svc.IsBusy() && taskKind==FolderExpand` 时按钮=取消（CancelScan_）。
- **数据流**：进度经既有 progressMu_/PROGRESS 消息，无新链路；取消在 FindNext 循环检查点生效（已存在），补占位项善后。
- **UI 入口**：无新控件——扫描按钮语义复用（扫描中=取消 的既有先例）。
- **测试策略**：FolderChildrenAsync 单测（目录树夹具+取消标志置位→items 截断且消息发出）；手工：展开用户目录观察进度刷新与取消。
- **附带**：G-7 的"取消后占位残留"一并清理（ApplyChildren 对已删父节点 drop 已处理；补取消路径删除占位）。

---

## 7. 分阶段路线图

| 阶段 | 内容 | 主题 | 量级 |
|---|---|---|---|
| **Now（本周）** | T1 Ctrl+K；T2 清空状态链+审计行；T3 取消/异常诚实化；T4 测试污染；T5 懒展开进度+取消；T8 报告下一步引导 | "状态永远一致，入口永远在线" | S×6 ≈2 天 |
| **Next（1-2 周）** | T6 历史右键/双击；T10 历史排序；R3 历史过滤；R4 导出；R5 批量谓词；R6 硬链接感知；T7 树右键打开；T9 失败聚焦；R22 UI 回归脚本雏形 | "每个终态都有下一步，每个列表都能带走" | S×7+M×1 |
| **Later（1 个月）** | R7 缓存规则包；R8 释放预测；R2 隔离区管理页；R10/R14 空间仪表+趋势；R11 自定义预设；R15 排除管理 UI；B-4 冷启动惰性化（03 继承） | "黑箱开窗，路径记忆" | M×3+S×4 |
| **Backlog** | R12 扩展名报表；R13 搜索分页；R16/R17 计划导出/预备；R18 CLI；R19 迁移深校验；R20 云占用；R21 规则导入导出；T-B5 迁移闸产品决策 | 触发条件驱动 | — |

依赖：R2/R3 依赖 R1 的"已释放"语义；R8 依赖 R1 的积压口径；R11 依赖 Settings 扩展；R22 依赖构建轮换流程允许挂钩。

---

## 8. "不该做"清单及理由

继承 r6 全部 8 条（定时自动清理 / 托盘常驻 / 硬链接合并 / 回收站逐项 / 多语言 / AI 解释 / 启动项·DLL 扫描 / pHash·$MFT·EV·TRIM·SMART），本轮逐条复核无新证据推翻，另新增：

1. **"一键加速/注册表清理"（R27）**——产品边界（需求.txt + About 文案：本地文件工具、先经安全闸、默认可逆）之外；注册表操作没有"隔离区"等价物，不可逆承诺无法成立。
2. **执行阶段全局可取消（全部操作）**——r6 综合报告 P1 提过"Executing 可取消"；本轮细化立场：**委派类（DISM 分钟级）应可取消，文件移动链（同卷 rename 毫秒级）不值得引入中断态复杂度**。做窄不做宽。
3. **历史记录编辑/删除**——历史是签名防篡改的信任锚（T-A1），给用户编辑入口等于自毁签名链语义；只允许"信任并签名旧记录"这一单向加固。

---

## 9. 被推翻的假设 / 已核实存在

| # | 假设（任务/上轮/直觉） | 复核结果 | 证据 |
|---|---|---|---|
| 1 | "用户今日清空了隔离区"（任务描述） | **被推翻**：history.jsonl 142 条中 `[已释放]` note 计 0 条；73 条 QUARANTINE SUCCESS 全部仍在计数（状态栏此刻仍显示"隔离区: 73 项"）。今日要么未执行、要么执行失败（逐盘失败会弹窗但记录不标）——日志无对应行，无法区分 | history.jsonl 逐条统计 + minisys.log 全文检索 |
| 2 | "扫描结果'重新验证'按钮"是待建功能（任务候选方向） | **已存在**：右键"刷新列表（移除已手动删除的项）"（IDM_LIST_VERIFY）+ WM_ACTIVATE 自动 + 空闲维护修剪，命令面板也有同名项。缺的只是可见性，不是功能 | MainWindow.cpp:801/2451、SessionService.cpp:1085 |
| 3 | "Ctrl+K 自 v2.10 起一直可用" | **被打破**：v2.22（d574af3）插入第 4 项后 count 仍为 3，Ctrl+K 自 build 104 起断线；v2.20（3c5a8f4 时点）时仍是 3 项=正确 | git show d574af3/3c5a8f4 对照 MainWindow.cpp |
| 4 | r6-08 称"复选框从不渲染"（X-41） | **已修复**（v2.20 NM_CUSTOMDRAW 自绘 DFC_BUTTON，代码在位 MainWindow.cpp:572-590） | 读码 |
| 5 | "日志/历史中的怪记录可能是用户操作" | **部分推翻**：`C:\some\file.txt → C:\MiniSys.Quarantine\x`（02:11:06）与测试夹具逐字一致，是 v2.20 新测试写入；9 条 FAILED 中 2 条 'MoveFileEx failed (Win32 5)' 是旧版文案残留（现行代码输出"无法移动（Win32 %lu）"） | GuardRailsAdversarialTests.cpp:272、QuarantineOp.cpp:37 |
| 6 | "SortBySize/SortByTime 有调用者" | **新孤儿**：v2.21 删按钮后零调用 | grep |
| 7 | r6 D1"asInvoker" | 仍成立：app.manifest `requestedExecutionLevel level="asInvoker"`、PerMonitorV2 | 读 manifest |
| 8 | "隔离完成→历史页查看"动线断裂 | **基本成立而非断裂**：确认弹窗与 ShowHint 均有指引文字，缺的只是可点击跳转与 G-7 那句"失败项已保留" | 读码 |

---

## 10. 性能护栏（新功能硬约束）

继承 r6 §7 全部 7 条（零扫描期 I/O / 按需小规模句柄 / 导出即内存遍历 / 趋势限启动一次 / 批量 UI 写走 map / 新持久化原子幂等 / worker 纪律与 taskGen_），本轮新增：

1. **FolderExpand 护栏**：进度每子目录一次 Post（禁每文件）；DirectorySizeParallel 维持 4 线程上限；必须可取消（CancelScan_ 检查点已存在）；建议对子目录数 >200 的展开在 info 提示"目录较大，耗时较长"。
2. **测试卫生护栏**：一切触碰 OperationLog/Logger/results 缓存的新测试必须先调 Set*PathForTesting；tests/main.cpp 增加启动时断言（历史文件 mtime 在套件前后不变）作为防回归闸。
3. **UI 回归脚本（R22）护栏**：截图断言只读（PW_RENDERFULLCONTENT 被动截取），不注入输入；纳入 rotate 流程时不得阻塞构建失败路径。
4. **加速键表护栏**：`CreateAcceleratorTableW(acc, std::size(acc))` + `static_assert`——同类"数组-计数"漂移（v2.22 教训）一次杜绝。

---

## 11. 特别加分：现有零件可直接拼装清单

| 已有零件 | 只差什么 | 拼出 |
|---|---|---|
| `note` 前缀 `[已释放]`（已在盘上）+ HistoryRiskLabel/UndoSelected | 两个 if | R1 状态链收口 |
| `OpRecord` Append+签名链 + RunEmptyQuarantine 汇总 | 一行 Append | 清空审计事件行 |
| `OnOpenLocation` 的 explorer /select + `itemPaths_` | 菜单项两处 | T6 历史打开位置、T7 树右键打开 |
| `checkStateByPath_` map + `SetAllChecked` 批量模式 + `InvertChecked` 先例 | 谓词函数 3 个 | R5 批量选择补全 |
| `rows_`/`cells_` + `UpdateHeaderSortArrows` + 列点击管线 | History 的 OnColumnClick + 比较器 | T10 历史排序（过滤加一个掩码） |
| `snapshot_`/`cells_` + Settings 原子写 | 转义+剪贴板 | R4 导出 |
| 去重三阶段管线（groupKey/DUP-） | 簇内 file index 比对 | R6 硬链接感知（r6 设计原样） |
| `ExecuteReport.quarantinedBytes/freedBytes` + `QuarantineUsageText` | 执行前聚合一次 | R8 释放预测（含积压 Z） |
| rules.json 管线（校验/降级/热加载） | 纯数据条目 | R7 缓存规则包 |
| v2.19 并行 GetFileAttributesExW 大小池 | 复用到隔离区条目 | R2 管理页的大小列 |
| `OpRecord.timestamp` | 一个阈值比较 | R2 按龄提示（>14 天"建议确认"） |
| `QueryDiskSpace` + info 卡 + jsonl 追加 | 一次采样+一行渲染 | R10/R14 空间仪表与趋势 |
| `-tour` + `tools/screenshot.ps1` + 像素断言先例（v2.20 复选框 395 暗像素法） | 脚本化+断言库 | R22 UI 回归测试 |

结论：Top 10 与 Top 5 中**除 T5 外全部是接线级**；本轮与此前各轮同构——零件库一直在变大，欠的是最后一段焊锡。

---

## 12. 未核实 / 局限

1. **未活体运行应用**（只读约束）：所有 UI 行为结论来自读码 + 用户日志/历史数据交叉；G-5（取消文案）、G-9（展开无进度）等未实机复现，但代码路径唯一、无歧义。
2. **G-12 幽灵行复活的实际观感**（重启后到 WM_ACTIVATE 校验之间的窗口时长）未实测；推断 <1s（ShowWindow 即触发 WM_ACTIVATE→400ms 定时器）。
3. **v2.23 懒展开在大目录上的实际耗时**未实测；16.7s/7 项的 FolderTree 扫描（今日日志）只证明全盘遍历量级，不等于展开耗时。
4. **今日"清空隔离区"是否被尝试过**无法从现有数据区分（§9-1）；若曾尝试且失败，逐盘失败详情只在当时的弹窗里，不落日志——建议 RunEmptyQuarantine 失败盘写 MS_LOG_WARN（已在 T2 顺带范围）。
5. rules.json 27 条的 minAgeDays/detailHint 具体值未逐条读取（与 r6 相同，仅核对 id/结构/数量）。
6. `DirectorySizeParallel` 的取消语义未逐行核对（r7-03 范畴）；T5 设计按"FindNext 循环检查点"陈述，若该函数内部不支持取消，需补线程参数传递。
7. 测试侧：99 例的逐条断言质量未审（本轮只统计分布与污染路径）；"100% 覆盖含 UI"的可达形态以 R22 给出工程方案，未评估googletest 外的 UI 框架选型。
8. 用户 settings.json 显示 `useSymlink: true`（高级模式常开）——迁移语义影响未展开（05/06 范畴），仅记录为事实。
