# MiniSys 第六轮评审 r6 · 03 后台线程 / 索引 / 缓存 / 扫描层

> 日期 2026-10-14 · 对象 HEAD `2a9d653`（v2.19a，与运行中 pid 19128 的 02:12 构建同源）· 方法：全量读码（SessionService / VolumeIndex / FastWalk / PathUtils / DirSizeCache / FolderTreeScanner / JunkScanner / DevBuildCache / DelegateOp / main）+ 真实日志（949 行原文件 od 级取证）+ 三项量化实验（`​.tmp-review/r6/03-exp/`，MSVC /O2 x64 自建）+ 对 267MB 真实缓存的**只读**分段测量。
> 规模基线：C: 单卷 3,087,989 项（缓存头实测），缓存 267,743,481 B，搜索 1000 行/键，16 逻辑核，C: 已用 98%（剩 8.35 GB）。

---

## 0. 摘要：Top 风险与整体卡顿的后端归因

| # | 结论 | 证据强度 |
|---|---|---|
| 1 | **B-2 Logger 在首个非 ASCII 字符处永久失效**：`std::wofstream` 无 locale，消息里的 "—"（U+2014，11 处格式串在用）使流进入 fail+bad 且**永不清除**——该行截断、本进程**后续所有日志静默丢失**。实验复现 + 生产日志 od 取证（每个含 "—" 的行是本进程最后一行）。诊断基建塌了，后续一切"日志无证据"的结论都要重新审视 | Confirmed（实验） |
| 2 | **B-1 walk 缓存下 EnsureBuilt 无条件重写 267MB**：`RefreshFromUsn` 对 walkBuilt 一律 `return true`，`EnsureBuilt` 随即 `SaveCacheIfWorthwhile()` 全量重写——**零变更也写**。每次垃圾扫描、每次空闲维护都触发；实测行写 CPU 0.9s + 267MB 盘写，落在单 worker 上，且 C: 仅剩 8GB | Confirmed（代码 + mtime 02:12 佐证） |
| 3 | **B-3 空闲维护队头阻塞且无取消**：idle maintenance（Verifying，walk 卷=分钟级 re-walk）期间用户键入搜索，`RunSearch` 只取消 `IsSearching`（MainWindow.cpp:1710），80ms 重试空转直到维护跑完；扫描按钮只弹提示不取消（:1491）。唯一取消点是 WM_ACTIVATE（:760），窗口始终前台时永不触发 | Confirmed（代码链） |
| 4 | **B-4 冷启动首搜 ≈4.1s**：TryLoadCache 逐行 fread 332ms + names 51ms + bounds 11ms + **Finalize 3.72s**（DFS 2.68s 主导），全在单 worker | Confirmed（真实缓存实测） |
| 5 | **B-5 FolderTree v2.18 双倍全盘遍历**：顶层全深度定大小 + 一级子目录再全深度定大小 ≈ 2× 全盘 I/O，逐目录 spawn 2 线程池、串行推进，`SizePool` **无取消检查**（最大子树内不可取消） | Confirmed（代码；耗时为解析推算） |
| 6 | **B-6 每键 ~7ms 纯浪费**：SearchAsync 内 3 次全量 `Results()` 拷贝（诊断日志遗留，实测 3.2ms）+ 排序比较器内 `ToLower`（实测 4.0ms vs 预计算 0.84ms） | Confirmed（实测） |
| 7 | 905MB 工作集构成已实测复现：数据 255MB + **查找表 549MB**（2.15 倍开销），压缩路线见 §4 | Confirmed（复现 808MB） |

**对"整体卡顿"的后端归因**（按证据强度）：主因不是 v2.19 的并行取大小（实测净收益 3.9×，开销 2.5%，见 §3.1），而是 ①单 worker 上不可抢占的分钟级任务（idle re-walk / 索引构建 / FolderTree）叠加 ②无取消路径（B-3/B-5/B-10）与 ③冷启动 4.1s 载入（B-4）。UI 侧渲染问题归 02/04 报告。

---

## 1. 发现清单

