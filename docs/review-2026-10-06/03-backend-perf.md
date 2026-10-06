# MiniSys v2.x 并发与后端性能审查报告(角色 3:后台线程 / 外部数据访问 / 缓存 / 扫描)

审查人:并发与后端性能工程师 · 2026-09-30
评审对象:工作树(HEAD=ae3ce19 v2.0 + 未提交 v2.1 UI 变更)。已核实:`git diff ae3ce19 -- MiniSys/src/core MiniSys/src/util MiniSys/src/platform` 为空 —— **全部后端发现同时适用于 v2.0 提交与当前工作树**;用户 23:48 会话所跑 exe 与 build\Release\MiniSys.exe(SHA-256 c03a9e29e91c448a6edd9ee2b14414c6c6c5c2abb229f1079fb3172e9cc35aba,2026-09-30 00:05 构建)后端代码一致,运行证据(日志 23:48:31.727 Win32 87)与代码缺陷直接对应。
验证手段:全量读码 + SDK 头文件比对 + MSVC 编译探针(.tmp-review\03-exp\layout_probe.cpp)+ 用户真实日志/历史只读取证 + 规则目标目录只读测量。**未提权,无法活体复现 FSCTL(见「未验证/局限」)。**

---

## 一、摘要(Top 风险 + 后端归因)

**扫描慢与失败的四条后端根因,证据强度从高到低:**

1. **[Confirmed] VolumeIndex 从未成功构建过**:`MFT_ENUM_DATA` 在 NTDDI≥WIN8 的 SDK(winioctl.h 11932-11936)里是 `MFT_ENUM_DATA_V1` 的**别名**,代码零初始化 32 字节结构体,Min/MaxMajorVersion=0 为非法版本区间 → `FSCTL_ENUM_USN_DATA` 返回 Win32 87(日志 23:48:31.727 直接证实)。共享索引(M2)整体失效,JunkScanner 每次全量回退 FS 遍历。
2. **[Confirmed] 即使修好 87,记录解析结构体布局全错**:VolumeIndex.cpp 自声明的 `MSN_RECORD_V2/V3` 与 SDK `USN_RECORD_V2/V3` 逐字段错位(编译探针实测:代码的 FileReferenceNumber@16 恰是真实的 ParentFileReferenceNumber@16;FileNameLength@64 读到的是文件名第 3、4 个字符)。修复 B-1 而不修 B-2,首次真实枚举即**越界读取/内存爆炸**(nameLen=文件名字符对,单条记录可 append 数百万 wchar)。
3. **[Confirmed→Likely] 57.4s 垃圾扫描的 ~90% 耗在 DirectorySize 尺寸池**:实测用户机器 %TEMP%=256,001 项、C:\Windows\WinSxS=302,177 项、npm-cache=32,244 项——三条 Subtree 规则各由**单线程** DFS 计尺寸,8 线程池按"候选目录"粒度并行,墙钟≈最重一条(TEMP 或 WinSxS,估 25-50s)。**索引修好后这些尺寸遍历依然存在**(USN 记录无 size,ADR-004),DESIGN-v2 的"首扫≤30s/二扫≤5s"在当前架构下达不到。
4. **[Confirmed] npm-cache 隔离失败(Win32 5)是"目录树内有打开句柄"类环境问题 + 应用无诊断**:同批 cargo(87MB)、.nuget(1.3GB)目录 rename 均瞬时成功(history.jsonl 23:51:43 同秒),证明 MoveFileEx 目录机制正常;npm-cache 单独失败且重试仍失败,最可能是 node/npm/AV/索引器持有树内句柄(无 FILE_SHARE_DELETE 的句柄会令目录 rename 返回 ERROR_ACCESS_DENIED)。应用侧缺陷是:**QuarantineOp 无重试、无 Restart Manager 锁定进程检测、错误只给 "Win32 5"**。

**并发正确性最重要的一条**:**B-5 results_ 无锁共享**:UI 线程在 `ApplySortAndRefresh` 中对 `results_[tab]` 原地 stable_sort,worker 线程在 RunScan 结束/RunPlan 收尾时写同一 vector —— 数据竞争(UB,低概率崩溃)。

---

## 二、发现清单

