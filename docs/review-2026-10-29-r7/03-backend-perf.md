# MiniSys 第七轮评审 r7 · 03 后台线程 / 索引 / 缓存 / 扫描层

> 日期 2026-10-29 · 对象 HEAD `6621d92`（v2.23）· 方法：全量读码（SessionService / VolumeIndex / DelegateOp / FolderTreeScanner / PathUtils(SizePool) / DirSizeCache / FastWalk / JunkScanner / LargeFileScanner / DevBuildCache / OperationLog / Logger / main / MainWindow 关键区段）+ 用户真实数据**只读**测量（10-29 全天日志 1334-1389 行逐条取证、index-cache-C.bin 头部与分段计时、history.jsonl/results-*.json 尺寸）+ 两项编译实验（`.tmp-review/r7/03-exp/`，MSVC /O2 x64，cl 经 vcvars64，产物只在 r7 目录）。
> 规模基线：缓存内 3,087,989 项（10-14 版）；10-29 walk 实测 3,427,118 项 / 469,811 目录；C: 卷 serial b22dc436；应用 asInvoker（elevated=0）。

---

## 0. 摘要：Top 风险与"整体卡顿"的后端归因

| # | 结论 | 证据强度 |
|---|---|---|
| 1 | **"重建索引"的 45s walk 成果永不落盘**：`EnsureBuilt` force 分支提前 return 不 Save，且 `WalkBuild` 内部的 `SaveCacheIfWorthwhile` 因此时 `valid_` 尚为 false 而**恒为 no-op**。用户缓存 mtime 停在 **10-14 02:12**——10-29 01:34:51 的 3,427,118 项 walk 后缓存未更新，下次启动仍加载 10-14 的旧索引，重建可以无限循环 | Confirmed（代码链 + mtime + 日志时间线） |
| 2 | **walk 索引 journal cursor 恒 0，USN 增量永不可用**：`RefreshFromUsn` 首行 `if (walkBuilt_) return true`。asInvoker 下每次"刷新"唯一路径 = idle maintenance 的 `RefreshWalk` = **45-50s、6 线程、普通 I/O 优先级**全盘 walk + 无条件重写 ~200MB 缓存（逐行写实测 880-1042ms）。用户"放着不动也卡"的主因 | Confirmed（代码 + 缓存头实测 walk=1 journal=0 + 10-29 walk 日志 45.1s） |
| 3 | **冷启动载入+Finalize ≈5.0s**（r6 B-4 未修）：实测 DFS dirPaths 3,398ms 主导，全在唯一 worker 上；工作集一步 +804MB | Confirmed（真实缓存只读复测） |
| 4 | **维护取消后索引失效不自愈**：取消的 re-walk 置 valid_=false 但 `ready_` 仍 true、查找表残留指向半棵树；`SearchAsync` 不触发重载 → 用户回到键盘后的搜索**持续 0 结果**，直到切页/垃圾扫描/手动重建 | Confirmed（代码链） |
| 5 | **v2.23 惰性展开 = 每次展开该子树全深度遍历**（`DirectorySizeParallel(kid,4)`，SizePool 无取消、无缓存）；v2.18 的 depth=2 双倍全盘扫描原样未动（r6 B-5 未修） | Confirmed（代码） |
| 6 | Executing 类（隔离/迁移/DISM/清空隔离区 2.5GB 逐文件 remove_all）零取消检查点 + `Shutdown()` 无界 join；DelegateOp reader detach 分支 UAF 仍在（r6 B-9/B-10 未修） | Confirmed（代码） |

**对"整体卡顿"的后端归因**（按证据强度排序）：
①**结构性**：单 worker 无队列，任何分钟级任务（walk 45s、FolderTree 16.7s、LargeFiles 9-25s、执行/清空隔离区）占用期间一切交互任务被 `IsBusy` 拒绝，搜索靠 80ms 定时器空转重试——"点了没反应"。②**周期性**：asInvoker → USN 恒 Win32 5 → walk 是唯一索引源 → 每次启动 5s 载入 + 每次空闲 15min 一次 45s 全盘普通优先级 IO + 200MB 缓存重写——"开着重启都卡、放着不动也卡"。③**一次性放大器**：folder:+ext: 组合过滤恒 0 结果（B-9）诱使用户点"重建"→45s walk→成果又不落盘（B-1）→ 下次启动回到旧索引再来一遍。④键入搜索本身 189-233ms（真实日志），**不是**主因。⑤内存 ~900MB@3.42M 项（查找表 2.15 倍开销，r6 方案未实施）加剧换页。

