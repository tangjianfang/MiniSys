# 03 号补审:后台线程 / 多卷索引 / 缓存(2026-10-11,HEAD 76f6514 = v2.13c)

对象:MiniSys/src/core/{SessionService,VolumeIndex,JunkScanner,DevBuildCache,FolderTreeScanner}.cpp、util/{FastWalk,DirSizeCache,PathUtils}.cpp、MainWindow.cpp。用户数据只读取证:%LOCALAPPDATA%\MiniSys\logs\minisys.log、index-cache-C.bin(仅读结构)。本机 16 逻辑核。

---

## 摘要(Top 风险 + 量化)

| # | 发现 | 等级 | 量化 |
|---|---|---|---|
| B-1 | **磁盘索引缓存写/读格式不对称:save 不写 walk 标志字节,load 读它 → 头部错位 → 缓存加载必败** | P0 CONFIRMED | 实测 4/4 个 v2.13a+ 会话全量重建:USN 枚举 18s + Finalize ~1.5s + 落盘 250MB/次;"重启秒开"特性自 v2.13a 起从未生效 |
| B-2 | **RefreshWalk 重扫后从不 Finalize → 全部查找表(byFrn_/childrenOf_/dirPaths_)指向旧数组,搜索路径错拼、同轮幽灵清理错误剪除活行并落盘** | P0(代码级 CONFIRMED;本机未触发,见"被推翻假设") | 触发面 = walk-built 卷 × 一次成功的空闲重扫;MiniSysTests 对 RefreshWalk/WalkBuild 0 覆盖 |
| B-3 | 空闲维护无冷却 + Verifying 任务无任何 UI 取消路径 → 单 worker 队头阻塞 | P1 CONFIRMED | walk 机:空闲≥5min 后每个 30s tick 重发维护,重扫分钟级(估 2-6min),用户回座后扫描/执行被拒、搜索 80ms 重试静默停摆;本机实测 FolderTree 扫描 75-178s 同样独占 |
| B-4 | 被取消的 RefreshWalk 直接销毁索引(valid_=false、nodes_ 清空),且无自愈路径 | P1 CONFIRMED | 磁盘缓存仍是好的,却不 TryLoadCache 恢复;其他卷有效时 AnyValid()==true,切搜索页也不重建 → 该卷搜索静默 0 结果直到手动"重建索引" |
| B-5 | WalkBuild 内 `reported` 非原子,6 线程并发读写(UB);appendMu 锁内做 O(path) 拼接与大块 realloc | P2 | 实害轻(进度重复发送);names_ 最后一次翻倍在锁内 memcpy ~141MB,停顿 10-30ms×全部线程 |
| B-6 | FolderTreeScanner 串行 DirectorySize | P2 | 日志实测 75-178s/次(tab=4,2026-10-07 21:07-21:21 六次);接并行池+DirSizeCache 预计 10-25s |

内存推算与用户线索吻合:理论 173-181 B/项(3.03M 项 ≈ 524-548MB;2.83M 项 ≈ 490-513MB)vs 实测 ~490MB/175B 项 —— 差距 ≈ 0,**大头不是节点内联而是 byFrn_ 哈希表与三个路径/子表结构**(见 §内存推算表)。

---

## 发现清单

### B-1(P0,CONFIRMED,置信度高)缓存格式写读不对称,索引缓存永远加载失败

- **位置**:`VolumeIndex.cpp:1209-1228`(SaveCacheIfWorthwhile)vs `VolumeIndex.cpp:1142-1143`(TryLoadCache 读 `cWalk`)。
- **机制**:save 顺序 magic|version|drive|serial|journal|nextUsn|**nodeCount|nameBytes**(50 字节头 + 40B/行 + 名字池);load 在 nextUsn 之后多读 1 字节 walk 标志 → nodeCount/nameBytes 全部错位 1 字节 → `cNodeCount > 50000000` → `break` → 加载失败 → EnsureBuilt 走 BuildFull 全量重建。b446d26(v2.13a,10-07 03:31)给 load 加了读、从未给 save 加写。
- **取证**(本机只读):
  - hexdump index-cache-C.bin(262,249,554 B):按"无 walk 字节"解析得 nodeCount=3,027,780、nameBytes=141,138,304,期望大小 = 50 + 3,027,780×40 + 141,138,304 = **262,249,554 = 实际大小,精确吻合**;按"有 walk 字节"解析得 9.2e18,必败。
  - minisys.log 全文 "loaded cache" 仅 1 次(2026-10-07 03:32:42,v2.13a 之前的老进程);v2.13a 后 4 个会话(03:37、16:16、20:37、10-11 02:17)全部直接 "batch 1:…" 全量枚举。
