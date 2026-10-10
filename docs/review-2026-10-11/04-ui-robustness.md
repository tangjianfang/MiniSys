# MiniSys UI 稳定性审查(崩溃风险专项)——第四轮 04 分报告

> 日期:2026-10-11 · 对象:HEAD `5cda187`(v2.14)工作树 · 范围:`git log 4c55ca0..HEAD` 全部新代码(v2.13 并行 walk → v2.13c tour/视觉轮 → v2.14 签名链)
> 必读已读:docs/REVIEW_2026-10-06-3.md、docs/review-2026-10-06-3/04-ui-robustness.md(上轮本角色,R-1..R-5)、docs/evolve-log.md。
> 方法:全部发现亲自读码(MainWindow.cpp/h 全文、SessionService.cpp/h 全文、VolumeIndex.cpp 关键 900 行、CommandPalette.cpp 全文、Presenters.cpp/h 全文、OperationLog.cpp/h 全文、SecretStore.cpp 全文、Icons.cpp、Settings.cpp、DevBuildCache.cpp 走查段;git diff 4c55ca0..HEAD 逐文件)。未运行程序、无 TSAN(见 §7)。

---

## 1. 结论:审计线索证实 / 证伪表

| # | 线索(任务清单) | 裁定 | 依据 |
|---|---|---|---|
| 1a | VerifyPathsAsync 一次任务连发多个 VERIFY_DONE 的代数一致性 | **证伪(单发)+ 新发现 IdleMaintenance 侧竞态(R-4)** | VerifyPathsAsync 全路径仅一处 `Post(WM_APP_VERIFY_DONE)`(SessionService.cpp:999);而 IdleMaintenanceAsync 每个被清剪页 Post 一次(480),多消息共享 lastDead_/lastVerifyTab_ 单槽 → 见 R-4 |
| 1b | 多页 VERIFY_DONE 在新任务启动后被代数闸丢弃 | **证实但后果有界** | 闸在 MainWindow.cpp:479;服务侧已 StoreResults+SaveResultsCache,仅 presenter 侧应用丢失,切页自愈 |
| 2 | v2.11 For/TotalEntries/ValidVolumes 锁 | **证实安全** | RegistryMu 为叶子锁(VolumeIndex.cpp:89-134);UI 线程只碰 valid_/entryCount_ 原子;worker 持锁仅 map 查找,无锁序环 |
| 3 | v2.13 并行 WalkBuild(condvar 队列/appendMu/cancel) | **主体正确;2 项发现** | 终局条件 `queue.empty() && active==0 → done`(614)、appendMu 全覆盖 nodes_/names_/nextFrnVal(590-603)、cancel 经 done+notify_all 传播(557-563)——均正确。发现:池线程无 try/catch(R-7);根枚举失败仍 walkBuilt_=true(R-8) |
| 4a | 命令面板 局部消息循环 / WM_QUIT 逃生 | **证实缺陷:无 WM_QUIT 逃生** | CommandPalette.cpp:139 `while (!st.done && GetMessageW(...)>0)` — WM_QUIT 被**吞掉**且 st.wnd 已被 owner 销毁链毁掉 → 主循环 GetMessage 永久阻塞(R-2) |
| 4b | RootsDlg 模态 EnableWindow 配对 / 消息路由 / WM_QUIT | **EnableWindow 配对正确;消息路由缺陷 1 项** | FALSE/TRUE+SetActiveWindow 配对(MainWindow.cpp:979/990-991);X 钮 WM_CLOSE 经 DefWindowProc 销毁、循环经 IsWindow 短路退出(不吞 WM_QUIT);但 `case IDOK` 无 HIWORD 判别(R-1);外部 PostThreadMessage(WM_QUIT) 仍致孤儿窗口+悬垂栈指针(R-10) |
| 5 | -tour 定时器切页与 SetTaskBusy/SaveSettings 交互 | **证实无崩溃级;3 项边角** | TIMER_TOUR 直接 TabCtrl_SetCurSel 绕过被禁用的 tab 控件(507-512 vs 1413);扫描中切页致扫描钮文字与语义脱节(1125 vs 1418);OnScanDone 按"完成时刻的活动页"组合信息(1312)。每 4s SaveSettings 仅磁盘churn |
| 6a | Append 锁内调 GetOrCreateMachineSecret — 死锁/重入 | **证伪(无死锁);时延记档** | OperationLog.cpp:226/233 mu_ 内调注册表;SecretStore 无内部锁、注册表句柄即刻释放,纯叶子调用,无锁序环。见 R-12 |
| 6b | SignUnsignedLegacy 锁内 RewriteAtomic | **证实(设计内)+ UI 线程执行** | OperationLog.cpp:254-274;全量重写在大 history 上是 UI 线程冻结点(当前量级无害) |
| 6c | 撤销拒绝路径的 UI 提示 | **预检提示在;worker 侧理由被丢弃** | 预检提示齐全(Presenters.cpp:867-881);UndoRecord 设置的 errOut "记录缺少有效签名…" 在 UndoRecordsAsync 的 NotReversible 分支被丢弃(SessionService.cpp:898-900 vs 929-933)→ R-11 |
| 7 | VK_TAB/VK_RETURN/VK_ESCAPE 手写拦截 × IsDialogMessage | **主循环安全;面板侧证实缺陷** | 主循环(141-179)在所有模态(系统对话框/两处自绘模态循环)期间挂起,不冲突;Enter 仅焦点在 editSearch 且 taskMode_==None;Esc 各弹窗分别由系统/面板 break/RootsDlg IsDialogMessageW 处理。但面板循环**全线程**拦截 WM_KEYDOWN 且不禁用 owner → R-9 |
| 8 | R-4 任务代数修复的六个 DONE/STARTED 处理器 | **五处正确;WM_APP_TASK_STARTED 一处被修复本身破坏** | MainWindow.cpp:449/459/466/471/479/484 均闸 gen;但 Presenters.cpp:771/888 直发 `PostMessageW(..., WM_APP_TASK_STARTED, 0, 0)`(wp=0)与 `wp == CurrentTaskGen()`(≥1)永不相等 → R-3 |