---

## 1. 发现清单

### B-1【P0，Confirmed】force-rebuild 的 walk 索引不落盘；WalkBuild 内部 Save 恒 no-op
- **位置**：`MiniSys/src/core/VolumeIndex.cpp:270-276`（EnsureBuilt forceFullNext_ 分支 `return BuildFull(...)` 提前返回，跳过 :311-312 的 `if (ok) SaveCacheIfWorthwhile()`）；`:677-679`（WalkBuild 成功后调 SaveCacheIfWorthwhile）；`:1274`（SaveCacheIfWorthwhile 首行 `if (!valid_.load()) return;`）。
- **机制**：BuildFull 在 :509 才置 `valid_=true`，而 WalkBuild 在其**内部**执行，此刻 valid_ 还是 false → WalkBuild 里的 Save 恒 no-op。非 force 的常规路径（无缓存/serial 变）由 :311-312 兜底补存；**force 路径（Invalidate→"重建索引"按钮）谁都不存**。
- **证据**：用户 `index-cache-C.bin` = 267,743,481 B，mtime **2026-10-14 02:12**；10-29 01:34:06-01:34:55 完整 BuildFull→walk→"indexed 3427118 entries"链（日志 1354-1363 行），缓存未更新。触发动作（用户在 01:33:58 搜索 0 结果 8 秒后点重建）为推断，但**到达 BuildFull 的代码路径唯一**（缓存有效、serial 未变，唯 `forceFullNext_` 可致）。
- **后果**：每次重建 45-50s 白费；重启回到 10-14 旧索引（缺 15 天变更）；与 B-9 叠加成"重建→没效果→再重建"循环。
- **修复**：force 分支改 `bool ok = BuildFull(...); if (ok) SaveCacheIfWorthwhile(); return ok;`（2 行）。USN-built 重建同样受益。

### B-2【P0，Confirmed】journal cursor 恒 0：USN 增量对 walk 索引永久短路；RefreshWalk 无任何 IO 礼让
- **位置**：`VolumeIndex.cpp:699-701`（`RefreshFromUsn` 首行 `if (walkBuilt_) return true;`）；`:534-535`（WalkBuild `journalId_=0; nextUsn_=0`，随缓存持久化——缓存头实测 `journal=0 walk=1`）；`SessionService.cpp:458-469`（IdleMaintenanceAsync 对 walkBuilt 卷走 RefreshWalk）；`VolumeIndex.cpp:322-325`（THREAD_MODE_BACKGROUND_BEGIN 只作用于 BuildFull 主线程）与 `:685-697`（**RefreshWalk 直接调 WalkBuild，主线程也不 background；WalkBuild 的 6 个池线程 :566-662 全程普通优先级**）。
- **后果**：①任何索引"刷新"退化为 45-50s 全盘 walk（10-29 实测 01:34:06.3→01:34:51.2）；②空闲 15min 冷却（MainWindow.cpp:1554-1557）一到即 6 线程普通优先级捶 98% 满的 C:，叠加 ~200MB 缓存重写（B-3）；③即使用户某天提权运行，walk 索引也**永远不会升级**回 USN 增量（RefreshFromUsn 先短路，BuildFull 只在 valid_=false 时才试）。
- **环境 vs 应用**：非提权开不了卷句柄（Win32 5）= asInvoker by design；但**无升级探测、无 IO_PRIORITY_HINT_LOW、无变更检测跳过**是应用缺陷。
- **修复草案**：a) WalkBuild 池线程 + RefreshWalk 主线程设 `THREAD_MODE_BACKGROUND_BEGIN`（或 VolumeIndex 建卷句柄时用 FILE_FLAG_BACKUP_SEMANTICS 无关，直接 NtSetInformationThread 降 IO 优先级亦可）；b) RefreshWalk 前抽样（根目录 mtime + 缓存条目数 + 2-3 个深目录 mtime）未变则跳过；c) EnsureBuilt 每 24h 一次真实探测卷句柄，提权环境下切回 USN。