- **后果**:每次启动多 ~20s 首索引(02:17 会话:枚举 02:17:12.96→02:17:30.85 + Finalize 至 :32.33);每次启动向 %LOCALAPPDATA% 重写 250MB;首进搜索页/首垃圾扫描被这 20s 占住 worker(叠加 B-3)。
- **修复**(1 行 + 测试):save 在 nextUsn 后补 `const uint8_t walk = walkBuilt_ ? 1 : 0; ok = ok && fwrite(&walk,1,1,f)==1;`。旧文件本就读不成,无需迁移;补 save→load round-trip 测试(当前 VolumeIndexTests 无任何缓存测试)。

### B-2(P0,代码级 CONFIRMED,置信度高;本机未活体触发)RefreshWalk 后查找表全部悬空

- **位置**:`VolumeIndex.cpp:642-646` RefreshWalk 直接调 WalkBuild;WalkBuild(`:490-638`)只重建 nodes_/names_,**从不调 Finalize()**(全文 Finalize 调用点 :182/:469/:773/:1193,均不在 WalkBuild 路径),也不更新 entryCount_。
- **机制**:重扫以新的发现顺序重发合成 FRN(1..N),nodes_ 整体替换;而 byFrn_/childrenOf_/dirPaths_/frnToDirPath_ 仍是旧树,且 frn 值域重叠(都是 1..N)→ 查找"成功但指向错误节点":
  - `Search` Pass2 `OriginalCasePathOf`(:909-929)按新 parentFrn 查旧 byFrn_ → 拿新数组里错误位置的名字拼路径 → **搜索结果路径错拼**(显示/勾选/执行都基于错路径;执行侧靠 mtime 复验大概率拦截,但显示层必错)。
  - `FindByPath`(:783-804)dirPaths_ 键命中 → `&nodes_[旧索引]` = 错节点 → `childrenOf_.find(错误节点的 frn)` 乱配 → HasFile/HasDir 结果不可判。
  - **同轮放大**:`IdleMaintenanceAsync`(SessionService.cpp:439-481)第 2 步幽灵清理紧跟第 1 步重扫,用悬空查找表判 `TryGetEntry` 失败 → **活行被当幽灵剪除并 StoreResults+SaveResultsCache 落盘**,重启后也不复活。
- **触发面**:walk-built 卷(USN 枚举 <10000 记录的机器 —— 恰是 v2.13a 要救的目标机型)× 一次成功的空闲重扫。本机 C: USN 有效(见"被推翻假设"),故日志无迹象;测试零覆盖(VolumeIndexTests.cpp 无 RefreshWalk/WalkBuild 字样)。
- **修复**:`RefreshWalk` 成功后 `Finalize(); entryCount_.store(nodes_.size()); valid_.store(true);`(save 已在 WalkBuild 内完成,时机正确——落盘只含 nodes_/names_,不受悬空表影响);补"build→refresh→HasDir/Search 路径正确"回归测试。

### B-3(P1,CONFIRMED,置信度高)空闲维护无冷却、不可取消 → 单 worker 队头阻塞