**总体判断:常规操作路径未发现内存不安全级(UAF/悬垂写)缺陷;本轮最重的是两条"进程级挂死/对话框自毁"路径(R-1/R-2)与一处由 R-4 修复引入的矩阵解锁回退(R-3)。与"无崩溃转储"观测一致——R-2 的表现恰是僵尸进程而非转储。**

---

## 2. 摘要(Top 风险)

| 排名 | 发现 | 等级 | 类型 |
|---|---|---|---|
| 1 | R-1 RootsDlgProc `case IDOK` 不判 HIWORD——EDIT 控件号=IDOK=1,首个 EN_CHANGE(打一个字/浏览追加)即走 IDOK 销毁对话框并"保存";叠加 R-6 后多行内容全丢、**已有 devCacheRoots 被清空** | P1 | 功能自毁+设置数据丢失(100% 可复现) |
| 2 | R-2 palette::Show 局部循环吞 WM_QUIT:面板打开时点主窗口 X(owner 未禁用)→ 主窗销毁(owner 链连带毁面板)→ WM_QUIT 被面板循环消费 → 主循环 GetMessage 永久阻塞 → **提权僵尸进程**(无转储、不可见) | P1 | 退出路径挂死 |
| 3 | R-3 v2.10 代数闸使 `WM_APP_TASK_STARTED`(wp=0 直发)永不匹配 → 文件夹树隔离、历史批量撤销两条执行路径**矩阵不锁**:无进度条、tab 可切、扫描钮文字失真 | P2 | 状态机回退(修复引入) |
| 4 | R-4 IdleMaintenance 多页 VERIFY_DONE 共享 lastDead_/lastVerifyTab_ 单槽,worker 在 UI 消费前覆写(SaveResultsCache 磁盘写放大窗口)→ 死路径应用到错误 presenter / 某页永不应用 | P2 | 数据竞态(错误数据级) |
| 5 | R-5 DevBuildCache 目录缓存被取消/瞬时 ListDir 失败投毒:walk 在 cancel 时 `return {}`,祖先把"不完整子树"按未变 mtime 入缓存 → 本会话后续扫描**静默漏项**;且 kMaxDirs 上限只在根间检查,递归内部不受限 | P2 | 缓存投毒(漏报) |
| 6 | R-6 `flush;` 漏 `()` 两处(MainWindow.cpp:877/1011)——多行路径解析永不切分 | P2 | 功能(与 R-1 叠加成数据丢失) |