### B-1【P0，Confirmed】walk 缓存下 EnsureBuilt 零变更重写 267MB
- **位置**：`MiniSys/src/core/VolumeIndex.cpp:283-297`（EnsureBuilt 成功路径调 SaveCacheIfWorthwhile）+ `:690`（`RefreshFromUsn` 首行 `if (walkBuilt_) return true;`）+ `:668`（WalkBuild 结束必存）。
- **触发**：缓存 walk=1（我的实测头：`walk=1 nodes=3087989`）。JunkScanner.cpp:231 每次垃圾扫描 `EnsureBuilt`；空闲维护 walk 卷走 `RefreshWalk→WalkBuild→SaveCacheIfWorthwhile`（同样无脏检查）。
- **后果**：每次触发 = 单 worker 阻塞 ~1-2s（0.9s 行写 CPU + fclose 落盘 267MB）+ 磁盘磨损；98% 满的 C: 上 267MB 反复改写（虽不致满，但放大碎片）。实测佐证：`index-cache-C.bin` mtime 02:12 == 会话边界（上一实例空闲维护收尾 / 新实例 EnsureBuilt），且该会话日志恰因 B-2 全灭——无法进一步分辨，本身就是 B-2 的恶果。
- **修复草案**：加 `dirty_` 标志，仅在 `ApplyChanges`/`BuildFull`/`WalkBuild` 实际变更后置位；`EnsureBuilt` 成功路径仅 `if (dirty_) Save()`。另：空闲 `RefreshWalk` 前可先取根序列号+条目数粗比对比，未变则跳过 re-walk。

### B-2【P0，Confirmed（实验+生产取证）】Logger 首个非 ASCII 字符后永久哑火
- **位置**：`MiniSys/src/util/Logger.cpp:43`（`std::wofstream` 默认 locale 打开）+ `:70-72`（`<<` 后从不检查/清除流状态）。
- **机制**（`03-exp/log_bench.exe` 复现）：MSVC 默认 codecvt 遇 U+2014 转换失败 → 流置 fail+bad → **该行从 "—" 起丢失（含换行）**，此后本进程所有 `stream <<` 均为 no-op（`fail=1` 永续）；下一进程的记录直接拼在残行后。生产日志 949 行中所有含 "—" 的行（`loaded cache for C:`、`cache write failed`、`USN enumeration truncated`、`READ_USN_JOURNAL failed` 等 20+ 处）全部截断，且**均为该会话最后一条记录**。
- **后果**：db3c5da 加的整条搜索链路诊断（`SearchAsync enter` / `Search worker body done` / walk 全部日志）在现场**从未落地**；`OnScanDone` 日志 `q='%s'` 遇中文查询同样致死。本轮多项"日志无证据"均由此解释。
- **修复草案**（一行级）：改 `std::ofstream` + `WideCharToMultiByte(CP_UTF8)` 落盘（或 `imbue(std::locale(locale(), new std::codecvt_utf8_utf16<wchar_t>))`）；顺带把 11 处 "—" 格式串统一为 "-"。**注意**：02 报告的 UI stall watchdog 若复用此 Logger，中文进度文本会让它哑火——必须独立写盘。

### B-3【P0，Confirmed】空闲维护队头阻塞 + 无取消（Verifying 不可抢占）
- **位置**：`MainWindow.cpp:1710-1716`（RunSearch 仅 `if (svc.IsSearching()) svc.CancelScan();`）；`:705/709`（debounce/retry 在 `taskMode_==None` 时执行——idle maintenance 从不 SetTaskBusy，taskMode_ 恒 None）；`:1491`（OnScan 对 Verifying 仅提示）；`:760`（仅 WM_ACTIVATE 取消 Verifying）。
- **触发/后果**：用户离开 ≥5min 且活动页非搜索 → maintenance 启动（walk 卷=分钟级）；用户回前台**不切换窗口**直接键入 → 200ms debounce → RunSearch（不取消）→ SearchAsync busy → 80ms 重试空转至维护结束（walk 全程）。点扫描同理只弹"请稍候"。
- **修复草案**：RunSearch 改为 `if (svc.IsSearching() || svc.IsVerifying()) svc.CancelScan();`（与 WM_ACTIVATE 语义对齐，1 行）；中期见 §5 调度重构。