### B-3【P1，Confirmed】idle RefreshWalk 每次无条件重写 ~200MB 缓存（r6 B-1 修复只覆盖 journal 路径）
- **位置**：`VolumeIndex.cpp:677-679`（`if (walkBuilt_) SaveCacheIfWorthwhile();` 无脏检查）+ `:1305-1316`（逐行 fwrite）。r6 加的 `dirtySinceSave_` 只在 `ApplyChanges`（journal delta，:826）置位——本机走不到。
- **量化**：3,087,989 行逐行写实测 1042ms / 3,427,118 行 880ms（块写 250/284ms，3.7×）；10-14 02:12 的缓存 mtime 即 idle RefreshWalk 所写。
- **修复**：walk 后比对 nodes_.size + serial + 抽样 mtime，未变跳过；至少改块写（一次性 fwrite 行块）。

### B-4【P1，Confirmed（复测）】冷启动 TryLoadCache+Finalize ≈5.0s（r6 B-4 未修）
- **实测**（真实缓存只读，`.tmp-review/r7/03-exp/cache_load_bench.exe`）：行读 344.5ms + names 73.3ms + bounds 10.5ms + reserve 63.8ms + byFrn 782.0ms + childrenOf 298.5ms + **DFS dirPaths 3397.7ms** = **4960ms**；工作集 4→808MB。Finalize 代码与 r6 基线相同（全量 DFS、dirPaths_ 完整小写路径键、childrenOf reserve(N) 负载因子 0.09）。
- **生产佐证**：01:29:22.291 starting → 01:29:27.772 "loaded cache"（5.5s，含 UI 启动）。
- **修复**（同 r6）：行块读；dirPaths_/DFS 惰性化（Search 只依赖 byFrn_+爬链）；childrenOf→CSR。3.42M 规模下现状只会更长。

### B-5【P1，Confirmed】维护被抢占取消后：索引失效 + 查找表悬垂 + 搜索持续空结果
- **位置**：`VolumeIndex.cpp:665-673`（WalkBuild cancelled → `valid_=false; entryCount_=0`，但 **`ready_` 仍 true、byFrn_/dirPaths_/frnToDirPath_ 保留旧树、nodes_ 为半棵新树**——表与数据完全错位）；`SessionService.cpp:625`（SearchAsync 经 `ValidVolumes()` 过滤 valid_，取消后该卷被跳过 → 搜索 0 结果）；SearchAsync **不触发** EnsureBuilt/TryLoadCache 自愈——用户停在搜索页继续键入只会得到"匹配 0 项（索引共 0 项）"，直到切页（OnTabChanged 触发 BuildIndexAsync，MainWindow.cpp:1652-1659）、垃圾扫描或手动重建（后者 45s）。
- **次要**：RefreshWalk 成功/取消路径在取消点之后仍要 Finalize（3.4s）+ Save（~1s），期间无 cancel 检查 → 抢占收敛最坏 ~5s，而非"秒级"。
- **修复**：取消路径补 `ready_=false` 并清查找表（对齐 r6 B-11 的雷区清理）；SearchAsync 发现 valid 卷为空且磁盘有缓存时先 TryLoadCache（5s）或直接发起 EnsureBuilt 任务并提示。

### B-6【P1，Confirmed】v2.23 惰性展开每次展开=子树全深度、不可取消、无缓存；v2.18 depth=2 双倍全盘扫描未动
- **位置**：`SessionService.cpp:531-582`（FolderChildrenAsync 对每个子目录 `DirectorySizeParallel(kid, 4)`——全深度）；`PathUtils.cpp:212-261`（SizePool Worker **无 cancel 检查**——r6 B-5 的取消半体未修，单个大子树（C:\Windows、node_modules）内不可取消）；`FolderTreeScanner.cpp:76`（顶层全深度）+ `:101-148`（depth=2 第二层再全深度 ≈2× 全盘 IO；v2.23 未改此文件，git log 确认最后改动是 v2.18 `0a8783c`）。
- **后果**：展开一个大节点 = 又一次子树级"全盘"（分钟级可达），期间 FolderExpand 占用唯一 worker，搜索/扫描全被拒；DirSizeCache 不参与（FolderChildrenAsync 直调 DirectorySizeParallel，绕过 (path,mtime) 缓存）。
- **修复**：展开复用 DirSizeCache；DirectorySizeParallel 增加 cancel 参数（每 64 项检查）；线程 BELOW_NORMAL；中期：从 VolumeIndex 树聚合（索引无 size，但目录 mtime 可做变更检测）。