---

## 3. 发现清单(近期改动优先;置信度三档)

### R-1(P1 · 置信度:高)v2.12 RootsDlg:EN_CHANGE 被当 IDOK,对话框首个按键即自毁

- **位置**:`MiniSys/src/MainWindow.cpp:858-885`(RootsDlgProc `case WM_COMMAND: switch (LOWORD(w)) { case IDOK:`);编辑框控件号 1(961-965,`mk(L"EDIT", ..., 1)`),"确定"按钮亦 IDOK=1(970-973)。
- **触发**:打开命令面板 →"设置开发缓存扫描根…"→ 在多行编辑框**输入任意一个字符**(或点"浏览添加…"——其 `EM_REPLACESEL` 同样触发 EN_CHANGE)。EDIT 的 EN_CHANGE 通知 = `WM_COMMAND(HIWORD=EN_CHANGE, LOWORD=1)`,`switch (LOWORD(w))` 命中 `case IDOK` → 读文本、`st->ok = true`、`DestroyWindow(h)`。
- **后果**:① 对话框在第一次修改文本的瞬间"伪确定"关闭——键盘录入路径完全不可用;② 叠加 R-6(joiner 不切分):多行内容拼成单串 → 校验丢弃 → `settings_.devCacheRoots = ""` **清空已有根**(1015-1016 SaveSettings);"浏览添加"看似成功(追加后立即关闭并保存),实际当已有 ≥1 行时同样清空。v2.13b 已在命令面板修过同族 bug(V1,CommandPalette.cpp:48-63 按通知码判别),未回灌 RootsDlg。
- **修复**:`case IDOK:`/`case IDCANCEL:` 前按 `HIWORD(w) == BN_CLICKED` 判别(照抄 CommandPalette.cpp:52-63 的写法);同时把 EDIT 控件号改为非 1(如 101)以绝后患。

### R-2(P1 · 置信度:高)v2.13c palette 局部消息循环吞 WM_QUIT → 主循环永久阻塞(提权僵尸进程)

- **位置**:`MiniSys/src/ui/CommandPalette.cpp:139`(`while (!st.done && GetMessageW(&msg, nullptr, 0, 0) > 0)`)+ `MainWindow.cpp:546-551`(WM_CLOSE→DestroyWindow→PostQuitMessage)。palette 全程未 `EnableWindow(owner, FALSE)`。
- **触发**:Ctrl+K 打开面板 → 鼠标点主窗口 X(主窗口仍启用可点)→ WM_CLOSE 由**面板循环**分发 → SaveSettings + DestroyWindow(主窗)→ owner 链连带销毁面板(其 WM_DESTROY 仅 return 0)→ 主窗 WM_DESTROY `PostQuitMessage(0)` → 面板循环的下一次 `GetMessageW` 取到 WM_QUIT 返回 0 → 循环退出(IsWindow 销毁跳过 DestroyWindow,命令不执行)→ 返回主循环 `while (GetMessageW(...) > 0)`——**WM_QUIT 已被消费**,无窗口、无定时器 → GetMessage 永久阻塞。
- **后果**:进程不退出、不可见、不可交互(提权进程僵尸;任务管理器可见)。无转储——与"0 崩溃"观测不矛盾但属最违和的稳定性缺陷。
- **修复**:循环退出后判 `msg.message == WM_QUIT` 则 `PostQuitMessage((int)msg.wParam)` 再返回;并把循环条件改为 `while (IsWindow(st.wnd) && !st.done && ...)`(IsWindow 短路可在 owner 销毁后不再取消息)。RootsDlg 的循环条件顺序(`IsWindow(dlg) &&`)恰好天然规避本路径,已核。

