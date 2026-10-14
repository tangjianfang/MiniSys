# 04 · UI 稳定性（崩溃风险）审查 — r6 / 2026-10-14

> 对象：HEAD `2a9d653`（v2.19a，build 95）。范围：MainWindow.cpp / ui/*（Controls/Layout/Presenters/CommandPalette/Icons/Dialogs）+ SessionService 异步边界 + main.cpp 关闭/接管路径 + 近 5 提交逐条。
> 方法：全部结论亲自读码（文件:行号），未运行应用、未修改仓库。日志副本 `.tmp-review/r6/minisys-log-copy.log`（949 行）与 dumps 目录只读核对。

---

## 0. 结论：假设证伪表

| # | 待验证假设 | 结论 | 证据 |
|---|---|---|---|
| H1 | v2.16d SupplyDispInfo 越界守卫至今有效 | **成立（Confirmed）** | `Presenters.cpp:190-214`（ListTabPresenter：row/col/idx 三重检查）与 `:900-915`（HistoryPresenter 同款）均在位；LVIF_STATE 分支只读 `snapshot_[row]`（row 已查边界） |
| H2 | v2.19 SearchAsync worker 内并行线程写 items[i] 有数据竞争 | **不成立（证伪）** | `SessionService.cpp:540-568`：`items` 为 worker 栈上私有向量；并行阶段只做 `cursor.fetch_add` 分区写 `items[i].sizeBytes`（不同下标、无重入 push），join 全部线程后才 `StoreResults`（:571，锁内换入）；UI 只在 WM_APP_SEARCH_DONE 后经 `Results()` 锁拷贝读取 |
| H3 | Layout(OnSize) ↔ ShowHint 可成重入环 | **不成立（证伪）** | `Layout.cpp:35-156`：LayoutWindow 只 SetWindowPos/发 WM_SIZE 给状态栏，不写 info 文本、不调 ShowHint；`ShowHint`（MainWindow.cpp:1757-1765）= SetWindowTextW(info)+OnSize，环不闭合 |
| H4 | Tab NM_CUSTOMDRAW 换粗体引发重排风暴 | **不成立（证伪）** | `MainWindow.cpp:332-348` 仅 CDDS_ITEMPREPAINT+CDIS_SELECTED 时 SelectObject+CDRF_NEWFONT；tab 项宽变化不向父窗发布局消息；TCM_SETPADDING 仅 CreateControls 设一次（Controls.cpp:20） |
| H5 | 模态泵期间 WM_APP_* 重入改 snapshot_（R-4 gen 门槛） | **修复存活；残余点已评估为安全** | 六个 WM_APP_*_DONE/TASK_STARTED 处理器全部 `wp != CurrentTaskGen() → return`（MainWindow.cpp:612-698）；`SessionService::Post` 对 wp==0 的 DONE 类消息盖章当前 gen（SessionService.cpp:70-84）。OnExecute 确认框期间 snapshot_ 确可被替换（TIMER_SEARCH_DEBOUNCE/VERIFY_DONE 在 taskMode_==None 时仍会跑），但事后对 `items` 的唯一使用是 PlanBuilder::Build（内部逐 idx 查界，PlanBuilder.cpp:47-48）+ worker 侧 PlanMatches 失配兜底（SessionService.cpp:674）→ 不会越界，只会正确报"计划已过期" |
| H6 | 递归 WalkSubtree 深链目录栈溢出 | **生产不可达（证伪）** | `VolumeIndex.cpp:873-887` 确为无上限递归，但 `CollectSubtree` 全仓唯一调用方是 MiniSysTests（VolumeIndexTests.cpp:75-78），src 无生产调用；DirectorySize/DirectorySizeParallel 均显式栈迭代（PathUtils.cpp:135-263）；FolderTreeScanner 逐层迭代且 depth 受限（FolderTreeScanner.cpp:101） |

**总评：未发现新的崩溃级（0xC0000005 类）缺陷。** 本轮发现集中在"进程级挂起/僵尸"（R-1）与功能失效/健壮性残余（R-2..R-7）。自 v2.16d 唯一 dump（2026-10-12 01:37）以来 0 新 dump，与代码面结论一致。

---

## 1. 摘要（Top 风险排序）

1. **R-1（P1）RootsDlg 模态循环吞 WM_QUIT → 提权僵尸进程**——上轮 R-2 修了命令面板、漏了同款手写模态循环；-takeover 常态化后可达。
2. **R-2（P2）RootsDlg"浏览添加"取错控件（GetDlgItem(h,1)=IDOK 按钮，应为 1001）**——R-1 修复改了编辑框 id 未同步此处，功能静默失效（非崩溃）。
3. **R-3（P2）关闭/接管尾部**：WM_CLOSE 不等 worker，Shutdown join 在无窗口状态下阻塞（walk 缓存 ~262MB 写盘）；-takeover 新实例等待 10s+5s 失败后 `existing==nullptr` 时**静默退出**，轮换结果可能是"两个实例都没活"。
4. **R-4（P3）BoldUiFont 静态缓存一次失败即永久 nullptr**；HFONT 进程级不删（可接受）。
5. **R-5（P3）SupplyDispInfo 兜底分支未判 pszText/cchTextMax**（理论性，comctl 实际恒给缓冲）。
6. **R-6（P3）History UndoSelected 行号对齐 rows_/recs 双份加载**（时序几乎不可达的错位撤销）。
7. **R-7（P3）信息条高度用 base 字体测量、首行 bold 绘制**——行数低估时末行裁剪（视觉缺陷，v2.19 引入）。

---

## 2. 发现清单

### R-1 · RootsDlg 模态循环吞 WM_QUIT → 提权僵尸（P1，Confirmed）
- 位置：`MainWindow.cpp:1220-1225`（模态循环），对照已修复的 `CommandPalette.cpp:160-167,185`。
- 触发：开发缓存根对话框打开期间，主窗被销毁。现实入口：**-takeover 轮换**（main.cpp:25 向旧实例 PostMessageW WM_CLOSE ×50；对话框 owner 虽被 EnableWindow(FALSE)，但 PostMessage 不受禁用影响）或任何编程式 WM_CLOSE。时序：WM_CLOSE → SaveSettings → DestroyWindow(hwnd_) → WM_DESTROY → PostQuitMessage；系统连带销毁 owned 的 RootsDlg；模态循环 GetMessageW 取到 WM_QUIT 返回 0 退出循环——**WM_QUIT 标志被这一次消费掉**；随后 EnableWindow/SetActiveWindow 打在死句柄上（无害），`st.ok==false` 返回；外层 RunMessageLoop 的 GetMessageW 再无 WM_QUIT → **永久阻塞**。进程无窗口、持有单实例互斥体：新实例 10s+5s 后拿不到互斥体且 `existing==nullptr` → 静默 return 0（见 R-3）。
- 后果：提权僵尸进程（v2.17 前启动的实例仍 elevated），轮换工作流断裂，需任务管理器强杀。
- 修复草案：把 CommandPalette 的 WM_QUIT 处理提炼成共享 `RunModalLoop(HWND)`（记住 quitEaten 并在退出前 `PostQuitMessage(msg.wParam)` 回发），RootsDlg/未来所有手写模态循环统一走它；顺带在循环退出后统一 `if (quitEaten) return` 语义。

### R-2 · RootsDlg 浏览按钮操作到 IDOK 按钮（P2，Confirmed）
- 位置：`MainWindow.cpp:1134` `HWND edit = GetDlgItem(h, 1);` vs 编辑框实际 id=1001（:1201）、IDOK 按钮才是 1（:1209）。
- 触发：对话框中点"浏览添加…"并选中目录。EM_SETSEL/EM_REPLACESEL 发给 BUTTON 被忽略 → 路径永不进编辑框。
- 后果：功能静默失效（上轮 R-1 修 id 冲突时改了编辑框 id，此处漏同步）。无崩溃（若 GetDlgItem 返回 NULL，SendMessage(NULL) 也只是空操作）。
- 修复草案：`GetDlgItem(h, 1001)`；建议加断言/枚举常量消除裸数字 id。

### R-3 · 关闭/接管尾部时序（P2，Likely——代码推演，未活体复现）
- 位置：`MainWindow.cpp:764-770`（WM_CLOSE 直接 DestroyWindow，不等 worker）；`SessionService.cpp:1089-1092`（Shutdown join）；`VolumeIndex.cpp:668`（worker 尾部 SaveCacheIfWorthwhile，~262MB 写盘）；`main.cpp:24-52`（takeover 等待与静默退出）。
- 触发：worker 正在跑（索引构建/重扫）时收到 WM_CLOSE/-takeover。
- 后果：① 旧实例无窗口状态下阻塞数秒~数十秒（02-U8 已知，P3 尾巴）；② 新实例 50×200ms + 25×200ms 后放弃，`existing` 已是 NULL 时**不显示旧窗、无任何提示**直接退出（main.cpp:45-52 的 `if (existing)` 分支落空）→ 轮换后可能无实例存活。不崩溃。
- 修复草案：takeover 放弃分支改为 MessageBox 提示"旧实例退出中，请稍候重试"或循环重试拉起；旧实例把 Shutdown 改为"先 cancel + 带超时 join，超时则 detach 并把互斥体让渡标记写盘"（P3 路线图 #5 已有）。

### R-4 · BoldUiFont 失败即永久缓存 nullptr（P3，Likely）
- 位置：`MainWindow.cpp:102-113`。static lambda 初始化在**首次调用**时定格：若首调时 WM_GETFONT 返回 0（理论上首绘早于 WM_SETFONT——现网 CreateControls 末尾统一设字体，实际不会）或 CreateFontIndirectW 失败（GDI 资源枯竭），则整个进程后续永远拿到 nullptr。
- 后果：选中页签粗体/信息条首行粗体永久失效（调用点均 `if (bold)` 判空，**无崩溃**）。HFONT 每进程 1 个、不删除——进程级泄漏，可接受。
- 修复草案：失败时不缓存（`if (!bold) return nullptr;` 放在 static 之外重试），或缓存 `std::optional<HFONT>`。

### R-5 · SupplyDispInfo 兜底分支的缓冲区契约（P3，Speculative）
- 位置：`Presenters.cpp:197-202` 与 `:907-912`。`di->item.pszText[0]=L'\0'` 未判 pszText 非空；`wcsncpy_s(pszText, cchTextMax, …, _TRUNCATE)` 在 cchTextMax==0 时触发 CRT invalid parameter（Release 下为快速失败）。
- 评估：comctl6 的 LVN_GETDISPINFO（绘制路径）恒提供内部缓冲与正 cchTextMax；仅当未来有代码用 LVM_GETITEM 外部缓冲调用才可能踩中。理论风险。
- 修复草案：`if (!di->item.pszText || di->item.cchTextMax <= 0) return true;`。

### R-6 · HistoryPresenter 双份加载的行号错位（P3，Speculative）
- 位置：`Presenters.cpp:946-955`（选中行号来自 rows_，`recs = LoadAll()` 是弹时新拷贝，二者可能不同源）。
- 触发：需在 Refresh 之后、Undo 之前 history.jsonl 增删记录且用户保持选中——Executing 期间 list/tab 全被 SetTaskBusy 禁用、OP_DONE 即 Refresh，实际时序几乎封死；且 UndoRecord 二次验签+可逆性校验兜底。
- 后果：错位撤销非目标记录（数据层面错操作，非崩溃）。
- 修复草案：按 `r.id` 而非行号回匹配（`for sel in rows: recs.find(id==rows_[sel].id)`）。

### R-7 · 信息条测量/绘制字体不一致 → 末行裁剪（P3，Confirmed-视觉）
- 位置：`Layout.cpp:123-144`（DT_CALCRECT 用 base 字体测高，6 行封顶）vs `MainWindow.cpp:154-176`（首行用 bold 绘制且行距 +2px）。bold 宽于 base → 同文本实际折行数可多于测量值 → 高度不足时末行被 `rc.top < bottom` 循环条件截断（不崩溃、不越界）。
- 修复草案：测量时也用 bold 字体（BoldUiFont）+ 与绘制一致的逐行算法；或测量后加 1 行余量。

---

## 3. 近 5 提交逐条复核（git log -5 -p 全读）

| 提交 | 内容 | 复核结论 |
|---|---|---|
| `2a9d653` v2.19a | DrawInfoText 增加 FillRect 擦除 + 首两帧日志 | **未发现崩溃级缺陷**。GetSysColorBrush 失败返回 NULL 时 FillRect 仅无效；`std::count` 有 `<algorithm>`（MainWindow.cpp:30）。实测日志尾部可见 `DrawInfoText: 172 chars, 3 lines` 正常工作 |
| `9b6c47b` v2.19 | BoldUiFont / DrawInfoText / Tab 粗体 / TCM_SETPADDING / Search 并行取 size / Layout 17px | **未发现崩溃级缺陷**。并行取 size 见 H2（证伪竞争）；BoldUiFont 见 R-4（P3）；TCM_SETPADDING 一次性（Controls.cpp:20）；NM_CUSTOMDRAW 见 H4 |
| `0a8783c` v2.18 | FolderTree 多层分析 + 年份时间戳 | **未发现崩溃级缺陷**。扫描器逐层迭代、depth=2 上限；呈现器两遍建树 + 4 轮孤儿匹配有 `stillOrphans.size()==nested.size()` 无进展熔断（Presenters.cpp:708-721）；TVIF_PARAM 下标在双击路径重新拉取 Results 后有 `idx < items.size()` 界（MainWindow.cpp:488-498）；FormatTimeShort buf[40] 足够（16 字符）。功能限制：>5 层深度的孤儿节点被静默丢弃（不显示，不崩溃） |
| `5f23656` v2.17b | walk children 缓冲 pair<wstring,bool>→pair<wstring,uint64_t> | **未发现缺陷**。纯正确性修复（FRN 截断），无内存语义变化 |
| `6eda7be` | 仅 tools/rotate.ps1（不提权） | 非应用代码，无 UI 崩溃面（脚本按黑名单未执行，只读 diff） |

---

## 4. 系统性加固方案

1. **统一模态循环**：抽 `ui/ModalLoop.h` — `bool RunModalLoop(HWND dlg)` 封装 IsDialogMessage/WM_QUIT 回发/退出清理；CommandPalette 与 RootsDlg（及未来对话框）统一替换，根除 R-1 类"吞 WM_QUIT"回归面（上轮已两次出现该模式）。
2. **统一安全投递**：`SessionService::Post` 已盖章 gen；再补两点——(a) 投递前 `IsWindow(hwnd_)` 检查（销毁后静默丢弃而非依赖 PostMessage 失败）；(b) DONE 消息集中在一张 `msg→kind` 表驱动，杜绝新增任务类型漏盖章。
3. **模态单飞（modal interlock）**：进入任何确认/信息模态前置 `taskMode_ = Confirming`（或 ScopedModalGuard），使 TIMER_SEARCH_DEBOUNCE/RETRY、TIMER_VERIFY_LIST、WM_APP_* 应用逻辑在泵期间排队或丢弃；恢复后统一冲刷。可把 H5 描述的"确认框期间 snapshot_ 被换"从"靠 planHash 兜底"升级为"结构不可能"。
4. **跨泵数据按值规则**：把 04-3（ShowItemInfo 字符串拷贝）的规则写进约定——任何跨模态泵的快照数据必须按值拷贝或泵后重取+重验长度；OnExecute 现状安全但依赖三层隐式守卫（过滤器界检/PlanBuilder 界检/PlanMatches），建议泵后显式 `if (items.size() != sizeAtDialog) → 请重新确认`，把隐式变显式。
5. **UI stall 看门狗**（上轮路线图 #1 落地）：RunMessageLoop 内 `GetTickCount64` 包裹 DispatchMessage，>50ms 记日志，>2s 抓线程栈（仅 worker 线程 SuspendThread+StackWalk 或简单记 RIP），为未来"卡死/僵尸"类问题留第一现场。
6. **回归测试思路**：① 单测枚举所有 WM_APP_* 处理器在 gen 失配+各 taskMode 下的早退（现靠人肉对齐）；② Presenters 侧测 SupplyDispInfo 对 row=INT_MAX/col=6/空 cells_ 的契约；③ UI 层最小化：把 RootsDlgProc/RootsDlg 校验逻辑抽纯函数后可测"浏览 id/WM_QUIT"两缺陷；④ takeover 时序用两个测试进程（非本机生产实例）跑互斥体让渡路径。

---

## 5. 修复顺序

- **P0**：（无——本轮未发现崩溃级缺陷）
- **P1**：R-1 RootsDlg WM_QUIT（统一模态循环）。
- **P2**：R-2 GetDlgItem id；R-3 takeover 静默退出提示 + 关闭 join 尾（承接上轮路线图 #5）。
- **P3**：R-4 BoldUiFont 失败重试、R-5 SupplyDispInfo 缓冲判空、R-6 撤销按 id 匹配、R-7 信息条测量字体一致化、加固方案 3/4/5。

---

## 6. 已检查且安全清单（逐项读码验证）

1. **SupplyDispInfo 双实现守卫**（Presenters.cpp:190-214 / 900-915）——H1 成立。
2. **v2.19 并行 size fetch**（SessionService.cpp:540-568）——worker 私有向量、分区写、join 后才入库，UI 经锁拷贝读——无竞争。
3. **v2.18 FolderTree TVIF_PARAM 失配**——双击/右键均不依赖陈旧下标：双击现场重拉 Results+界检（MainWindow.cpp:488-498）；右键走 itemPaths_（HTREEITEM→path）按 path 匹配（Presenters.cpp:814-846）。
4. **索引快照线程纪律**——snapshot_/cells_/checkStateByPath_ 仅 UI 线程触碰（Refresh/RenderItems/ApplyDeadPaths 全在 WM_* 处理器内）；worker 侧只经 resultsMu_ 读写 results_。
5. **ShowItemInfo 模态拷贝（上轮 04-3）存活**（Presenters.cpp:786-798）：TaskDialog 只用局部拷贝 titleCopy/content。
6. **命令面板 WM_QUIT 回发（上轮 R-2）存活**（CommandPalette.cpp:160-185）。
7. **RootsDlg IDOK HIWORD 守卫（上轮 R-1）存活**（MainWindow.cpp:1096-1098）——但衍生 R-2。
8. **gen 盖章与六个 DONE/TASK_STARTED 门槛（上轮 R-3/R-4）存活**（SessionService.cpp:70-88、MainWindow.cpp:612-698）。
9. **StartTask 生命周期**——CAS(None→kind) 保证前任 worker 已置 None；先 `join()` 再 move-assign 新 thread（SessionService.cpp:309-341），无 std::terminate 路径；异常必发 DONE（04-5 修复存活）；worker 捕获 hwnd 按值，向已销毁窗投递仅静默失败。
10. **PostMessage 载荷全部标量**（gen/tab/进度），无堆指针所有权问题；投递失败无泄漏。
11. **UI 栈使用**——DrawInfoText 4KB 缓冲、RootsDlg 堆字符串、消息路径最深 ~8KB，1MB 栈安全；生产代码无递归（见 H6）；LocalizeCategory 递归每层缩 12 字符必有界。
12. **最终化环防护（T-A3）存活**——Finalize 迭代式 DFS + `nodes*2+16` 预算熔断（VolumeIndex.cpp:226-237）；连通环必被预算捕获后整索引拒绝。
13. **Settings::Load/Save**——fstream 无异常位、Json::Parse 失败回退默认、temp+rename 落盘（Settings.cpp:18-75）；WM_CLOSE 路径不会抛。
14. **OperationLog**——LoadAll/Append/UpdateStatus 全 mu_ 保护（OperationLog.cpp:200-322）；`std::stoull` 包 try/catch（:79）。
15. **UpdateExecButton/OnOpenLocation/OnExcludeSelected/OnVerifyList**——全部 `idx >= items.size()` / ItemAtRow 判界。
16. **wcsncpy_s/wcscpy_s/swprintf_s 使用点**——markup 数组 2048、格式化缓冲均足够（StringUtils.cpp:26-38、Logger.cpp:66-84 的 _vscwprintf 两段式）。
17. **Icons.cpp 想象列表换装**——LR_COPYDELETEORG 后不再双删（上轮 R-3 修复存活）；失败路径 DestroyIcon/ImageList_Destroy 配对完整。
18. **presenter/HWND 生命周期**——presenters_ 为 MainWindow 内 C 数组 unique_ptr，MainWindow 栈对象活到 RunMessageLoop 之后（Shutdown 先行）；presenter 只持 HWND 值拷贝不拥有。
19. **WM_DPICHANGED/WM_GETMINMAXINFO/最小尺寸**——早于 WM_NCCREATE 的消息经 USERDATA 空判走 DefWindowProc，安全。
20. **模态期间意外任务**（H5 深查）——OnExecute 泵内 snapshot_ 可被换，但泵后仅 PlanBuilder::Build（逐 idx 界检）+ PlanMatches 兜底读；未检查界的循环全部位于对话框**之前**——不构成 UAF/OOB。

## 7. 未验证 / 局限

- **未活体复现**任何发现（对运行中进程与仓库只读约束）；R-1/R-3 为代码级时序推演（标注 Confirmed=代码路径确定存在、触发依赖时序；Likely=推演成立未复现）。
- **dump 未重新符号化**：仅一处历史 dump（2026-10-12 01:37，v2.16d 前），根因已由上轮定位且修复经本轮代码核实；本轮未引入新 dump 工具链分析（无符号栈不采信"最近导出符号"）。
- comctl6 LVN_GETDISPINFO 的 pszText/cchTextMax 契约基于文档与惯例，未做内核级实证（R-5 因此标 Speculative）。
- History 错位撤销（R-6）未构造复现；触发时序经分析近乎封死，保留 Speculative。
- tools/rotate.ps1 / build.bat / incr_build.bat / screenshot.ps1 按黑名单未执行；takeover 行为结论全部来自 main.cpp 读码。
- Settings::Save 在 WM_CLOSE 的写盘耗时、262MB 缓存写尾时长均未实测（引用上轮 02/03 报告口径）。