### B-1 VolumeIndex::BuildFull MFT_ENUM_DATA 零初始化 → FSCTL_ENUM_USN_DATA Win32 87【Confirmed】
- 位置:`MiniSys\src\core\VolumeIndex.cpp:231`(`MFT_ENUM_DATA med{};`)、`:237-239`(DeviceIoControl 传 `sizeof(med)`)。
- 证据链:① SDK 头 `"C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\um\winioctl.h"` 11932-11936:`#if (NTDDI_VERSION >= NTDDI_WIN8) typedef MFT_ENUM_DATA_V1 MFT_ENUM_DATA;`;② 项目 vcxproj 未定义 NTDDI_VERSION(WindowsTargetPlatformVersion 10.0,v143),默认 NTDDI≥WIN10;③ 编译探针实测 `sizeof(MFT_ENUM_DATA)=32`(V0 为 24);④ 零初始化 ⇒ MinMajorVersion=MaxMajorVersion=0,文档要求 2 或 3,非法区间返回 ERROR_INVALID_PARAMETER;⑤ 用户日志 23:48:31.727 `FSCTL_ENUM_USN_DATA failed (Win32 87)` 与代码路径一一对应。
- 后果:索引每卷每次构建必失败 → `EnsureBuilt` 恒 false → JunkScanner 全部规则走 FS 回退;M2"共享索引"三条不变量之一(所有 Scanner 消费同一索引)实际未生效;`THREAD_MODE_BACKGROUND` 礼让、增量刷新(M3)全部死代码。**注:测试从未暴露此缺陷**——`VolumeIndexTests.cpp:95-100` 注释自述"FSCTL_ENUM_USN_DATA needs an elevated process",非提权测试在 CreateFile 即 Win32 5 返回,从未走到 FSCTL。
- 修复草案:
  ```cpp
  MFT_ENUM_DATA_V1 med{};
  med.MinMajorVersion = 2;   // V2(NTFS)+V3(ReFS)记录
  med.MaxMajorVersion = 3;
  ```
  同时 `med.StartFileReferenceNumber = nextFrn;` 循环续传逻辑不变。需活体验证(管理员下运行修复后的 EnsureBuilt)。
- 置信度:Confirmed(87 的成因);"Min=2/Max=3 即可通过"为 Likely(未活体验证)。

### B-2 MSN_RECORD_V2/V3 自声明结构体与真实 USN 记录布局全面错位【Confirmed(布局)/Likely(崩溃后果)】
- 位置:`MiniSys\src\core\VolumeIndex.cpp:16-46`(`#pragma pack(push,8)` 的两个结构体,注释称"ntifs.h is WDK-only; re-declared"——**该注释错误**,`USN_RECORD_V2/V3` 就定义在 winioctl.h 中,根本无需自声明)。
- 探针实测错位表(MiniSys 字段 @偏移 ↔ 真实字段@同偏移):

| MiniSys 字段(偏移) | 实际读到的真实字段 |
|---|---|
| FileReferenceNumber@16 | **ParentFileReferenceNumber**@16 |
| ParentFileReferenceNumber@24 | **Usn**(记录的 USN 号) |
| TimeChanged@40 | Reason(DWORD)+SourceInfo |
| Reason@48 | SecurityId+FileAttributes |
| FileAttributes@60 | **FileName[0..2)前两个字符** |
| FileNameLength@64 | **FileName[2..4)第 3、4 个字符**(DWORD!) |
| FileName@68 | FileName[4](真实起点 60) |

  真实 V2 布局(winioctl.h):`RecordLength@0, MajorVersion(WORD)@4, MinorVersion(WORD)@6, FileReferenceNumber@8, ParentFileReferenceNumber@16, Usn@24, TimeStamp@32, Reason(DWORD)@40, SourceInfo@44, SecurityId@48, FileAttributes@52, FileNameLength(WORD)@56, FileNameOffset(WORD)@58, FileName@60`。代码版把 Major/Minor 声明成 DWORD、漏掉 SecurityId 与 FileNameOffset。V3 同样全错(真实 FileName@76/FileNameLength@72,代码 72/64)。
- 触发:B-1 修复后首次真实枚举。
- 后果:`n.nameLen = 文件名第3字符|第4字符<<16`(如 "thumbcache" → 'u'|'m'<<16 = 7,076,213 wchar ≈ 14MB 越界 append),每条记录越界读 64KB 缓冲区之外;轻则 names_ 数 GB + 全部索引数据错乱(frn=父 FRN、childrenOf 以 USN 为键、dirPaths_ 为空),重则 AV 崩溃(C++ catch 捕不到)→ minidump。`isDir` 也错(attrs=文件名前两字符,随机命中 0x10 位)。
- 修复草案:删除自声明,直接用 SDK `USN_RECORD_V2/USN_RECORD_V3` + `USN_RECORD_COMMON_HEADER` 判版本,名字用 `FileNameOffset/FileNameLength`(字节)定位。修复后必须用真实卷活体回归(测试目前只喂合成节点,`VolumeIndexTests.cpp:24-29`)。
- 置信度:布局错位 Confirmed(探针输出);崩溃/OOM 后果 Likely(未活体执行)。

### B-3 RefreshFromUsn:READ_USN_JOURNAL 版本字段同样零初始化 + 每次 Finalize 全量重建【Likely / Confirmed(成本)】
- 位置:`VolumeIndex.cpp:332-338`(`READ_USN_JOURNAL_DATA ruj{}` —— 同样是 V1 别名(48 字节,含 Min/MaxMajorVersion WORD),零初始化 ⇒ 0/0 非法 → FSCTL_READ_USN_JOURNAL 大概率同返 87 → `readOk=false` → 全量重建);`:450`(`ApplyChanges` 末尾无条件 `Finalize()`)。
- 另:EnsureBuilt 的"廉价验证"(头文件注释 volume serial + journal ID)实际只廉价在 GetVolumeInformationW;`RefreshFromUsn` 成功路径(即使 0 变更)也会 `Finalize()` —— byFrn_(预留 2n bucket)+childrenOf_+dirPaths_ 全量重建 + DFS 为**每个目录**拼完整小写路径字符串。按 1M 条目估:数秒 CPU + 数十 MB~数百 MB 临时字符串,每扫一次付一次 → "二扫≤5s"目标不可达。
- 修复草案:`ruj.MinMajorVersion=2; ruj.MaxMajorVersion=3; ruj.UsnJournalID=journalId_;`(0 仅在"读当前 journal"语义下合法);`changes.empty()` 时跳过 Finalize;Finalize 增量化(改名/删除只动受影响子树)或改懒构建。
- 置信度:87 复现 Likely(机制同 B-1,未活体验证);Finalize 成本 Confirmed(代码路径确定,量级按 1M 条目推算)。