- **位置/机制**:
  - 触发:`MainWindow.cpp:1016-1056` OnIdleCheck(30s 周期 TIMER_IDLE_REFRESH,:597):空闲≥5min、窗口可见、非搜索页、页龄 10-30min → **立即** `IdleMaintenanceAsync()`(无倒计时、无冷却戳);页龄≥30min → 10s 倒计时 → OnScan 全页重扫。
  - 独占:`SessionService.cpp:402-489` 以 TaskKind::Verifying 占用唯一 worker;walk 卷走 `RefreshWalk`(分钟级)。StartTask CAS(:302-309)busy 即拒。
  - **无取消路径**:CancelScan 全部 UI 调用点仅两处——OnScan 的 IsScanning 分支(MainWindow.cpp:1224)与 RunSearch 的 IsSearching 分支(:1431-1433);Verifying 维护任务**用户无法取消**。
  - **无冷却再入**:IdleMaintenanceAsync 不改 LastScanAt,条件持续满足 → 上次维护一结束,下个 30s tick 再发。walk 机上空闲期间 worker 接近连轴转(重扫 2-6min + ≤30s 空隙循环)。
- **拒绝始时/时长**(量化):
  - 扫描/执行:OnScan `MainWindow.cpp:1229-1231`、OnExecute `:1496-1499` 立即 hint"已有任务在进行中";最长等待 = 当前任务剩余(walk 重扫估 2-6min;本机 FolderTree 全页重扫实测 75-178s)。
  - 搜索:RunSearch `:1434-1438` SearchAsync 失败 → 80ms TIMER_SEARCH_RETRY 静默重试,直到任务结束(用户只见状态栏"空闲维护:重扫…",输入框不锁——SetTaskBusy 未被维护任务调用,taskMode_ 仍 None)。
  - 校验:OnVerifyList `:736-738` auto 静默丢弃/manual hint。
  - 频率:walk 机 = 空闲时段近乎常态;USN 机 = 每个空闲段首次 2-4s(ApplyChanges→Finalize 重建 300 万节点哈希表),后续 tick 增量为零时近零耗时——USN 机实害主要是 FolderTree/junk 长扫描本身。
- **修复(最小改造)**:
  1. 维护冷却:SessionService 记 `lastMaintenanceAt_`,OnIdleCheck 间隔 <10min 不再发(几行)。
  2. 让维护可取消:新增 `TaskKind::Maintaining`(UI 矩阵按 Verifying 同样放行);WM_TIMER 的 30s tick 里检测 `GetLastInputInfo` 输入戳变化且当前任务为 Maintaining → `CancelScan()`(WalkBuild 取消粒度 = 每帧一个目录,取消延迟亚秒级)。
  3. 取消后旧索引保留(见 B-4),维护自然续期——不需要真抢占即可把"用户回座被拒"从分钟级压到秒级。
  - 更完整的分段/双 lane 方案见 §调度重构方案。

### B-4(P1,CONFIRMED,置信度高)取消的 RefreshWalk 销毁索引且无自愈

- **位置**:WalkBuild 取消分支 `VolumeIndex.cpp:623-630`(v2.13b 修复:valid_=false、entryCount_=0,防"空壳索引服务搜索")。
- **机制**:取消后 walkBuilt_ 亦为 false → ①IdleMaintenanceAsync:416-417 两个跳过条件(!IsValid / IsWalkBuilt)都命中,空闲维护**不会**重建;②OnTabChanged(MainWindow.cpp:1145)仅当 `!AnyValid()`(全局,跨卷)才 BuildIndexAsync —— 其他卷仍有效时该卷**静默 0 搜索结果**,唯一恢复路径 = 手动"重建索引"或重启。
- **修复**:取消分支改调 `TryLoadCache(drive_)` 恢复上次落盘的 walk 索引(磁盘缓存未损,250MB 读约 1-2s),恢复不了再走现在的显式失效。(依赖 B-1 修复后缓存才真正可载。)

### B-5(P2,CONFIRMED,置信度高)WalkBuild 数据竞争与锁内重活