### B-7【P2，Confirmed】Executing 类零取消 + Shutdown 无界 join（r6 B-10 未修）
- `SessionService.cpp:802-883`（RunPlan 循环不查 `cancelScan_`；DelegateOp 上限 30min 内不可打断）；`:919-977`（RunEmptyQuarantine 逐盘 `fs::remove_all`——今日 2.5GB/25 项量级为秒-分钟级，无取消）；`:1011-1054`（UndoRecordsAsync 无取消）；`:1188-1191`（Shutdown 无条件 join——关窗/takeover 要等当前项走完；main.cpp:24-44 takeover 仅 10s 等待后放弃，旧实例可能继续驻留捶盘）。
- **修复**：RunPlan 每 16 项查 cancel；Shutdown join 加上限（如 3s）后 detach 缓存写线程。

### B-8【P2，Confirmed】DelegateOp reader 线程 detach 分支 UAF（r6 B-9 未修）
- `DelegateOp.cpp:120-164`：reader lambda 引用捕获 `output`/`outMu`（Execute 栈对象）；:160 `reader.detach()` 后主线程 :163-164 `CloseHandle(outRead)/CloseHandle(readDone)` 并返回——分离线程随后 `lock_guard(outMu)` 写已析构对象 = UAF，`SetEvent(readDone)` 作用于已关句柄。触发罕见（CancelIoEx 后 2s 未收尾）但模式错误。修复：shared_ptr 持有输出态+事件，或去掉 detach。
- 其余子进程机制核过无问题：job 对象 KILL_ON_JOB_CLOSE、30min 有界轮询、管道截尾 64KB、`CREATE_NO_WINDOW`、takeover 有界等待。**全仓 CreateProcess 仅此一处**（另 MainWindow.cpp:2319 ShellExecuteW 打开资源管理器，非子进程封装）。

### B-9【P2，Confirmed】"folder:"+"ext:" 组合过滤恒 0 结果——诱发用户点"重建"（45s 白费的直接导火索）
- `VolumeIndex.cpp:1074-1075`（`filter.dirsOnly && !n.isDir → continue`）与 `:1076-1078`（`!filter.exts.empty() && (n.isDir || !extMatches) → continue`）——**dirsOnly 与 exts 互斥**：目录无扩展名、文件被 dirsOnly 排除，组合查询恒空。
- **实证**：01:33:58 `'ext:pdb folder:debug'` hits=0（旧索引 3.09M）；01:34:56 同查询在**新 3.42M 索引**上仍 hits=0；8 秒后（01:34:06）即发生 BuildFull——用户"搜不到→重建"的行为链完整落日志。
- **修复**：`folder:` 语义改为路径包含匹配（matchPath 变体），不再与 ext: 互斥。（功能面归 07/08，此处只收后端代价。）

### B-10【P2，Confirmed】StartScan 被维护抢占后"稍候自动开始扫描"不会发生
- `SessionService.cpp:361-369`（busy+maintenanceActive → cancel + 提示"稍候自动开始扫描…"）→ `MainWindow.cpp:1754` `if (!svc.StartScan(...)) return;` ——**没有安排任何重试**。对比 SearchAsync 失败有 80ms TIMER_SEARCH_RETRY（:1968）。用户等一个不会来的扫描。修复：失败时 SetTimer(500ms) 重试至成功或 10s 超时。
- 附带：空闲倒计时归零触发的是 `OnScan()` 全量重扫当前 tab（MainWindow.cpp:956-981）——tab=FolderTree 时即 depth=2 双倍全盘遍历；执行后用户回前台不会自动取消（扫描可手动点取消按钮取消，弱于维护的 WM_ACTIVATE 自动取消）。