### R-3(P2 · 置信度:高)v2.10 R-4 修复引入:WM_APP_TASK_STARTED(wp=0)与代数闸永不相等 → 两条执行路径矩阵不锁

- **位置**:发送方 `MiniSys/src/ui/Presenters.cpp:771`(FolderTree 右键隔离)、`Presenters.cpp:888`(历史批量撤销)——均为 `PostMessageW(ui_.main, WM_APP_TASK_STARTED, 0, 0)`;接收方 `MainWindow.cpp:459`(`wp == SessionService::Instance().CurrentTaskGen() && IsBusy()`);`SessionService.cpp:307` `taskGen_.fetch_add(1)` 在 StartTask 内、返回 true 之前执行 → 任务运行期 `CurrentTaskGen() >= 1`,wp=0 永不等于。
- **触发**:文件夹树页右键"移入隔离区…"确认,或历史页多选"撤销选中操作"。全仓仅这两处发送 TASK_STARTED(grep 证实),不经 `SessionService::Post` 的 wp=0→gen 补戳(Post 只补自身调用,SessionService.cpp:74-79)。
- **后果**:执行期间 `SetTaskBusy(TaskMode::Executing)` 不发生:进度条不显示(OP_PROGRESS 打到隐藏的 bar 上)、tab/列表/执行钮保持可操作、`taskMode_` 仍 None(各入口靠 `svc.IsBusy()` 二道闸拦住实际动作——故无崩溃/无错位执行,但"执行期间锁定矩阵"契约失效、界面误导)。OP_DONE 侧 gen 正确,执行仍正常收尾。
- **修复**:两处改 `PostMessageW(ui_.main, WM_APP_TASK_STARTED, SessionService::Instance().CurrentTaskGen(), 0)`;或在 SessionService 增加 `PostTaskStarted()` 封装统一戳 gen。

### R-4(P2 · 置信度:高)IdleMaintenanceAsync 多页 VERIFY_DONE 与 lastDead_/lastVerifyTab_ 单槽覆写竞态

- **位置**:worker 侧 `SessionService.cpp:473-480`(每页:`{verifyMu_; lastDead_=…; lastVerifyTab_=tab}` → `Post(WM_APP_VERIFY_DONE)` → 立即继续下一页的 `Results(tab)` 拷贝+索引查询+`StoreResults`+`SaveResultsCache`(磁盘写,数千行级数十 ms));UI 侧 `MainWindow.cpp:750/754`(`LastDeadPaths()` 与 `LastVerifyTab()` **两次独立加锁**读取)。
- **触发**:空闲维护同时清剪 ≥2 页且 UI 线程忙(千行渲染/弹窗)超过 worker 处理下一页的耗时:UI 处理第 1 条 VERIFY_DONE 时 worker 已把单槽覆写为第 2 页数据 → 第 2 页死路径被应用两次(tab 对应 presenter 无害),第 1 页永不应用;两次独立锁读取亦可撕裂出 dead/tab 不属同一页的组合。
- **后果**:错误数据级:被漏应用页的 presenter 快照残留幽灵行(服务侧 results_/磁盘缓存已剪),切页 Refresh 自愈;期间执行会因 planHash 判 PlanStale 拒绝(安全兜底在)。无崩溃。
- **修复**:VERIFY_DONE 的 wp/lp 不足以携带负载又不许传指针——建议 worker 把 `(tab, dead)` 逐页**先 Post 再覆写前等待 UI 确认**过重;轻量解:单锁内成对读取(UI 侧加 `LastVerifyResult(tab, dead)` 一次加锁返回两者)+ worker 侧每页结果放入 `std::vector<pair<TabId, vector<wstring>>>` 快照,UI 按消息序号取(或干脆每页独立 VERIFY_DONE 携带页号于 lp,worker 侧数组按下标取,处理完清引用)。