### B-4 Json 解析器冒号/逗号后不跳空白 → rules.json 从未生效【Confirmed】
- 位置:`MiniSys\src\util\Json.cpp:180-184`(ParseObject 消费 `:` 后直接 ParseValue,无 SkipWs);`:204-206`(ParseArray 消费 `,` 后直接 ParseValue,同样无 SkipWs)。ParseObject 在 `,` 后经循环顶部 SkipWs(OK),ParseArray 首元素经入口 SkipWs(OK)。
- 证据:rules.json 前 16 字节 od 实测 `{ \n · · "version": 2` —— 偏移 14 恰是 `:`(13)后的空格,ParseValue(' ')→ParseNumber→`Fail("bad number at offset 14")`,与日志逐字符吻合(23:48:31.725 等 14 次)。**注意两处都要修**:只修冒号,数组 `}, {` 的逗号+换行会以同样方式失败(修复后需用真实 rules.json 回归)。
- 后果:外置规则热更新(R-005 设计项)完全失效,每次走 `BuiltinRules()` 回退。**实际行为无差异**(已逐条核对 builtin 27 条与 rules.json 27 条等价),影响 = 特性失效 + 每次 Junk 扫描一条 WARN 噪声。
- 修复草案:ParseObject `++pos_;` 后加 `SkipWs();`;ParseArray `,` 分支 `++pos_;` 后加 `SkipWs();`。单测覆盖 `"a": 1`、`[1, 2]`、`[{"x": 1}, {"x": 2}]`。
- 置信度:Confirmed(代码 + 字节级偏移 + 日志三方吻合)。

### B-5 SessionService::results_ 无锁共享:UI 原地排序 vs worker 写【Confirmed(竞争存在)/Likely(触发概率低)】
- 位置:写方 `SessionService.cpp:116`(`results_[tab] = std::move(buffer)`,RunScan 尾)、`:218-227`(RunPlan 尾:逐元素 move-out + `items = std::move(kept)`);读/写方(UI 线程)`ui\Presenters.cpp:90-108` `ApplySortAndRefresh` 经 `MutableResults` 拿引用做 `std::stable_sort` **原地重排**;`MainWindow.cpp:533-538`(OnOpenLocation 读 `items[idx].path`)、`MainWindow.cpp:130`(ExecutePlan 预检 `PlanMatches(plan, Results(tab))`)。`results_` 声明 `SessionService.h:102`,无任何互斥保护。
- 触发条件:①扫描进行中(仅 scan 按钮被禁,`MainWindow.cpp:339`;排序按钮/列头点击仍可用)点排序 —— worker 在扫描结尾一次性 move-assign 同一 vector;②执行进行中(仅 exec/undo/scan 禁用,排序/打开位置可用)点排序或"打开位置" —— worker 在 RunPlan 尾做元素级搬移。两窗口都是毫秒级,但都是标准意义上的数据竞争(UB),堆损坏/崩溃理论可复现。
- 连带:`FolderTreePresenter::OnContextMenu`(Presenters.cpp:293)发起的执行**不禁用任何按钮、不显示进度条**(MainWindow::OnExecute 才做 EnableWindow),执行期间用户在 History 页仍可点 Undo → 与 B-8 的 UpdateStatus 竞争叠加。
- 修复草案(按侵入度递增):最小修 —— UI 侧不再原地排序(Presenter 持 `vector<size_t>` 视图索引排序,`lParam` 存结果索引,与 CollectChecked 兼容);或 `SessionService` 增加 `resultsMu_`,RunScan/RunPlan 写、Results/MutableResults 读写全部加锁(注意 RenderItems 持引用遍历期间不能释放锁,需改为**拷贝快照**渲染)。同时执行期间禁用排序按钮与列头。
- 置信度:竞争存在 Confirmed(双方代码路径都读到);崩溃后果 Likely(概率低但属实 UB)。