- `VolumeIndex.cpp:516` 声明普通 `size_t reported`,由 6 个 worker 在 `:601-606` 无锁并发读写(`reportedLog` 是原子的,`reported` 漏改)。实害 = 进度重复/漏发 + 标准 C++ UB。修复:改 `std::atomic<size_t>`。
- appendMu 锁内(:573-586):`fr.path + L"\\" + name` 的 O(path) 拼接、`names_ += name`(池翻倍时锁内 memcpy,最后一次 ~141MB ≈ 10-30ms 全线程停顿)、`nodes_.push_back`(增长翻倍同性质)。6 线程在 2-6min 尺度上影响有限,但属可白拿的并行度:child 路径在锁外拼好,锁内仅 push;或名字池分块(deque of blocks)。
- 其余核对结论:condvar 终局条件(queue 空 && active==0 才 done,均在 qMu 内判定/通知)、nextFrnVal 全程 appendMu 内、cancel 经 qMu+notify_all+join 传播(取消延迟 ≤ 单个大目录枚举,WinSxS 类 1-3s)、根帧仅初始单入队且 "."/".."/reparse 跳过无重复入队 —— **正确**;SaveCacheIfWorthwhile 在 BuildFull 路径内的调用是死代码(valid_ 尚为 false),实际落盘由 EnsureBuilt:285 完成,net 效果正确(见"已验证")。

### B-6(P2,CONFIRMED)FolderTree 串行定大小,是本机最长的 worker 独占任务

`FolderTreeScanner.cpp:73` 对每个顶层目录顺序调 `DirectorySize`(单线程,PathUtils.cpp:135);日志实测 tab=4 每次 75-178s,且空闲自动重扫每 ~30min 一轮。改用 JunkScanner 同款 8 线程池 + DirSizeCache(子树 mtime 键)预计 10-25s,同时把 B-3 的最长等待削一个量级。注意保持发现顺序输出(JunkScanner 已有先例:并行定大小、按序 emit)。

### P3 备忘(不单列编号)

- Finalize `:191` `childrenOf_.reserve(nodes_.size())`:300 万桶 ×8B = 24MB,而键只有 ~30 万目录;`byFrn_.reserve(n*2)` 桶 48MB —— 见紧凑化。
- `filePaths_`(VolumeIndex.h:206)从未填充,死成员。
- FastWalk.cpp:108 每完成一个目录 `cv_.notify_all()`(8 线程惊群;热目录流下每秒上千次)。改:push 时 notify_one,终局/队列空时才 notify_all。
- THREAD_MODE_BACKGROUND_BEGIN 仅 USN 枚举路径(BuildFull:296-298)设置;WalkBuild 的 6 线程全默认优先级,"空闲维护"期间与前台争磁盘——建议 walk 线程同样降为后台 I/O 优先级(配合 B-3 让位更稳)。
- DevBuildCache `dirCache`/DirSizeCache 无淘汰,会话级上界 ~150k 目录条目(几十 MB 内),实测非问题。

---

## 内存推算表(Node 布局 + 名字池,实测参数:3,027,780 项 / nameBytes 141,138,304)

x64 下 `sizeof(Node)=48`:frn 8 + parentFrn 8 + attrs 4 + 填充 4 + lastWrite 8 + nameOff 4 + nameLen 4 + isDir 1 + 填充 7。

| 结构 | 每项字节 | 总量 | 构成 |
|---|---|---|---|
| nodes_(vector) | 48.0 | 145.3MB | 15B/项为对齐填充;vector 翻倍瞬时峰值另 +50% |
| names_(wstring 池) | 46.6 | 141.1MB | 实测 23.31 wchar/名;增长翻倍 → 容量峰值可达 ~2× |
| byFrn_ unordered_map | 40-48 | 121-145MB | MSVC 链节点 24B(+堆取整)+ `reserve(2n)` 桶 6.06M×8B=48.4MB |
| childrenOf_ umap<u64,vector> | ~17 | ~52MB | 桶 24.2MB(**键仅 ~30 万目录,300 万桶超额**)+ 每目录 vector 堆块 |
| dirPaths_ umap<wstring,u32> | ~17 | ~52MB | 全路径小写 key 堆串(均 ~50-60 wchar)+ 链节点 |
| frnToDirPath_ umap<u64,ptr> | ~4 | ~13MB | |
| filePaths_ | 0 | 0 | 死成员 |
| **合计** | **~173-181** | **~524-548MB @3.03M** | **@2.83M ≈ 490-513MB ≈ 实测 ~490MB / 175B 项 ✓** |