### B-11【P3，Confirmed】ghost 清理 O(N×M) + 每比较现造小写串（r6 B-13 未修）
- `SessionService.cpp:503-506` 与 `:1112-1114`：`std::find(dead.begin(), dead.end(), ToLower(it.path.wstring()))`——vector 线性查找 + 每次重造串。修复：`unordered_set<std::wstring>`。

### B-12【P3，Confirmed】缓存落盘 remove+rename 非原子窗口（r6 B-12 未修）
- `VolumeIndex.cpp:1319-1322`：`remove(final)` 后 `rename(tmp,final)`，中间崩溃=无缓存（下次 45s walk 重建）。同仓 `SaveResultsCache` 已用 `MoveFileExW(REPLACE_EXISTING)`（SessionService.cpp:212），照抄即可。

### B-13【P3，Likely】DelegateOp 输出按 UTF-8 解码，实为 OEM 代码页
- `DelegateOp.cpp:126`：`Utf8ToWide(std::string(pipeBuf, n))`——DISM/powercfg 在中文系统输出 cp936，非 ASCII 内容（中文错误信息）解码为乱码进入 note/错误弹窗。机制确凿（代码即证），DISM 实际输出未取验证 → Likely。修复：`MultiByteToWideChar(GetOEMCP(),…)`。

### B-14【P3，Confirmed】ResetForTesting 仍不清 frnToDirPath_；filePaths_ 死字段
- `VolumeIndex.cpp:149-164`：清 dirPaths_ 但不清 `frnToDirPath_`（其值是指向 dirPaths_ 键的指针——clear 后悬垂）；`ready_` 已清（r6 B-11 半修）。`filePaths_`（VolumeIndex.h:219）声明后全仓无构建无清理——死代码，建议删除。今日无害（单 worker 串行 + Finalize 开头 clear），是并发化重构（S3/S4）的雷。

---

## 2. r6 已修项复核（勿重复报的部分，逐条核码）

| r6 项 | 现状 | 依据 |
|---|---|---|
| B-1 缓存脏标志 | ✅ 半修：`dirtySinceSave_` 在 EnsureBuilt journal 路径生效（:299-302）；**walk 路径每次仍重写**（本报告 B-3）、force 路径完全不写（B-1） | 读码 + mtime |
| B-2 Logger UTF-8 | ✅ 修复存活：ofstream+WideToUtf8+5MB 轮转（Logger.cpp:14-52）；10-29 日志中文/破折号全部完整落盘 | 读码 + 日志实证 |
| B-3 空闲维护抢占 | ✅ 半修：`maintenanceActive_`（SessionService.h:205）+ SearchAsync/StartScan busy 时主动 cancel（:365/:591）+ WM_ACTIVATE（MainWindow.cpp:992）；RunSearch 本身仍只取消 IsSearching（:1962-1963）；新缺口 = B-5（取消后索引失效）与 B-10（无重试） | 读码 |
| B-4 冷启动 Finalize | ❌ 未修 | 实测 5.0s（本报告 B-4） |
| B-5 FolderTree 双倍遍历/SizePool 取消 | ❌ 未修；v2.23 的 FolderChildrenAsync 又加一份同构（B-6） | 读码 + git log |
| B-6 每键 3 次全量拷贝 | ✅ 修复存活：`hitCount` 先捕获再 move（SessionService.cpp:667-671） | 读码 |
| B-7 逐行写缓存 | ❌ 未修（实测 880-1042ms，B-3） | 实验 |
| B-8 逐工作项 spawn 池 | ❌ 未修（JunkScanner.cpp:276-311 的 8×2 线程、DevBuildCache.cpp:301-341 同构） | 读码 |
| B-9 DelegateOp detach UAF | ❌ 未修（B-8 本报告） | 读码 |
| B-10 Executing 取消/有界 Shutdown | ❌ 未修（B-7 本报告） | 读码 |
| B-11 ResetForTesting | ◐ ready_ 已清；frnToDirPath_ 仍不清（B-14） | 读码 |
| B-12 缓存非原子 rename | ❌ 未修（B-12 本报告） | 读码 |
| B-13 ghost O(N×M) | ❌ 未修（B-11 本报告） | 读码 |
| B-14 probe UI 直读索引 | 未复核（-probe 自测路径，本轮未验证） | — |