### B-4【P1，Confirmed（实测）】缓存载入 4.1s：逐行 fread + Finalize DFS 主导
- **位置**：`VolumeIndex.cpp:1210-1220`（3.09M 次 40B fread）；`:186-264`（Finalize：byFrn 761ms + childrenOf 285ms + DFS 2,676ms，实测）。
- **实测**（真实缓存只读，§3.2）：行读 332ms（块读+解码 71ms，4.7×）；names 51ms；bounds 11ms；Finalize 3,722ms。合计 ≈4.1s 单 worker——首次进搜索页的"卡"。生产对照：提升会话日志 `enum done→indexed` Δ1.2-1.8s（USN 3.03M 节点 Finalize），与本测量同量级。
- **修复草案**：①行块合并 fread；②`dirPaths_`/DFS 惰性化——Search 只依赖 `byFrn_`+`OriginalCasePathOf`（已按 FRN 爬链），`HasDir/CollectChildren` 首用时再建；③childrenOf 用 CSR 数组（§4）。三项合计可把载入压到 ~0.5s。

### B-5【P1，Confirmed（代码）】FolderTree v2.18 双倍全盘遍历 + SizePool 不可取消
- **位置**：`FolderTreeScanner.cpp:76`（顶层每目录 `DirectorySizeParallel(dirPath, 2)`——**全深度**，参数 2=线程数）+ `:101-148`（level 循环对每个一级子目录**再次全深度**）+ `PathUtils.cpp:274-283`（每次调用新建 2 线程 SizePool 再 join）+ `SizePool::Worker`（**无 cancel 检查**）。
- **后果**：顶层一遍 = 全盘，一级子目录一遍 ≈ 全盘 → I/O ≈2×（depth=2 时；depth 每 +1 再 +1×）。旧版单层实测 75-178s（日志 tab=4），v2.18 解析推算 ~150-300s（未实测，避免捶打 98% 满的盘）。取消延迟 = 当前最大子树（如 C:\Windows）。逐目录 spawn 2 线程：顶层 ~30 次 + 一级数百次 spawn/join。
- **修复草案**：一次 BFS 分层遍历按层输出（子目录大小自底向上聚合，全盘只读一遍）；复用常驻池；SizePool 每 64 项查一次 cancel。

### B-6【P2，Confirmed（实测）】SearchAsync 每键 ~7ms 冗余
- `SessionService.cpp:589-595`：SetProgress 与两条 MS_LOG 里 3 次 `Results(TabId::Search).size()` —— 每次锁内全量拷贝 1000 行（实测单拷贝 1.08ms ×3 ≈3.2ms/键，db3c5da 诊断遗留，v2.19 仍在）。
- `VolumeIndex.cpp:1114-1116`：`std::sort` 比较器内 `ToLower(a.name)<ToLower(b.name)`——1000 名实测 4.79ms vs 预计算键 0.84ms（5.7×）。
- 修复：worker 内先记 `size_t hits = items.size()` 复用；RawHit 预存 lowered name。两者合计每键省 ~7ms（对比 §3.1 整个取大小阶段才 10ms）。

### B-7【P2，Confirmed（实测）】SaveCacheIfWorthwhile 逐行 fwrite
- `VolumeIndex.cpp:1294-1303`：3.09M 次 40B fwrite 实测 894ms（块写 413ms，含解码缓冲）。叠加 B-1 的"零变更也写"。修复：组块写（一次性 `fwrite` 整行块 + names 块）。

### B-8【P2，Confirmed】全仓"逐工作项 spawn 线程池"模式
- 实例：JunkScanner.cpp:285-310（8 外层线程 × 每 `DirSizeCache::SizeOf` 内部再开 2 线程 SizePool → 峰值 1+8+16=25 线程，每目录一对 spawn/join，~90 候选=180 次建池）；DevBuildCache.cpp:314-341（同构）；FolderTreeScanner（B-5）；v2.19 搜索取大小（≤5/键）。
- 实测单线程创建+join ≈0.13ms：Junk 全程额外 ~25ms CPU——**不是主要瓶颈**，但 25 线程瞬时尖峰 + 调度抖动在 16 核机上属于无谓噪声，且所有子池取消语义不一致（外层查 cancel，SizePool 不查）。修复：SessionService 持有 4-6 线程常驻池 + 统一取消令牌（§5）。

### B-9【P2，Likely（罕见路径）】DelegateOp 分离读线程的悬垂引用
- `DelegateOp.cpp:120-164`：reader lambda 按引用捕获 `output`/`outMu`（Execute 栈对象）。`detach()` 分支（CancelIoEx 后 2s 仍未收尾）后主线程继续 `CloseHandle(outRead/readDone)` 并返回——分离线程随后 `lock_guard(outMu)` 写已析构对象 = UAF；`SetEvent(readDone)` 作用于已关句柄（有句柄复用误触风险）。可达性低（CancelIoEx 通常 ms 级生效），但模式错误。
- 修复：`shared_ptr` 持有输出态+事件，reader 捕值；或彻底去掉 detach（延长 join 上限）。
- 其余子进程机制**核过无问题**：job 对象 KILL_ON_JOB_CLOSE、30min 有界轮询、管道截尾 64KB、`CREATE_NO_WINDOW`、takeover 10s 有界等待 + 互斥量重取（main.cpp:16-61）。