### B-6 QuarantineOp:失败无重试、无锁定进程诊断、Win32 5 无解释【Confirmed(代码)/Likely(用户机根因)】
- 位置:`QuarantineOp.cpp:22-29`(`MoveWithRetry` 名为 retry 实为**单次** MoveFileExW,仅 MOVEFILE_WRITE_THROUGH);失败路径 `:84-90` 只记 `errOut = "MoveFileEx failed (Win32 5)"`。与 MoveJunctionOp 形成对照:`MoveJunctionOp.cpp:118-126` 有 `RestartManagerCheck`(RmGetList)预检并报占用进程名,**QuarantineOp 完全没有**。
- 用户机证据:history.jsonl 第 17/20 行 —— npm-cache(1,605,435,521 字节)两次 FAILED "MoveFileEx failed (Win32 5)";同批 cargo/nuget 大目录 rename 同秒成功 → 机制正常、环境差异 → npm-cache 子树内有句柄未按 FILE_SHARE_DELETE 打开(node/npm 常驻、VS Code 扩展宿主、杀软、SearchIndexer 均可能)。属"环境固有 + 应用诊断缺失"叠加。
- 后果:用户看到 Win32 5 无从下手;重试按钮(再次执行)必然再失败(锁定者未退出)。
- 修复草案:
  1. `MoveWithRetry` 真重试:同卷 rename 对 ERROR_ACCESS_DENIED/ERROR_SHARING_VIOLATION 以 100ms×N 退避重试(3-5 次);
  2. 失败且错误为 5/32 时,调用与 MoveJunctionOp 相同的 `RestartManagerCheck(source)`,把占用进程名写进 note 与用户提示("被 node.exe 占用,关闭后重试");
  3. 目录失败时可降级为逐子项隔离(锁定的子项单独失败,其余成功);
  4. 错误码映射表:5→"被占用",32→"共享冲突",1314/1317→权限。UI 文案见跨报告线索。
- 附带:`UniqueTargetFor`(:48-58)check-then-move 有 TOCTOU,但同批串行执行下无实害。
- 置信度:代码缺陷 Confirmed;npm-cache 具体锁定者未活体确认(RmGetList 需在失败当时跑)→ Likely。

### B-7 EmptyQuarantine:无视删除失败全量标记"已释放";隔离区占用显示永不归零;O(N²) 重写【Confirmed】
- 位置:`SessionService.cpp:246-282`。① `:257` `fs::remove_all(root, ec)` 失败仅 `rpt.failed++`,**:265-271 无条件**把**所有** Quarantine/Success 记录改为 `UpdateStatus(id, Success, "[已释放]...")` —— remove_all 部分失败(隔离区内有锁定文件,而这里删的恰是曾被隔离的内容)时,文件还在、记录已宣称永久删除;② `QuarantineUsageText`(:284-296)统计条件仍是 `status==Success`,不识别"已释放"标记 → **清空后状态栏占用数字不变**;③ 标记循环逐条 `UpdateStatus`,每条 = LoadAll(全文件 JSON 解析)+ 全文件原子重写(`OperationLog.cpp:224-240`)→ N 条记录 N 次重写,O(N²);历史积累到千条时清空隔离区 = 千次全文件重写。
- 修复草案:引入 `OpStatus::Purged`(或 note 约定 + UsageText 过滤 `note.find(L"[已释放]")==0`);先 remove_all 收集每卷成功与否,仅对成功卷的记录标记;标记改为一次 LoadAll + 批量改 + 一次 RewriteAtomic(新增 `UpdateStatusBulk`)。
- 置信度:Confirmed(代码全路径读到;用户尚未触发清空,暂无运行时证据)。

### B-8 OperationLog::UpdateStatus 读-改-写在锁外 + 状态栏每 tick 全量 LoadAll【Confirmed】
- 位置:`OperationLog.cpp:224-240` —— `auto all = LoadAll();`(LoadAll 内部锁完即释放)之后才拿 `mu_` 改写 → 与 worker 线程 `Append`(:214-222)之间的窗口内新 Append 的记录会被 RewriteAtomic **覆盖丢失**(丢的是 QUARANTINE 记录时即永久失去 Undo 映射)。触发路径:FolderTree 右键执行期间(Presenters.cpp:293,不锁 UI)用户在 History 页 Undo;或未来任何并发 Update/Append。
- 连带性能:`MainWindow.cpp:160-163,318` —— 每条 WM_APP_SCAN_PROGRESS/OP_PROGRESS 都调 `UpdateStatusBar` → `QuarantineUsageText` → `LoadAll()`(全文件逐行 JSON 解析)。LargeFileScanner 每 1024 个文件 post 一次进度,一次全盘扫描数百次 tick;当前 20 条记录无感,千条记录后每 tick 数十 ms 解析 → UI 卡顿。
- 修复草案:UpdateStatus 把 LoadAll 挪进同一 `lock_guard` 作用域;状态栏占用改为内存缓存(Execute/Undo/Empty 时增量维护计数),或 LoadAll 结果 1s 节流。
- 置信度:Confirmed(代码;丢记录竞态窗口小,实际触发未观测)。