---

## 3. 量化实验（`.tmp-review/r7/03-exp/`，build.cmd 可重跑）

### 3.1 cache_load_bench —— 真实 index-cache-C.bin 只读分段（HEAD TryLoadCache+Finalize 复刻）

```
header: version=2 drive=C serial=b22dc436 journal=0 walk=1 nodes=3087989 nameBytes=137.5MB
rows fread 344.5ms | names 73.3ms | bounds 10.5ms (bad=0)
reserve 63.8ms | byFrn 782.0ms (3,087,989) | childrenOf 298.5ms (387,047 keys)
DFS dirPaths 3397.7ms (444,864 dirs)
TOTAL ≈ 4960ms；WorkingSet 4 → 808MB (+804MB)
```
与 r6 实测同构（4.1s→5.0s，DFS 恒主导）；3.42M 规模（今日 walk）下线性外推 ≈5.5s / ~890MB。

### 3.2 cache_write_bench —— SaveCacheIfWorthwhile 逐行写 vs 块写（临时文件，即删）

```
3,087,989 行：逐行 fwrite 1042.1ms（188MB）  ←→ 块写 249.6ms（4.2×）
3,427,118 行：逐行 fwrite  879.9ms（209MB）  ←→ 块写 283.7ms
```

### 3.3 真实环境计时（生产日志 10-29，非合成）

| 操作 | 耗时 | 出处 |
|---|---|---|
| 启动→缓存可用 | 5.48s（22.291→27.772） | 日志 1334/1337 |
| walk 全盘（BuildFull 回退） | 45.1s（01:34:06.3→51.2，6 线程，3,427,118 项） | 日志 1354-1362 |
| Finalize（walk 后） | 4.7s（51.162→55.833 "indexed"） | 日志 1362-1363 |
| 搜索 'ext:pdb'（1000 hits，3.09M/3.42M 索引） | 189ms / 233ms（含 1000 行取大小） | 日志 1340-1343/1374-1376 |
| 搜索 'ext:pdb folder:debug' | 96-100ms，恒 0 hits（B-9） | 日志 1351-1353 |
| LargeFiles 扫描（tab=2，FastWalk 全盘） | 9.4s / 19.1s / 24.2s / 24.9s | 日志 1344-1348/1377 |
| FolderTree 扫描（tab=4） | 16.7s | 日志 1379 |
| Junk 扫描（tab=0，索引加速） | 3.0s（DevBuildCache discovery 6528 dirs 0 hits + 96 items） | 日志 1381-1382 |
| 隔离 25 项 2.5GB（同卷 rename） | <1s（01:58:04-05 连续 25 条），1 项 FAILED Win32 5（GPUCache 被占用，环境问题） | 用户线索 |
| history.jsonl | 39,450 B ≈330 条（LoadAll 全量重读但小，无碍） | 只读 |
| results-*.json | Search 428KB/LargeFiles 76KB/Junk 48KB（1.5s 节流全量 JSON 写，可忽略） | 只读 |

**用户线索修正**：`index-cache-C.bin` 并非"数 GB 级"——实测 267,743,481 B（256MB，MiB 255.5），且 mtime 停在 10-14（这正是 B-1 的证据而非反证）。

---

## 4. 调度与降级方案：r6 S0-S5 现状核对 + 最小改动路径

**现状**：单 worker（`std::thread worker_`，StartTask 以 taskKind_ CAS 互斥）+ **无队列**——busy 即拒绝，靠 UI 定时器重试。交互（Search/Verify/Preview/FolderExpand）与重活（Indexing/Executing/maintenance）同抢唯一线程。cancelScan_ 覆盖：扫描器、walk、搜索 sink/取大小、索引枚举循环；**不覆盖**：SizePool、RunPlan、Undo、EmptyQuarantine、DelegateOp、Finalize/Save 尾段。任务异常已兜底（StartTask 捕获 + 无条件 DONE post + taskKind 复位——04-5 修复存活，无"异常后永久卡死"）。