### R-5(P2 · 置信度:高)DevBuildCache v2.13 目录缓存:取消/瞬时失败投毒 + kMaxDirs 上限失效

- **位置**:`MiniSys/src/core/DevBuildCache.cpp:212`(`if (cancel.load()) return {};`)、`241`(`if (!ListDir(dir, entries)) return {};`)、`270`(`dirCache[key] = DirEntryCache{dirMtime, subtree}` — 祖先把缺了子树的结果按**未变 mtime** 入缓存)、`276`(visited/kMaxCandidates 上限只在根循环间检查,walk 递归内部无 visited 上限)。
- **触发**:垃圾扫描进行中点"扫描"钮取消(CancelScan)→ walk 中途返回空 → 各祖先缓存不完整子树;下次扫描同 mtime 命中缓存 → **本会话静默漏报开发缓存项**(用户取消重扫是常见操作)。ListDir 瞬时失败(AV 占用/ACL)同理。旧版栈循环在 while 条件里检查 `visited < kMaxDirs`,新版失效 → 单根深树可无限走访(内存/时长放大)。
- **后果**:漏报(错误数据级,无崩溃);缓存会话级,mtime 变化后自愈。
- **修复**:walk 带 `bool& poisoned` 或以"取消/失败即向祖先传播失效标记,取消时不写 dirCache";`kMaxDirs` 检查移入 walk 入口。

### R-6(P2 · 置信度:高)`flush;` 漏调用括号两处 —— 多行扫描根解析失效

- **位置**:`MainWindow.cpp:877`(`if (ch == L'\n') flush;`)与 `MainWindow.cpp:1011`(`if (ch == L';') flush;`)。语句 `flush;` 只求值 lambda 不调用(C4551 类笔误),`cur` 永不落盘到 `joined`。
- **触发/后果**:RootsDlgProc 的 joiner 把所有行拼成一串(无 ';'),OnDevCacheRoots 的解析器(1011)亦不切分 → 多行内容整体无效被丢。是 R-1"清空已有根"的放大器。
- **修复**:`flush();` 两处;补单测(两函数纯字符串逻辑,可直接测)。

### R-7(P3 · 置信度:高[代码路径]/触发概率:低)WalkBuild 并行池线程无异常保护

- **位置**:`VolumeIndex.cpp:634-637`(`pool.emplace_back(worker)` ×6;worker lambda 无 try/catch)。对照:SessionService 主 worker 有 catch(...)(SessionService.cpp:314-322)。
- **触发**:百万级 walk(实测 2,831,802 项)中 `names_ += name` / `nodes_.push_back` 抛 bad_alloc(低内存机器)→ std::terminate 直接终止进程。CrashDump 的 SEH 过滤器通常能兜住 C++ 异常,但语义上属未受控终止。
- **修复**:worker lambda 包 try/catch,置 `cancelled/failed` 标志后返回。

### R-8(P3 · 置信度:中)RefreshWalk 无卷重验,根枚举失败 → "仅根节点"的合法空索引 → 空闲维护可全量误剪

- **位置**:`VolumeIndex.cpp:639`(`walkBuilt_ = !nodes_.empty() && !cancelled` — 根节点在池启动前已 push,恒非空)、`659-671`(RefreshWalk 不做 GetVolumeInformationW/serial 复验;成功即 Finalize+entryCount)。EnsureBuilt 路径有 serial 复验(283-300),RefreshWalk 没有。
- **触发**:walk-built 卷在空闲维护重扫时被卸载/失联(VHD 固定盘 dismount)→ 根 FindFirstFile 失败 → nodes_=[root] 仍 walkBuilt_=true → Finalize 后索引"合法"含 1 项 → IdleMaintenance 第 2 步逐行 `TryGetEntry` 全失败 → 该卷上所有缓存页全量判死、StoreResults+SaveResultsCache **清空并落盘**。
- **后果**:错误数据级(可重扫恢复),概率低;建议 walk 后 `if (visited <= 1 || nodes_.size() <= 1) valid_=false`。