### B-9 DelegateOp:子进程无超时、无 Job Object、不可取消;cleanmgr 交互式挂起【Confirmed(前半)/Likely(挂起)】
- 位置:`DelegateOp.cpp:101-114` —— 管道读循环 + `WaitForSingleObject(pi.hProcess, INFINITE)`,全程不检查 `cancelScan_`;无 `CreateJobObject`/`AssignProcessToJobObject`。
- 后果链:① 选 WinSxS(Advanced)执行 DISM(数分钟~数十分钟)期间 UI 只能干等,点关闭 → `RunMessageLoop` 退出后 `Shutdown()`(MainWindow.cpp:90)join worker → **窗口已销毁但进程驻留直至 DISM 退出**(假死);强杀 MiniSys 则 DISM 成为孤儿进程继续改系统;② `runPlan` 亦不检查 cancelScan_(SessionService.cpp:163 起),cancelScan_ 语义只覆盖 Scanner;③ 管道两端 bInheritHandle=TRUE 且 hStdInput=NULL:`cleanmgr`(windows-old 规则,`JunkRules.cpp:98`)是交互 GUI 程序,CREATE_NO_WINDOW 挡不住其主窗口,用户不关它 MiniSys 永远"执行中";DISM 的子进程(DismHost)若继承写端,ReadFile 在 dism 退出后仍阻塞到孙进程退出(Likely)。
- 修复草案:Job Object(KILL_ON_JOB_CLOSE)包住子进程;WaitForSingleObject 改带超时轮询 `cancel.load()`(超时上限按命令配置,DISM 30min);cleanmgr 改 `cleanmgr /VERYLOWDISK /d C:`(静默)或换 `Dism /Online /Cleanup-Image /StartComponentCleanup` 之外不提供;SessionService 增加可取消执行语义并在 UI 暴露取消(与 R-003 一并修)。
- 置信度:无超时/无 Job/不检查 cancel Confirmed;cleanmgr 交互挂起与孙进程继承句柄挂起 Likely(未活体跑)。

### B-10 撤销(迁移类)在 UI 线程整目录拷贝 → 界面冻结【Confirmed】
- 位置:`Presenters.cpp:337-348`(UndoSelected 在 UI 线程直接调 `svc_.UndoRecord`)→ `SessionService.cpp:298-314` → `MoveJunctionOp::Undo`(:213-238)→ `ShCopyDirectory`(整应用目录 target→source SHFileOperationW)+ 删 target。v1 R-001 在"执行"侧已修(执行入 worker),**撤销侧复发**:撤销一个 5GB 应用 = UI 冻结数分钟,窗口无响应(系统白影)。QuarantineOp::Undo 是 rename(瞬时)不受影响。
- 修复草案:UndoRecord 也走 StartTask 串行队列(TaskKind::Executing),复用现有进度 PostMessage;或 Undo 内先 rename target→source 同卷场景(目标盘→C 盘通常跨卷,rename 不行,只能异步拷贝)。
- 置信度:Confirmed(同步调用链完整读到)。

### B-11 扫描性能链架构:尺寸计算是真正瓶颈,索引救不了 Junk 扫描【Confirmed(机制+测量)/Likely(耗时占比)】
- 位置:`JunkScanner.cpp:257-292`(尺寸池:候选目录粒度,每候选单线程 `DirectorySize`,`PathUtils.cpp:130-164` 单线程 DFS);`PathUtils.cpp` DirectorySize 无任何并行。
- 测量(本机只读,2026-09-30):%TEMP% **256,001** 项;C:\Windows\WinSxS **302,177** 项(Advanced/Delegate 规则,尺寸纯展示);npm-cache 32,244 项;Edge User Data 8,675 项;C:\MiniSys.Quarantine 11,540 项;WU Download 87 项。
- 关键论点:即便 B-1/B-2 修好索引,`ExpandRuleIndexed` 对目录候选仍不带 size(`JunkScanner.cpp:158-168`,注释自认 ADR-004),尺寸仍要 FastWalk/DirectorySize 精扫 —— **WinSxS(302k)与 TEMP(256k)两条最重路径与索引无关**。DESIGN-v2 §11 的"Junk 首扫≤30s(含索引构建+精扫)"与 ADR-004 的"命中子树通常远小于全量"在此机器数据下不成立。
- 队头阻塞确认:池按候选并行(8 线程、~30 候选),墙钟≈最重单候选(TEMP 或 WinSxS 单线程 25-50s,估),其余线程早早空闲。
- 修复草案(按收益):
  1. **跳过无意义尺寸**:Delegate/InfoOnly 项(winsxs/hiberfil/pagefile)不预扫尺寸,显示"—"或"约 N GB"(一条 GetFileAttributesExW 够 hiberfil;WinSxS 可标"需 DISM 清理"),直接省掉 302k 项遍历;
  2. **子树内并行计尺寸**:把 DirectorySize 改为目录队列池(同 FastWalk 模式),TEMP 256k 摊到 8 线程 ≈ 3-8s;
  3. **懒尺寸**:仅对 recommended/被勾选项计算,或列表先出、后台补尺寸;
  4. FolderTreeScanner(见 B-14)与 AppScanner(:127-133 对所有 C 盘应用先整树计尺寸、再过滤 <50MB)复用同一并行原语。
- 置信度:机制 Confirmed;57s 中尺寸池占比 45-55s 为 Likely(日志无分段计时)。

### B-12 MoveJunctionOp::Execute 全树走两遍 DirectorySize【Confirmed】
- 位置:`MoveJunctionOp.cpp:109`(PreflightCheck 内 `DirectorySize(source_)` 算 1.1× 空间校验)+ `:139`(`rec_.sizeBytes = DirectorySize(source_)` 再算一遍)。两次完整 DFS 之间还夹着 RmGetList。多应用批量迁移时每个应用翻倍。
- 修复:preflight 结果复用(`Execute` 直接采用 preflight 算出的 srcSize);或用 GuardRails 已有的 sizeAtScan(PlanItem 携带)替代第二次。
- 置信度:Confirmed。