### B-10【P2，Confirmed】Executing 类任务零取消检查点 + 关闭尾
- `RunPlan`（SessionService.cpp:703-784）/ `RunEmptyQuarantine` / `UndoRecordsAsync` 的循环均不查 `cancelScan_`；DelegateOp 上限 30min（DISM 场景合法）。`Shutdown()` 无条件 join（:1089-1092）→ 用户关窗/被 takeover 时，旧实例要等当前项（单次迁移复制、DISM）走完才退出——上轮"关闭 join 尾"同源，且 B-1 的 267MB 写尾也在其中。修复：RunPlan 每 N 项查 cancel；Shutdown 给 join 加上限+分离缓存写线程。

### B-11【P3，Confirmed（今日良性，重构雷区）】ResetForTesting 漏清 ready_/frnToDirPath_
- `VolumeIndex.cpp:149-163` 清 `dirPaths_` 但**不清 `frnToDirPath_`**（其值是指向 dirPaths_ 键的指针——悬垂）、不置 `ready_=false`。今日无害纯属侥幸：单 worker 串行 + 取消路径后 `nodes_` 为空使 Search 循环零次。任何引入并发/优先级通道的重构（§5）都会把"良性"变成 UAF。修复：补两行清除。

### B-12【P3，Confirmed】索引缓存落盘 remove+rename 非原子窗口
- `VolumeIndex.cpp:1308-1311`：`remove(final)` 后 `rename(tmp,final)`——中间崩溃=无缓存（下次重建）。同仓 `SaveResultsCache` 已用 `MoveFileExW(REPLACE_EXISTING)`（SessionService.cpp:211），照抄即可。

### B-13【P3，Confirmed】ghost 清理 O(N×M) + 每比较现造字符串
- `SessionService.cpp:469-477` 与 `:1012-1018`：对每行 `std::find(dead.begin(), dead.end(), ToLower(it.path.wstring()))`——每次比较重造 lowered 串。dead 多时 500k 次分配。修复：`unordered_set<std::wstring>`。

### B-14【P3，Speculative，仅 -probe】UI 线程直读 VolumeIndex 内部容器
- `MainWindow.cpp:669-675`（SEARCH_DONE 的 probe 分支）UI 线程调 `DebugNameMatchCount/DebugSearchPass1` 直接迭代 `nodes_/names_`——若 worker 恰在重建（如空闲维护 re-walk）即数据竞争。仅 `-probe` 自测路径可达；正式功能未触发。建议 probe 调度到 worker 或在文档标注契约。

---

## 2. 上轮遗留项核对（先核代码再下结论）

| 上轮项 | 核验 | 结果 |
|---|---|---|
| B-1 缓存读写不对称（walk 标志字节） | VolumeIndex.cpp:1287-1291 写侧已发 walkFlag；实测头 `walk=1` | ✅ 已修复且存活 |
| B-2 RefreshWalk 不 Finalize | :683-686 WalkBuild 成功后 Finalize+entryCount | ✅ 存活 |
| B-3 空闲维护无冷却不可取消 | 冷却 15min（MainWindow.cpp:1305-1306）+ WM_ACTIVATE 取消 | ✅ 半修复：**取消仍只挂 WM_ACTIVATE**（本报告 B-3 补全） |
| T-A3 Finalize 环预算 | :226-237 budget=2N+16 | ✅ 存活（实测正常走完 3.09M） |
| T-A4 缓存池上限 | :1149-1150 kMaxNameBytes/kMaxNameLen + 读侧 bounds | ✅ 存活（实测 bad=0） |
| v2.17b parentFrn bool 截断 | :594 `pair<std::wstring, uint64_t>` | ✅ 存活 |
| v2.16c walk 回退可达性 | :326-344 独立探测；日志 23:16/23:53 回退已触发 | ✅ 存活 |
| "内存压缩"（路线图 #2） | 未做 | ⏳ 本报告 §4 给实测表 |
| "FolderTree 并行化"（#3） | v2.18 反而加层放大（B-5） | ❌ 恶化 |
| "关闭 262MB 写尾"（#5） | 未做；且 B-1 使写尾更频繁 | ⏳ B-10 |