### R-9(P3 · 置信度:高)palette 全线程拦截按键且不禁用 owner —— 焦点离开面板后 Enter/方向键被面板劫持

- **位置**:`CommandPalette.cpp:140-158`(拦截不判 `msg.hwnd`/GetFocus 归属);`Show` 全程无 `EnableWindow(owner, FALSE)`。
- **触发**:打开面板 → 鼠标点主窗口(可激活、可输入)→ 在主窗口搜索框按 Enter(本意执行搜索)→ 面板循环把该 Enter 当"执行选中命令"→ 可能直接触发如"清空隔离区…"(仍过确认框)/"重建索引"。Esc 在主窗口任意处也会关面板、↑↓ 动的是面板列表。
- **后果**:误动作面(有各命令自身闸门兜底);同时"模态"名不副实,面板期间可启动任务/切页。
- **修复**:拦截前判 `GetFocus()` 属于 st.wnd 子树;或干脆 `EnableWindow(owner, FALSE)` 变真模态(需同步处理 R-2)。

### R-10(P3 · 置信度:高[代码路径]/触发概率:极低)RootsDlg 外部 WM_QUIT → 孤儿窗口 + 悬垂 GWLP_USERDATA

- **位置**:`MainWindow.cpp:984`(`while (IsWindow(dlg) && GetMessageW(...)>0)`)循环因 GetMessage 返回 0(WM_QUIT,仅可由外部 PostThreadMessage 送达——仓内唯一 PostQuitMessage 在主窗 WM_DESTROY,而该路径会连带销毁 dlg,已被 IsWindow 短路化解)退出时 `dlg` 未销毁,`RootsDlgState st`(栈,936)指针仍挂在 GWLP_USERDATA;随后 EnableWindow/SetActiveWindow 后返回,主循环因 WM_QUIT 被吞而阻塞,孤儿 dlg 可交互,IDOK 写 `st->joined` = **写死栈**。
- **修复**:循环后 `if (IsWindow(dlg)) DestroyWindow(dlg);` + WM_QUIT 转发(与 R-2 同一模式统一封装)。

### R-11(P3 · 置信度:高)撤销拒绝理由被丢弃(签名拒绝的 UI 提示缺口)

- **位置**:`SessionService.cpp:929-933`(UndoRecord 拒绝时 errOut="记录缺少有效签名(可能被篡改或为旧版记录),已拒绝还原")vs `SessionService.cpp:898-900`(UndoRecordsAsync NotReversible 分支只打"该记录不可撤销",errOut 未用)。
- **后果**:混选(部分合法+部分未签名)时,执行后弹窗里未签名项无理由显示;纯未签名在预检提示(Presenters.cpp:876-877)覆盖。补一行把 errOut 拼进 details 即可。

### R-12(P3 · 置信度:高)注册表/文件 IO 的时延与竞态杂项(均非死锁)

1. `OperationLog::Append` 持 mu_ 调 `GetOrCreateMachineSecret`(OperationLog.cpp:226/233,注册表读/首建写)+ RunPlan 每项一次 Append → 执行 N 项 = N 次注册表读;`HistoryPresenter::UndoSelected` 预检**每选中行一次** VerifySignature=一次注册表打开(Presenters.cpp:870,UI 线程,大选区可感知);`SignUnsignedLegacy` UI 线程全量重写(OperationLog.cpp:271)。无死锁(叶锁);建议 secret 进程内缓存(std::call_once)。
2. `Settings::Save`(UI,tmp+MOVEFILE 替换)× `DevBuildCache::SearchRoots`→`Settings::Load`(worker,DevBuildCache.cpp:163)并发:替换对已打开句柄失败(MSVC ifstream 不带 FILE_SHARE_DELETE)→ 偶发**设置保存静默失败**;tour 模式每 4s 一次 Save 放大碰撞面。
3. `-tour` 相关边角(非缺陷级):扫描/执行中 tour 绕过被禁 tab 控件切页——数据面全为锁拷贝、无崩溃;扫描钮文字在扫描中被 OnTabChanged 重置为"扫描"(实际语义仍是取消,1125 vs 1418);OnScanDone 按"完成时刻活动页"组合信息页文案(错误信息级,自愈)。