### B-13 测试进程污染用户真实数据目录(日志被测试写满)【Confirmed】
- 位置:`Logger` 无路径覆盖(`util\Logger.cpp:29-33` 固定 `%LOCALAPPDATA%\MiniSys\logs\minisys.log`);`SessionServiceTests.cpp:55-60` 的 `ThrowingScanner` 抛 "boom" → 用户日志 12 条 `[ERROR] Scanner threw: boom`(21:54-22:45、00:04-00:05 段);`VolumeIndexTests` 非提权跑真实 `EnsureBuilt(L'C')` → 12 条 `cannot open volume C: (Win32 5)`;**主审线索"每次 Instance() 重复迁移"实为测试夹具日志**:12 条 "Migrated 1 legacy TSV records" 全部来自 MiniSysTests.exe 会话(OperationLog 测试用 SetHistoryPathForTesting 指向临时目录,但 Logger 仍写真日志)。真实提权会话(23:48:29 起)**零条** Migrated。
- 后果:真实运维日志被测试噪声淹没(本次审查即需人工剥离);长期膨胀(无轮转,R-010 维持)。
- 修复:测试 main 里给 Logger 加 SetPathForTesting(同 OperationLog 模式);或测试进程用环境变量切换 %LOCALAPPDATA%。
- 置信度:Confirmed(日志时间线 + 测试代码对应)。

### B-14 FolderTreeScanner 全串行 DirectorySize,扫所有固定盘【Confirmed】
- 位置:`FolderTreeScanner.cpp:63-83` —— 逐顶层目录**串行** `DirectorySize`,默认扫全部固定盘(`:22-24`),无排除(C:\Windows、C:\Users 全量)。用户未在日志中触发该 Tab(无耗时记录),按本机数据推算单 C:\Windows 就 >1M 项,串行 DFS 分钟级。优化同 B-11 第 2 条(目录队列池)。
- 置信度:Confirmed(代码);耗时推算 Likely。

### B-15 GuardRails 隔离区保护只硬编码 C/D/E 盘【Confirmed】
- 位置:`GuardRails.cpp:36-38` —— `D:\MiniSys.Quarantine`、`E:\MiniSys.Quarantine` 字面量;F: 及以后的固定盘隔离区不受保护。`QuarantineOp::QuarantineRootFor`(QuarantineOp.cpp:42-46)却对任意盘生成根。后果:FolderTree 页右键 F:\MiniSys.Quarantine\packages → GuardRails 放行 → 同卷 rename 进自身根(packages→packages-2),无实害但产生自我嵌套记录与误导历史。修复:保护名单按 `EnumerateDrives()` 生成,或前缀匹配 `<任意盘>:\MiniSys.Quarantine`。
- 置信度:Confirmed。

### B-16 其他经核实的小项
- **LargeFileScanner 不排除 C:\MiniSys.Quarantine**(`LargeFileScanner.cpp:43-55` 排除表无此项):已隔离的 1.4GB 内容(11,540 项)会在大文件/重复页再次出现,且被推荐为可删对象 —— 与"隔离区由专门流程管理"的语义冲突(跨 UI 报告印证)。同样不排除 pagefile.sys/hiberfil.sys/swapfile.sys(根下文件会进 top-N,属可接受展示)。Confirmed(排除表读到)。
- **R-007 复核(onFile 全局锁限制并行)**:实际上**不是**当前瓶颈 —— `LargeFileScanner.cpp:147` 的 `minBytes` 预过滤先于锁,默认 100MB 门槛下进锁的条目极少;ARCHITECTURE.md 风险表的此项评价应降级(若用户把阈值设 0 则成立)。
- **SessionService::StartTask**(`SessionService.cpp:64-86`):CAS 成功后若 `std::thread` 构造抛异常(resource exhaustion),taskKind_ 残留非 0 → 服务永久 busy 且无人复位。加 try/catch 复位即可。Likely(异常路径未验证)。
- **FastWalk 池复核**:Worker 终止分支在持锁状态下 notify_all(FastWalk.cpp:93-97),`inflight_--` 虽在锁外(107),推演后**无丢唤醒死锁**(终止通知必然在后来者入睡后到达);取消粒度逐文件,异常路径 onFile 抛出会 std::terminate(Pool 线程无 catch)——低风险。已列入"验证无问题"。
- **AppScanner**(:127-133):对每个 C 盘应用先整树 DirectorySize 再按 50MB 过滤,小应用也白扫;`progress(done++, ...)` 在 progress 为空时不递增(仅计数瑕疵)。Confirmed。
- **USN_JOURNAL_DATA_V1 用于 FSCTL_QUERY_USN_JOURNAL**(VolumeIndex.cpp:217):V0/V1 前 56 字节同构,jd.UsnJournalID/NextUsn 读取正确 —— 无问题(探针 sizeof=64 兼容)。

---

## 三、扫描耗时分布推算表

依据:日志 23:49:29(tab0, 30 项, 57,371ms)、23:55:38(tab1, 244 项, 69,181ms);本机实测目录规模;代码路径。"占比"为估算(Likely)。

### Junk 扫描 57.4s(tab=0,索引失败路径)