结论:差距不在"节点内联"——nodes_+names_ 仅 95B/项(54%),其余 ~46% 是三张哈希表(其中 byFrn_ 一张就 ~80B/项)。

**紧凑化方案与预期节省**(按 3.03M 项):

| 措施 | 改法 | 节省 |
|---|---|---|
| Node 48→24B | parentFrn→u32 parentIdx(节点下标);nameLen→u16(NTFS 分量 ≤255);attrs→u16 位域并吸收 isDir;walk 型 frn≡idx+1 可整体省掉 frn | −73MB |
| byFrn_ → 开放寻址 u32 表(负载 0.75) | 2×3.03M×4B=24MB;walk 型直接删除(恒等映射) | −97~121MB |
| childrenOf_ → CSR(按 parentIdx 排序的连续子数组) | 3.03M×4B 子索引 + 30 万×4B 偏移 = 13MB,顺带消灭 24MB 超额桶 | −39MB |
| dirPaths_ → 二级路径池 + 偏移开放寻址 | 路径串进大池(~30MB)+ 3MB 表 | −19MB |
| names_ 名字 interning(去重后存偏移) | 文件名重复率高(30-50% 典型) | −40~70MB |
| **合计** | | **~524MB → ~200-240MB(−55%),175B/项 → ~70B/项;缓存文件 250MB → ~150MB(行 40→24B、可去 frn)** |

落地顺序建议:先 byFrn_ 开放寻址 + childrenOf_ reserve 修正(收益最大、不改磁盘格式);Node 紧凑化需 bump 缓存版本(与 B-1 一并做)。

---

## walk 索引新鲜度(任务 4)

- **窗口量化**:误导窗口 = 自上次索引刷新以来的全部变更。刷新点仅三个:进程启动(B-1 修复前 = 每次全量;修复后 = 缓存 + USN 增量)、垃圾扫描(JunkScanner.cpp:225 EnsureBuilt)、空闲维护。**用户活跃期间(空闲 <5min)窗口无上限**(小时级);walk 卷还跨重启(读盘缓存)。搜索本身(SearchAsync:495-565)不做任何刷新/存在性校验——top-150 size fetch 失败仅置 0B,无"已不存在"标记。
- **兜底确认**:执行链 GuardRails + PlanBuilder 的 size/mtime 逐项复验会拦住"已删路径",提权/误删风险不存在;误导纯在显示层(勾选执行时才报"已不存在")。
- **轻量失效建议**:
  1. USN 卷:SearchAsync 入口先 `RefreshFromUsn`(小 delta 毫秒级;>20k 变更或 walk 卷跳过)——把活跃期窗口从小时级压到一次搜索的粒度;
  2. walk 卷:对命中集父目录抽样(K=16)`GetFileAttributesExW` 比对 dirPaths_ 对应节点 lastWrite,漂移超阈值 → 状态栏提示"索引可能过期(上次遍历 HH:MM),执行前逐项复验";
  3. top-150 size fetch 失败的行标"已不存在?"而非 0B。
- 目录 mtime 抽样校验可行性:节点已存 lastWrite(FindFirstFile 的 ftLastWriteTime),父目录键在 dirPaths_,抽样一次 GetFileAttributesExW ~10µs —— 可行,成本可忽略。

---

## 调度重构方案(任务 6,每步可独立合入)

- **Step 0(热修,S)**:B-1 一行 + B-2 三行 + 回归测试。不动架构。
- **Step 1(维护节流 + 可取消,S)**:`lastMaintenanceAt_` 冷却 10min;`TaskKind::Maintaining`;30s tick 检测输入恢复即 CancelScan。效果:队头阻塞上界从"重扫剩余分钟"降到"≤一个大目录枚举(1-3s)",且空闲期不再连轴重扫。
- **Step 2(影子构建,S-M)**:RefreshWalk 构建进临时 nodes_/names_(或临时 VolumeIndex),成功才 Finalize+原子换入;取消 = 丢弃影子、旧索引原样有效(根治 B-2/B-4 的取消面)。WalkBuild 每 20k 目录一个取消检查点(现有粒度已够,补让位标志)。
- **Step 3(双 lane,M)**:StartTask 增优先级参数;busy 时用户任务登记 pendingUserTask_,维护体在检查点发现即提前收尾返回;worker 空闲先取交互 lane 再取维护 lane。保持单 worker、不改容器线程契约(历轮评审关掉的竞态不重开)。
- **Step 4(可选)**:FolderTree 并行化(B-6)与 walk 断点续跑(影子构建落盘);Step 2 后续跑价值有限,低优先。