---

## 3. 量化实验（`.tmp-review/r6/03-exp/`，可重跑：`build.cmd`）

### 3.1 sizing_bench —— v2.19 并行取大小专项（1000 条合成路径，预热后均值）

| 方案 | 每搜均值 | 最差 | 备注 |
|---|---|---|---|
| A 串行 1000×GetFileAttributesExW（v2.18 语义） | 39.0 ms | 42.0 ms | 39µs/次 |
| **B v2.19 spawn-per-search（本线程+5）** | **10.0 ms** | 11.8 ms | 5 线程 spawn+join 仅 0.25ms |
| C 常驻 5 线程池 | 9.2 ms | 9.8 ms | 比 B 省 0.8ms |
| D 纯 spawn×5×10 波 | 0.65 ms/波 | — | 0.13ms/线程 |
| E 1000 行全量拷贝 ×4 | 4.3 ms（1.08ms/拷） | — | B-6 的浪费 |
| F 排序 1000 名：比较器内 ToLower vs 预计算 | 4.79 vs 0.84 ms | — | B-6 |

**专项结论**（回应审查项 2）：
1. "线程创建风暴"**不成立**：TaskKind CAS 保证同一时刻只有一批 ≤5 个 spawned 线程存活；连续键入 10 次 = 10 个**串行**批次，总开销 ~6.5ms，对比每批 10-40ms 的工作量占 2.5%。常驻池收益 <1ms/键——优先级低于统一取消令牌。
2. **写并发正确性成立**：`items` 在池启动前已定型，`cursor.fetch_add` 逐项分区，`items[i].sizeBytes` 互不相交；join 先于 StoreResults。cancel 每项检查（退出延迟 ≤1 次属性查询 ~40µs）。例外：worker 内 `LongPath` 若 bad_alloc 会 std::terminate（理论性，与常驻池无关）。
3. v2.19 净收益 3.9×（39→10ms），是真优化；真正的每键浪费在 B-6（7ms）> spawn 开销（0.65ms）。

### 3.2 cache_bench —— 真实 267MB 缓存只读分段（index-cache-C.bin）

```
walk=1 nodes=3,087,989 nameBytes=144,223,870(137.5MB) avgName=23.4 wchar
P1 逐行 fread(现行)      332.2 ms   ←→ 块读+解码 71.5 ms（4.7×）
P3 names 块读             50.9 ms
P4 bounds 检查            10.8 ms（bad=0）
P5 Finalize            3,722.3 ms（byFrn 761 + childrenOf 285 + DFS 2,676）
   maps: byFrn=3.09M childrenOf=387k dirPaths=445k frnToDirPath=445k
   内存: nodes 117.8 + names 137.5 + 查找表 549.2 MB；WS 259→808 MB
   buckets: byFrn 8.39M(LF .37) childrenOf 4.19M(LF .09!) dirPaths 524K(LF .85)
P6 行写(现行) 894.1 ms    ←→ 块写 413.0 ms（本地临时文件）
```

### 3.3 log_bench —— Logger 复现
见 B-2：em-dash 行后 `fail=1 bad=1` 永续，line3 丢失，文件止于 `line2 has em-disk`。与生产日志 od 取证逐字节同构。

---

## 4. 内存构成（实测）与压缩方案

**905MB 工作集分解**（3,087,989 项 / 444,864 目录；复现峰值 808MB + 结果集/历史/碎片 ≈ 905MB 吻合）：

| 结构 | 实测/推算 | 占比 | 备注 |
|---|---|---|---|
| `nodes_` 3.09M×40B | 117.8 MB | 13% | 布局本身紧凑 |
| `names_` | 137.5 MB | 15% | 平均名 23.4 wchar |
| `byFrn_` | ~141 MB（buckets 67 + 链节点 74） | 16% | reserve(2N) → LF 0.37，半数桶空转 |
| `childrenOf_` | ~75 MB+（buckets 33.5 预留给 387k 项，LF 0.09） | 8% | **91% 桶浪费** |
| `dirPaths_` | ~70 MB+（445k×[节点 48B + 堆键 ~98B]） | 8% | 键=完整小写路径串 |
| `frnToDirPath_` | ~18 MB | 2% | |
| DFS 瞬态+rehash 碎片 | ~130 MB（549 实测 − 上述 ~404 结构和） | 14% | Finalize 期间峰值 |
| 查找表合计 | **549.2 MB（实测 ΔWS）** | **61%** | 数据的 2.15 倍 |