| r6 Step | 现状 | 判定 |
|---|---|---|
| S0 维护可抢占 | maintenanceActive_ 机制落地 | ✅ |
| S1 RunSearch 取消 Verifying | 变形实现（服务端 busy 时主动 cancel）；新缺口 B-5/B-10 | ◐ |
| S2 SizePool/RunPlan 取消检查点 | 未做 | ❌ |
| S3 交互/维护双通道 | 未做 | ❌ |
| S4 常驻池统一 | 未做（仍逐工作项 spawn，8 处） | ❌ |
| S5 缓存写尾部+dirty+块写+有界 Shutdown | dirty 仅 journal 路径 | ◐ |

**本轮最小改动路径**（在 S2/S3 之前先做的三件 P0，均 ≤10 行级）：
1. **B-1 落盘 2 行**——重建成果存活，直接消掉"重建→白费→再重建"循环；
2. **B-2 礼让 + 变更检测**——WalkBuild 池线程 THREAD_MODE_BACKGROUND + RefreshWalk 前抽样 mtime 跳过，消掉"放着不动也卡"；
3. **B-5 取消自愈**——取消路径 ready_=false + 清表，SearchAsync 空卷时先 TryLoadCache，消掉"回来第一次搜索空结果"。
之后按 r6 S2（SizePool/RunPlan 检查点）→ S3（双通道）推进；S3 前必须先清 B-14 雷区（r6 同判）。

---

## 5. 修复顺序建议

| 级 | 项 |
|---|---|
| **P0** | B-1 force 路径落盘（2 行）；B-2 walk 线程 IO 礼让 + RefreshWalk 变更检测跳过；B-5 取消路径清表 + 搜索空卷自愈；B-9 folder:/ext: 互斥（功能性，却是 45s walk 的直接导火索） |
| **P1** | B-3 walk 缓存脏检查 + 块写（含 B-7 r6 的逐行写）；B-4 Finalize 惰性化/块读；B-6 惰性展开复用 DirSizeCache + SizePool 取消；B-10 StartScan 重试 |
| **P2** | B-7 Executing 取消检查点 + Shutdown 有界 join；B-8 DelegateOp reader 生命周期；r6 §4 内存压缩（CSR 等，3.42M 下 ~900MB）；S3 双通道 |
| **P3** | B-11 ghost 清理 set 化；B-12 MoveFileExW；B-13 OEM 解码；B-14 清理悬垂表 + 删 filePaths_ 死字段 |

---

## 6. 跨报告线索

- **给 02（UI 延迟）**：①"键入→结果"后端仅 189-233ms，键入链路卡顿主体在 UI 侧渲染/快照（本报告实测供基线）；②taskMode_ 在维护/执行期间为 None 或 Executing——动作矩阵"解锁但 worker 忙"的语义裂缝仍是 B-10 类提示语撒谎的共因；③启动 5.5s 的构成（TryLoadCache 5.0s）归后端，勿再计入 UI 首帧。
- **给 04（稳定性）**：B-8（detach UAF）、B-5（查找表悬垂——今日被单 worker 串行掩盖）、B-14（ResetForTesting 悬垂 + 死字段）；Shutdown 无界 join 与 takeover 10s 放弃的组合可能出现双实例并发捶盘（旧实例未死、新实例已起——互斥量兜底但 takeover 超时路径直接 return 0，需确认）。
- **给 05（安全）**：无新发现；缓存 v2 无 MAC 决策与本轮无冲突；B-12 属可靠性。
- **给 07/08（UX/功能）**：B-9（folder:+ext: 恒 0）+"重建索引"按钮无进度预期管理（45s 无 determinate 进度）+ 重建后无"已更新缓存"反馈——三者合成用户"功能坏了"的感知；"稍候自动开始扫描"承诺（B-10）。
- **给 06（架构）**：单 worker God-object 调度与 VolumeIndex 双 God object 的耦合是 S3 双通道的最大阻力；逐工作项 spawn 池模式仍 8 处。

---

## 7. 被推翻的假设 / 已验证无问题