---

## 修复顺序

- **P0**:B-1(1 行 + round-trip 测试)、B-2(Finalize + 回归测试)——数据正确性与核心特性失效。
- **P1**:B-4(取消恢复 TryLoadCache)、B-3 Step1(冷却 + Maintaining 可取消)、B-6(FolderTree 并行化)。
- **P2**:B-5(reported 原子化、锁内拼接外移)、Step 2/3、内存紧凑化第一批(byFrn_ 开放寻址 + childrenOf_ 桶修正)。
- **P3**:filePaths_ 删除、notify_all 惊群、walk 线程后台 I/O 优先级、names_ interning、Node 24B(随缓存 v3)。

---

## 已验证无问题 / 被推翻假设

**已验证无问题:**
1. WalkBuild 并行队列协议:终局条件(queue空&&active==0 在 qMu 内判定)、appendMu 对 nodes_/names_/nextFrnVal 的全覆盖、cancel 经 qMu+notify_all+join 的传播与 `cancelled` 标志的 happens-before、根帧单入队无重复 —— 除 B-5 的 `reported` 外正确。
2. 四个线程池(SizePool/FastWalk Pool/JunkScanner/DevBuildCache 定大小池)终止条件与共享写均无竞争:`sized[i]`/`dirs[i]` 互斥下标;SizePool 的 Join 提前置 done_ 依赖"扩展走 localStack 不入队"的耦合,当前正确但脆弱(值得注释)。
3. 进度回调与锁:JunkScanner/DevBuildCache 的 pmu 内仅 filename 级字符串构造 + progress(锁序 pmu→progressMu_ 单向,无死锁);FastWalk onProgress 在池锁外。RunSearch 的 80ms 重试在任务结束后必然放行(实测无饿死路径)。
4. DirSizeCache 并发正确性(任务 5):锁只罩 map find/insert,子树计算在锁外;并发重复计算无害;会话级内存 ≤ 数 MB。线程数:外池 min(hw,8)=8 + 每次 SizeOf 内嵌 2 线程 → 峰值 24 线程(16 核机 1.5× 超额,I/O 型可接受;每次定大小 spawn 2 线程的 churn ~数十 ms/百次,可改常驻池,P3)。
5. StartTask 代数戳(R-4)与 DONE 必达(异常路径也 Post)在本次走查中未见回退;`if (worker_.joinable()) join` 与 taskKind_ 置 None 的时序无竞态(单 UI 线程调 StartTask)。

**被推翻假设:**
1. **"并行遍历建成 2,831,802 项"(evolve-log Round 2 / redispatch 线索)**——minisys.log:392-393 证明该数字来自 **USN V1 枚举**("enum done (V1): 2831802 records",journal id 非零);全文无任何 "filesystem walk" 行;10-11 02:17 会话 USN 枚举得 3,027,780 项。即 **HighUsn=INT64_MAX 的 V1 枚举在本机 C: 有效**,walk 回退(及 B-2/B-4 的触发面)在本机从未激活——它们是其他机型(USN 截断)的风险面,修复优先级不变,但"walk 建成 283 万"的记录应更正。
2. "~80B/文件"(redispatch 线索)——实测推算 173-181B/项;80B 只对应 nodes_+names_ 的一半故事(见推算表)。

**未验证/局限:**
- WalkBuild 重扫时长无本机实测(本机不走 walk);2-6min 为单线程 10-30min(v2.6 注释)/6 线程的推算。
- MSVC unordered_map 节点的堆取整(24→32B)按 Windows 堆 16B 粒度推定,误差已用区间表达(121-145MB)。
- 未跑 TSAN(黑名单外工具不可用);B-5 的竞争由代码判读确认。