**压缩方案表**（目标 ~804MB → ~340-400MB）：

| # | 方案 | 预估省 | 风险 |
|---|---|---|---|
| 1 | `byFrn_`→开放寻址 u64→u32 平铺哈希（LF 0.75） | ~75-150 MB | 自实现或 robin_hood；改动面=Finalize/ApplyChanges/Search 爬链 |
| 2 | `childrenOf_`→CSR（childIdx u32×3.09M + dirOffset u32×44.5万） | ~60-95 MB | 重建期间排序即可；CollectChildren/WalkSubtree 改迭代 |
| 3 | `dirPaths_`→`u64 路径哈希→u32 idx` 平铺（碰撞用 PathOf 全比） | ~60-90 MB | HasDir 变为哈希+验证；ghost prune 走 TryGetEntry 不受影响 |
| 4 | `frnToDirPath_`→随 #3 存 u32；或 Search 改为爬链构造 lowerPath | ~11 MB | matchPath 路径每命中多 ~深度次查找 |
| 5 | Finalize 惰性化（#B-4）：载入时只建 byFrn_，dirPaths_ 首用再建 | 载入 -2.7s + 峰值 -~200MB | HasDir/JunkRules 索引路径首用延迟 ~2s |
| 6 | 不建议动 nodes_/names_（收益 <25MB， upheaval 大） | — | — |

---

## 5. 调度重构方案（每步可独立合入）

**现状**：单 FIFO worker + TaskKind 互斥；Verifying/Scanning 分钟级不可抢占；cancelScan_ 单点标志（覆盖：扫描器、walk、搜索 sink/取大小、索引构建循环；**不覆盖**：SizePool、RunPlan、Undo、EmptyQuarantine、DelegateOp）。

- **Step 1（1 行，P0）**：RunSearch 对 `IsVerifying()` 也 CancelScan —— 立即消除 B-3 最痛场景。
- **Step 2（小）**：给 SizePool/FolderTree 加取消检查（B-5）；RunPlan 每 16 项查 cancelScan_（B-10）。
- **Step 3（中）**：**双通道**：交互通道（Search/VerifyPaths/Preview，只读索引）+ 维护通道（索引变更：build/refresh/walk/cache write 仍串行，保持 VolumeIndex 单写者契约）。交互任务在维护任务的检查点（每目录/每 64 项）处置 cancel→yield。不需要真优先级抢占，即可把"搜索被分钟级任务卡死"压到"≤1 检查点"。
- **Step 4（中）**：SessionService 持 4-6 线程常驻池，统一供 v2.19 取大小 / Junk / DevBuild / FolderTree sizing（替代全部 per-item SizePool）；收益主要是统一取消+削峰（CPU 上实测仅省 ~1ms/键 + Junk 25ms/扫描）。
- **Step 5（中）**：缓存写移入维护通道尾部 + dirty 标志（B-1）+ 块写（B-7）；Shutdown join 带上限、写尾分离线程。
- **注意事项**：任何引入并发的 Step 都必须先修 B-11（ResetForTesting 补清 ready_/frnToDirPath_），否则优先级通道会踩中今日被单 worker 串行掩盖的悬垂指针。

---

## 6. 修复顺序

| 级 | 项 |
|---|---|
| **P0** | B-2 Logger UTF-8（恢复一切诊断）；B-1 脏标志跳过零变更 267MB 重写；B-3 RunSearch 取消 Verifying（1 行） |
| **P1** | B-4 缓存块读+Finalize 惰性化；B-5 FolderTree 单遍分层+可取消；B-6 去诊断拷贝+预计算排序键；§4 #1/#2（内存紧凑化主体） |
| **P2** | B-7 块写；B-8 常驻池（随 Step 4）；B-9 DelegateOp reader 生命周期；B-10 Executing 取消+有界 Shutdown；§4 #3/#4 |
| **P3** | B-11、B-12、B-13、B-14；DirSizeCache size==0 不缓存（空目录每扫重走）小注 |

---

## 7. 跨报告线索