**被推翻 / 修正的假设**：
1. **用户线索"index-cache-C.bin 数 GB 级"不实**——实测 256MB；且其 mtime 停在 10-14 恰是 B-1（不落盘）的证据。
2. **线索假设"journal cursor 恒为 0 → USN 增量永远不可用"**：成立（RefreshFromUsn :700 首行短路），但**补充证伪了它的另一半隐含假设**——"任何索引刷新都退化为 walk"中的"刷新"只有 idle RefreshWalk 与手动重建两条路；日常 EnsureBuilt（垃圾扫描/进页）对 walk 缓存是**零成本短路**（不触发 walk）。真正的周期性代价是 idle maintenance 每 15min 一次。
3. **"01:34:06 的 BuildFull 来自空闲维护"**（我自己的初始假设）——被代码排除：IdleMaintenanceAsync 对 invalid 卷 `continue`（SessionService.cpp:457）、对 walk 卷走 RefreshWalk（不打 "cannot open volume"）；在缓存有效且 serial 未变前提下，BuildFull 唯一入口是 Invalidate（重建按钮）。触发动作是推断（Likely），路径唯一性是代码级（Confirmed）。
4. **"tab=4 的 16.7s/7 items 是 depth=2 双倍遍历"**——7 items 与 depth=2 预期产出不符，更像 focusRoot 下钻扫描（Presenters.cpp:653-657）或用户中断；depth=2 双倍遍历的判定不依赖该样本（代码即证）。

**已验证无问题**（逐一读到代码/实测）：
- StartTask 的 CAS+taskGen+异常兜底：DONE 永不缺失，taskKind_ 必复位，无永久卡死；join 旧 worker 先于新 spawn。
- SearchAsync 取大小池的写并发与取消（v2.19 语义保留）；`static lastSave` 节流的 happens-before（单 worker 串行）。
- WalkBuild 的 condvar 终止条件与取消传播（每目录弹出前检查）；v2.17b parentFrn 修复存活。
- FastWalk 池取消/终止（inflight 原子 + 每文件检查）。
- TryLoadCache 边界校验（实测 bad=0）、Finalize 环预算（2N+16）、kMaxIncrementalChanges=20000、SaveResultsCache 的 tmp+MoveFileEx 原子。
- VolumeIndex 跨线程契约现状：UI 仅触原子量与 RegistryMu_ 聚合，容器访问全在单 worker——今日无竞争（B-5/B-14 是取消/重构路径的雷，非今日竞争）。
- OperationLog：Append 逐条 + HMAC、UpdateStatusBulk 全量原子重写、Generation 缓存（QuarantineUsageText）——330 条/39KB 规模无性能问题；T-B2/T-B4 修复存活。
- 隔离 25 项 2.5GB <1s：同卷 MoveFileExW rename 语义，符合设计；GPUCache Win32 5 为文件占用（环境），QuarantineOp 错误带 Win32 号。
- Logger B-2 修复存活（10-29 中文日志完整 + 轮转逻辑在位）。
- DelegateOp 除 B-8/B-13 外：job 对象、超时、输出上限、无窗口标志全部在位；全仓 CreateProcess 仅 1 处。

**未验证 / 局限**：
- 未运行 MiniSys 本体（用户禁令）；所有复刻实验为进程外等价代码，Finalize/写盘存在负载方差。
- EmptyQuarantine remove_all 的 2.5GB 实际删除时长未实测（不删真实数据；合成 2.5GB 会捶 98% 满的 C:，放弃）——量级按逐文件元数据操作推算为秒-分钟级。
- FolderTree 惰性展开端到端耗时未实测（需 GUI）；B-6 的"分钟级"为机制推算（子树全深度 × 不可取消）。
- "用户点了重建索引按钮"（B-1 触发动作）为行为推断，无直接日志（按钮点击不打日志）。
- B-13（OEM 乱码）未实测 DISM 真实输出；r6 B-14（-probe UI 直读索引）未复核。
- takeover 超时放弃后旧实例滞留的双实例风险只读了 main.cpp 路径，未构造实验。

## 附：实验物料
`.tmp-review/r7/03-exp/`：`build.cmd`（vcvars64+cl /O2，产物仅在本目录）、`cache_load_bench.cpp/.exe`（真实缓存只读 `_wfsopen("rb")`）、`cache_write_bench.cpp/.exe`（写本目录临时文件即删）。