---

## 4. 系统性加固方案

1. **局部模态循环统一封装**(治 R-2/R-10 一族):`RunModalLoop(HWND popup, doneFn)` —— 条件 `IsWindow(popup) && !done()`、退出统一 `if (IsWindow) DestroyWindow`、GetMessage 返回 0 时 `PostQuitMessage(msg.wParam)` 转发。palette/RootsDlg/未来任何自绘模态共用;这是仓库内第三处手写循环,已出现三种三种行为。
2. **WM_COMMAND 分发规范**:自绘对话框的 `switch(LOWORD(w))` 必须先判 `HIWORD(w)==BN_CLICKED`(按钮类)/`EN_CHANGE`(编辑类);控件 id 分配避开 1/2(IDOK/IDCANCEL)。CI grep:`case IDOK` 所在函数内无 `HIWORD` 即告警。
3. **发送方/接收方消息契约表**:每个 WM_APP_* 消息登记 wp/lp 语义与发送 API(SessionService::Post 补戳 vs 裸 PostMessageW);R-3 正是裸发破坏契约所致。grep 闸:除 SessionService::Post 外禁止 PostMessageW 发 TASK_STARTED/DONE 族。
4. **单槽结果 → 消息序号化**:VERIFY_DONE 的多页结果入定长环形数组、lp 携下标,UI 处理后清槽;消除"worker 覆写先于 UI 消费"整类竞态(与 §R-4 一并)。
5. **取消必须使缓存失效**:任何"增量缓存"(DevBuildCache dirCache、将来他处)在取消/子任务失败路径上一律丢弃而非写入不完整结果;walk 型操作以"完成位"守门写缓存。
6. **lambda 调用笔误 grep**:`;\s*;` 查不出,但 `\b[a-zA-Z_]+;\s*$` 出现在"名为动词的 lambda 变量"行可静态告警;或干脆禁止裸语句 lambda 名(改 `std::function` 显式调用)。

---

## 5. 修复顺序建议

- **P0(无)**:未发现常规路径内存不安全级缺陷。
- **P1(立即)**:R-1(6 行:HIWORD 判别 + EDIT 改 id)、R-6(2 字符 ×2 处,与 R-1 同提交)、R-2(palette 循环 4 行:IsWindow 短路 + WM_QUIT 转发)。
- **P2(本周)**:R-3(两处 wp 携 gen)、R-4(VERIFY_DONE 成对单锁读 + 结果序号化)、R-5(取消不写缓存 + kMaxDirs 内移)。
- **P3(择机)**:R-7..R-12;§4-1/4-2/4-3 封装与 grep 闸。

---

## 6. 已检查且安全清单(本轮逐项核码;含上轮修复存活核对)