- **给 02（UI 延迟）**：①UI stall watchdog 别复用 Logger（B-2，中文/破折号即哑）；②taskMode_ 在 Verifying 期间为 None——动作矩阵"解锁但 worker 忙"的语义裂缝是 B-3 的共因；③启动自动搜索在恢复标签非 Search 时 RunSearch 空转（日志 `RunSearch: tab=4` 两次实锤，MainWindow.cpp:1701-1704 dynamic_cast 失败静默返回）。
- **给 04（稳定性）**：B-9（分离线程 UAF 模式）、B-11（悬垂指针雷区）、B-14（probe UI 线程读索引）。
- **给 05（安全）**：缓存 v2 无 MAC 的既有决策（v2.16d 注释）与本轮无冲突；B-12 的非原子窗口属可靠性非安全。
- **给 06（架构）**："逐工作项 spawn 池"已成跨模块惯例（8 处），值得进重复代码/模式清单；SessionService+VolumeIndex 双 God object 与 Step 3 通道化互相成就。
- **给 08（功能）**：FolderTree 层层分析（v2.18）当前性能形态接近不可用（B-5），扩 depth 前必须先单遍化。

---

## 8. 被推翻的假设 / 已验证无问题 / 未验证项

**被推翻**：
1. "v2.19 每键 spawn 6 线程=线程创建风暴"——实测 0.25-0.65ms/批 vs 10-40ms 工作；CAS 门控使批次严格串行（§3.1）。
2. "日志副本在拷贝时被截断拼接"——原文件逐字节相同；截断+拼接是 Logger 自身缺陷（B-2）。我自己的初始假设，被 od 推翻。
3. "walk 回退从未在本机发生"（上轮 03 对 v2.13a 提权时代的结论在今日不成立）——23:16/23:53 两次触发回退告警，现行缓存 walk=1（实测头）；unelevated 常态下 walk 是唯一索引源。

**已验证无问题**（逐一读到代码/实验）：
- StartTask 的 CAS+taskGen 语义：DONE 盖代数戳正确；被取消搜索的部分结果会被 UI 代数闸丢弃，不会覆盖新结果；join 旧 worker 先于新 spawn；SearchAsync 的 `static lastSave` 经线程创建/join 建立 happens-before，无竞争。
- v2.19 取大小的写并发与取消（§3.1-2）。
- WalkBuild condvar 终止条件（queue 空 && active==0）与取消传播（≤1 目录/worker）；v2.17b parentFrn 修复存活。
- FastWalk 池终止/取消（inflight 原子 + 每文件检查）。
- VolumeIndex 跨线程契约现状：UI 仅触原子量与 RegistryMu_ 保护聚合；容器访问全在单 worker——今日无竞争（B-11/B-14 是未来雷）。
- TryLoadCache 边界校验（实测 bad=0）、Finalize 预算、kMaxIncrementalChanges=20000、SaveResultsCache tmp+rename 原子。
- takeover 10s 有界等待与互斥量重取循环。
- DirSizeCache (path,mtime) 键：目录 mtime 不反映深层变更属已知折衷；真正小瑕疵是 size==0 不缓存。

**未验证 / 局限**：
- 未运行 MiniSys 本体（用户禁令）；所有测量为进程外复刻，Finalize/写盘时长存在负载方差。
- FolderTree v2.18 实际耗时为解析推算（~2× 旧 75-178s），未在真实盘上实测（避免捶 98% 满的 C:）。
- v3-HMAC 时代 "cache write failed" 的根因不可在 HEAD 诊断（代码已随 v2.16d 回退）；日志显示失败发生在 "indexed" 后 0.8-1.4s，早于 267MB 落盘完成点，支持"写中途失败"而非"盘满"。
- 905MB 为用户报告值；本复现峰值 808MB，差值归结果集/历史/分配器碎片（未逐项测）。
- 提权 USN 路径（BuildFull V1/V0、RefreshFromUsn delta）仅读码+旧日志时间线，未在提权环境复测。
- 硬件变化（16 核）下 JunkScanner 8+16 线程尖峰的实际调度影响未 profiling。

## 附：实验物料

- `​.tmp-review/r6/03-exp/build.cmd`（vcvars + cl /O2）、`sizing_bench.cpp/.exe`、`cache_bench.cpp/.exe`、`log_bench.cpp/.exe`、`synth/`（1000 合成文件）、`logtest.txt`。真实缓存仅 `_wfsopen("rb")` 读；写对比落在 03-exp 本地临时文件并即删。