| 环节 | 代码路径 | 规模依据 | 估算耗时 | 占比 |
|---|---|---|---|---|
| rules 加载(失败+WARN) | JunkRules::Load | 文件 6KB | <10ms | ~0% |
| EnsureBuilt→BuildFull 失败 | VolumeIndex.cpp:199-246 | 开卷+QUERY 成功+ENUM 87 | ~10ms(日志 .725→.727) | ~0% |
| 规则展开(约 40 次 GetFileAttributesExW/FindFirst) | ExpandRule | 27 规则×少量 | 0.3-1s | ~1% |
| **DirectorySize 池(8 线程,~30 候选)** | JunkScanner.cpp:257-292 | TEMP 256k + WinSxS 302k + npm 32k 并行,墙钟≈最重一条单线程 DFS | **45-55s** | **~85%** |
| QueryRecycleBin(全卷) | PathUtils.cpp:100-107 | 回收站小 | 0.1-1s | ~1% |
| 项构造+输出 | JunkScanner.cpp:295-350 | 30 项 | <10ms | ~0% |

注:TEMP 与 WinSxS 分属不同线程并行,二者各估 25-50s,墙钟取 max 加尾部。

### LargeFiles 扫描 69.2s(tab=1)

| 环节 | 代码路径 | 规模依据 | 估算耗时 | 占比 |
|---|---|---|---|---|
| FastWalk 全 C:(8 线程,排除 Windows/PF/PD/SVI 等) | LargeFileScanner.cpp:166 | Users(TEMP 256k+npm 32k+隔离区 11.5k 等)+其余,估 35-60 万项 | 45-60s | ~75% |
| partial_sort topN=200 + 输出 | :169-186 | 244 项 | <50ms | ~0% |
| 去重:同 size 分组 | :188-201 | 少 | <10ms | ~0% |
| 头 64KB 哈希 + 全量哈希(≥100MB 文件) | :204-233 | 等大文件若干 GB 读取 | 5-15s | ~15% |

### 修复后预期(Likely,均需活体基准校准)

| 措施 | Junk 首扫 | Junk 二扫 | 说明 |
|---|---|---|---|
| 仅修 B-1/B-2/B-3(索引可用) | ~50s(几乎不变) | ~45-50s | 展开销省了,尺寸遍历还在;Finalize 每扫一次 |
| + B-11-1(跳过 WinSxS/InfoOnly 尺寸) | ~30-40s | ~30-40s | 省 302k 项单线程 |
| + B-11-2(子树并行计尺寸) | **~10-20s** | ~8-15s | 达成 ≤30s 目标 |
| + B-11-3(懒尺寸,仅勾选项) | **2-5s(先出列表)** | 同左 | 语义变化需 UI 配合 |
| LargeFiles | 69s → 60-65s | — | 只能靠排除隔离区/TEMP 类小修;本质受 ADR-004(无 $MFT)限制 |

---

## 四、修复顺序建议

**P0(缺陷,合入前必修)**
1. B-1 + B-2 + B-3 绑定修(同一 PR):MFT_ENUM_DATA_V1 显式 Min=2/Max=3;删除自声明 USN 结构体改用 SDK USN_RECORD_V2/V3;READ_USN_JOURNAL 版本字段;`changes.empty()` 跳过 Finalize。**必须附管理员活体冒烟**(真实卷 EnsureBuilt + 条目数与 GetFileAttributesExW 抽样比对),现有合成测试对这条链零覆盖。
2. B-4 Json 双 SkipWs(冒号+逗号)—— 一行级修复,恢复 rules.json 热更新。
3. B-5 results_ 数据竞争(UI 排序视图化或加锁快照)。

**P1(用户可感性能/可靠性)**
4. B-11:跳过 Delegate/InfoOnly 尺寸 + DirectorySize 目录队列池化(Junk/FolderTree/AppScanner 共用)。
5. B-6 QuarantineOp 失败诊断(RM 检测 + 重试 + 错误映射)。
6. B-9 DelegateOp Job Object + 超时 + 取消语义(R-003 一并)。
7. B-10 撤销入后台队列。

**P2(劣化趋势与卫生)**
8. B-7/B-8 OperationLog:批量 UpdateStatus、锁内读改写、UsageText 缓存。
9. B-13 测试日志路径隔离;B-12 迁移双遍尺寸;B-15 保护名单动态化;B-16 隔离区排除。

---

## 五、跨报告线索(需其他角色印证)

- **UI 角色**:①执行中排序/打开位置按钮未禁用(B-5 的触发面);②FolderTree 右键执行无进度条、无按钮禁用(Presenters.cpp:293);③EmptyQuarantine 后状态栏占用不归零(B-7)的呈现;④清空隔离区的确认弹窗引用了错误的 usage 数字(同一 bug);⑤cleanmgr 委派项的用户预期管理(B-9)。
- **安全角色**:①**DelegateOp 的 command 来自 rules.json 且原样 CreateProcess**(`DelegateOp.cpp:81-86`)——rules.json 位于 exe 同目录(build\Release,当前用户可写),提权进程执行用户可改的命令字符串 = 本地提权面;修复 B-4 让外置规则真正生效后该面**从理论变为现实**,需命令白名单/签名校验;②B-15 保护名单缺口;③B-16 隔离区文件在大文件页可再次被"删除"(二次操作同一数据)。
- **测试/CI 角色**:B-13(测试污染真实 %LOCALAPPDATA%);VolumeIndex 真实卷零覆盖(B-1 存活 54/54 全绿的原因);`MiniSysTests.exe` 也在 build\Release 与主程序同目录产出。