1. **上轮 R-1 存活**:OnVerifyDone 仅活动页渲染,`ApplyDeadPaths(dead, /*render=*/active)`(MainWindow.cpp:762-763;Presenters.cpp:380)。
2. **上轮 R-2 存活**:IDM_LIST_SELECTALL/SELECTNONE 有 SetBatchUpdate 包裹(401-415)。
3. **上轮 R-3 存活**:Icons.cpp:26-32 无 double-destroy,失败路径保留源句柄。
4. **上轮 R-4 存活且扩展**:六个 DONE/STARTED 处理器全部 gen 闸(449/459/466/471/479/484);StartTask 的 gen 自增先于线程启动、DONE 全部在 taskKind_!=None 期间发出 → 同任务所有 DONE 携同 gen,机制正确(副作用见 R-3)。
5. **上轮 R-5 存活**:SearchPresenter::Refresh 先 CaptureCheckState(Presenters.cpp:562-572)。
6. 04-5 契约存活:DoneMessageFor 全 kind 覆盖、异常路径补发、StartTask wrapper catch(...)(SessionService.cpp:35-45/314-331)。
7. 单 worker 串行 + StartTask CAS + join-before-start(302-334):无 double-thread;Shutdown 双调安全。
8. results_/scanAt_/fromCache_ 全链 resultsMu_ 锁拷贝;ApplyDeadPaths 的 StoreResults 与 worker 侧剪除幂等(比较按 path)。
9. LastReport/LastPreview 的 const& 消费仍安全:OnPlanDone/OnPreviewDone 在弹模态前完成字符串拷贝,模态期间 owner 禁用 → 无并发写者;引用绑定的成员属单例,生命周期无忧。
10. VolumeIndex TryLoadCache:MAC 先验后析、nameOff/len 边界、kMaxNameLen、FRN 环预算(T-A2/T-A3/T-A4 全部在位,1159-1284);SaveCacheIfWorthwhile 写-验对称(walkFlag B-1 修复在位,1319)。
11. RegistryMu 为叶子锁;Finalize/TryGetEntry/Search 不持它;UI 线程经原子读 valid_/entryCount_;WalkBuild 的 appendMu/qMu/cv 终局条件正确,cancel 经 done+notify_all 全员传播,join 后才读结果。
12. GetOrCreateMachineSecret 无内部锁、注册表句柄即开即关,OperationLog mu_ 内调用不构成锁序环(逐调用点核:Append/VerifySignature/SignUnsignedLegacy/TryLoadCache/SaveCache)。
13. RootsDlg EnableWindow FALSE/TRUE + SetActiveWindow 配对;X 钮经 DefWindowProc 销毁、IsWindow 短路退出且不吞 WM_QUIT;palette WndProc 在 USERDATA 设置前的创建期消息不触解引用(仅 WM_COMMAND/WM_CLOSE 分支用 st)。
14. 主循环 VK_TAB/VK_RETURN/VK_ESCAPE 拦截与各模态不冲突(所有模态循环挂起主循环);Ctrl+A 焦点在 EDIT 时选文本(383-391);Esc 在系统对话框/面板/RootsDlg 各自正确。
15. HistoryPresenter 行号-lParam-recs 对齐(每次 Refresh 全量重建);OP_DONE 后 Refresh 收敛;UpdateStatusBulk/RunEmptyQuarantine 按盘标记,gen 递增使 qCache 失效。
16. OnOpenLocation 路径剥引号(T-A8 在位);GuardRails 委派白名单双层复验在位(RunPlan 696 + TryLoadCachedResults 276)。
17. WM_ACTIVATE 取消空闲维护(B-3)链路:CancelScan → RefreshWalk/EnsureBuilt 检查点退出;后续剪枝循环无 cancel 检查但纯索引查询、快,无崩溃面(记为观察项)。
18. 单实例互斥、CoInit、加速键归属(模态内 TranslateAccelerator 不跑,反而阻断模态期加速键重入)。

**被推翻/证伪的假设汇总**:§1 之 1a(VerifyPathsAsync 多发)、2(注册表锁死锁)、6a(死锁/重入)、7(主循环按键与模态冲突)、5(tour 崩溃级)、以及"IdleMaintenance 的 VERIFY_DONE 全部按预期页应用"(实为 R-4 竞态)。

---

## 7. 局限

- 静态读码 + 消息序/线程交错推演,未运行程序、无 TSAN/Application Verifier;R-1/R-2/R-3/R-6 为可人工复现路径(建议按 §3 各"触发"步序实测一次确认),R-4/R-5 概率基于耗时估算(SaveResultsCache 数千行 ~10ms+ 级)。
- EN_CHANGE 在 WM_SETTEXT 时不发、EM_REPLACESEL/键盘输入时发——依据为 Win32 既定语义与实绩行为;R-1 的"首字符即关"建议活体确认(浏览追加路径 100% 触发)。
- 用户数据目录只读列示,未动;黑名单(build.bat、tools\*)未触碰;仓库零修改。
- 未覆盖:GuardRails 名单完备性、DelegateOp/QuarantineOp 执行细节(安全/IO 角色域);Json 解析器内部;MiniSysTests。