---

## 六、被推翻的假设 / 已验证无问题

**被推翻/修正的假设:**
1. **"Win11 需要 MFT_ENUM_DATA_V1(Min=2/Max=3)才能枚举"——根因修正**:不是"代码用了 V0 而 Win11 要 V1",而是代码**以为**在传 V0 的 24 字节,实际因 SDK typedef 传了 32 字节零值 V1。主审的结论方向正确,本报告以 SDK 头文件 + 编译探针(32≠24)坐实机制。(传正确填充的 V1 是修复;传真 V0 是否也可行未验证。)
2. **"MigrateLegacyIfNeeded 每次调用都重复迁移"——不成立(对真实使用)**:迁移后 tsv 被改名 .bak,后续 `FileExists(tsv)` 早退;真实提权会话(23:48)日志零条 Migrated。日志中 12 次重复全部来自 **MiniSysTests.exe 会话**(其 OperationLog 测试用临时 history 路径,但 Logger 写真日志)。真正的缺陷是 B-13 测试污染,不是迁移不幂等。(保留一个理论窗口:若 tsv→bak 改名失败会重复尝试写 jsonl,但 jsonl 已存在时不重写、无重复记录。)
3. **"rules.json 失效导致清理行为错误"——影响有限**:内置回退表与 rules.json 逐条等价(27 条,模式/年龄/策略一致),行为零差异;影响=热更新特性死 + WARN 噪声。
4. **R-007"onFile 全局锁限制遍历并行"——当前配置下非瓶颈**:minBytes(默认 100MB)预过滤先于 entriesMu,进锁条目极少。
5. **"npm-cache 失败可能是 ACL/跨卷问题"——排除**:同批同机制大目录(cargo/nuget)rename 同秒成功;npm-cache 与隔离区同在 C: 卷。最可能为树内句柄(B-6)。

**已验证无问题(后人勿重复排查):**
- FastWalk 线程池终止逻辑(锁外 `inflight_--` + notify_all 的丢唤醒风险):终止分支持锁 notify,推演无死锁;取消逐文件粒度正常。
- USN_JOURNAL_DATA_V1 用于 FSCTL_QUERY_USN_JOURNAL:V0/V1 前 56 字节同构,journalId/NextUsn 读取正确。
- PlanBuilder planHash / PlanMatches:worker 内二次校验 + UI 预检的时序(总线程序列 taskKind_ seq_cst)正确。
- StartTask busy 拒绝与 join 时序(单 UI 调用方)正确;仅极端 std::thread 构造异常会残留 busy(B-16 小项)。
- QuarantineOp 命名冲突策略(-2/-3)与 Undo 冲突策略(.restored)逻辑正确。
- LargeFileScanner 三级去重(同 size→头 64KB→全量)逻辑与哈希失败跳过处理正确。
- MoveJunctionOp 四步预检 + 回滚 + 自检链完整(Restart Manager 已用,正是 QuarantineOp 该抄的作业)。

---

## 七、未验证 / 局限

1. **无活体 FSCTL 验证**:审查 shell 未提权,无法打开 `\\.\C:`。B-1 的 87 成因(代码+SDK+探针+日志四方吻合)视为 Confirmed,但 **"Min=2/Max=3 修复后枚举成功"、B-3 的 READ 87 复现、B-2 的崩溃后果**均未实机运行,标 Likely;修复 PR 必须补管理员冒烟。建议后续在本仓库 .tmp-review\03-exp\ 下补 console 探针(结构已在 layout_probe.cpp 中验证一半)。
2. **耗时占比为推算**:日志无分段计时(仅总量 57,371/69,181ms),目录规模为实测,单条 DirectorySize 耗时按每项 0.05-0.2ms 冷缓存经验值估;修复后预期数字需以 DESIGN-v2 §11 的基准点(`Scan tab=%d done`)实测校准。
3. **npm-cache 锁定者未指认**:需在失败当时跑 RmGetList(修复后诊断路径);本报告只能给出机制级归因。
4. **用户 23:48 会话实际运行的 exe 已被 00:05 构建覆盖**,无法取哈希;但 backend 代码与 ae3ce19 及工作树一致(git diff 空),且日志行为与代码缺陷一一对应,版本归属按"v2.0 后端代码"论。
5. **FolderTree / Apps Tab 无用户耗时日志**,B-14 与 AppScanner 成本按代码+目录规模推算。
6. 未审查:Junction.cpp 的字节级 reparse 布局、CrashDump、资源/布局(UI 角色范围);rules.json 与 builtin 的逐字段等价性为人工比对(27 条全对上),未做程序化 diff。

**实验产物**:`C:\tjf\github\MiniSys\.tmp-review\03-exp\layout_probe.cpp`(SDK/自声明结构体偏移探针,MSVC 编译运行,输出见本报告 B-1/B-2 引用)、`build_probe.bat`、`layout_probe.exe`。对用户真实数据(%LOCALAPPDATA%\MiniSys、各缓存目录、C:\MiniSys.Quarantine)仅做只读列举/统计,未写入。
