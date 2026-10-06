# MiniSys 评审分报告 05 — (B) 文件操作危险门禁 & (C) 配置与外部 IO

> 审阅对象:C:\tjf\github\MiniSys,HEAD = `ae3ce19`(2026-09-29 23:15,"feat: v2.0 per DESIGN-v2")。core/util/platform 层与该提交一致;MainWindow.cpp / Presenters.cpp 等含未提交的 v2.1 UI 改动(对话降噪、图标),涉及 UI 行为的结论以工作区代码为准(已标注)。
> 方法:将**真实源码**(GuardRails/QuarantineOp/OperationLog/JunkRules/JunkScanner/VolumeIndex/PlanBuilder/DelegateOp/Json/StringUtils/PathUtils/Hash .cpp)编入对抗驱动 `C:\tjf\github\MiniSys\.tmp-review\exp\guard_probe.exe`,对系统路径**只读**探测(Validate/IsProtectedPath/GetFileAttributesExW/UniqueTargetFor);MoveFileExW 仅在 `.tmp-review\exp\sandbox` 沙箱执行(与 `QuarantineOp::MoveWithRetry` 逐字符相同的调用表达式);从未调用 `QuarantineOp::Execute`,从未触碰 `C:\Windows` 等系统路径的写操作,未启动 MiniSys.exe。用户真实 `%LOCALAPPDATA%\MiniSys` 只读查看。
> 每条发现给出 file:line / 复现输入 / 后果 / 修复草案 / 置信度。Conf=Confirmed(读到代码且有实验佐证)、Likely(机制确凿、推算)、Spec。

---

## 0. 摘要(Top 风险,按处置优先级)

| # | 风险 | 置信度 | 一句话 |
|---|---|---|---|
| 1 | **T-C1** rules.json 解析器不容空格:随包分发的 rules.json 从未生效,静默回退内置表,用户毫无感知(用户日志 "bad number at offset 14" 逐字节复现,offset 14 即 `"version": 2` 冒号后的空格) | Conf | 外置规则特性自 ae3ce19 起完全失效 |
| 2 | **T-B1** GuardRails 前缀匹配可被 5 类路径变体绕过(双分隔符、无盘符根路径、`\\?\` 前缀、UNC、8.3 短名),且已用真实 rules.json→JunkScanner→PlanBuilder→Validate→QuarantineOp 自检**全链路实证**:构造的规则让 System32 以 "Safe/默认勾选" 出现在垃圾列表,所有闸门全绿,只剩 MoveFileExW(该语法已证可移动) | Conf | "系统目录一律拒绝" 的产品承诺可被攻破 |
| 3 | **T-B2** 黑名单太窄:C:\Users、C:\Program Files、C:\ProgramData 等不在名单,文件夹树右键可直接隔离;而 UI 文案宣称"系统目录受保护名单拦截" | Conf | 无需 rules.json 即可触达 |
| 4 | **T-C4** VolumeIndex 索引在真实管理员环境恒失败(FSCTL_ENUM_USN_DATA Win32 87),永久回退 FastWalk(57s 扫描);回退路径恰是 T-B1 变体存活的路径 | Likely | 性能+安全的双重退化 |
| 5 | **T-B7** QuarantineOp 无占用预检、无重试、无错误归因 —— 用户 npm-cache 1.6GB 两次失败的 "MoveFileEx failed (Win32 5)" 已在沙箱精确复现(目录内有打开句柄的文件→Win32 5) | Conf | 用户核心痛点 |
| 6 | **T-B9** 内置 Cautious 规则 6 条(windows-temp/wu-download/cbs-logs/dism-logs/minidump/font-cache)全在 C:\Windows 下,执行时被 GuardRails 一律拒绝 —— 规则表承诺了闸门永远不给的清理 | Conf | "扫出来却执行不了" 的直接来源 |
| 7 | **T-C2a** JSON 解析器无深度限制,10k 个 `[[[[` 即栈溢出崩溃(0xC00000FD) | Conf | 损坏 rules.json = 启动崩溃循环 |

用户四条诉求的映射:"不知道能不能执行"→T-B9/T-C3(静默回退);"删除是不是风险分析提示"→确认弹窗只有 N 项+总大小+泛化 dangerous 警告,无逐项影响说明;"被删除的文件是不是系统问题"→无风险归因文案;"失败无原因分析"→T-B7。

---

## 1. (B) 危险操作门禁发现清单

### T-B1【Confirmed】IsUnderProtected 裸字符串前缀匹配被路径变体绕过(全链路)
- 位置:`MiniSys\src\core\GuardRails.cpp:43-56`(`IsUnderProtected`:对 `ToLower(p.wstring())` 与黑名单前缀做 `compare`),名单在 `:22-41`。
- 机制:不做规范化(无 `GetFinalPathNameByW`/短名展开/分隔符归一),`\\?\` 前缀、双反斜杠、无盘符根路径、UNC、8.3 短名都不与 `c:\windows` 等前缀匹配。
- 复现(实验驱动 `guard_probe.exe guard|chain|rules`,全部真实代码):
  - `GuardRails::IsProtectedPath` 对 `C:\\Windows\System32\cmd.exe`(盘符后双反斜杠)、`\Windows\System32\cmd.exe`(无盘符)、`\\?\C:\Windows\System32\cmd.exe`、`\\localhost\C$\Windows\System32\cmd.exe`、`\\?\UNC\localhost\C$\...`、`C:\PROGRA~1\WI7DB9~1`(本机真实短名)、`C:\PROGRA~3\MICROS~1` 全部返回 **false**;`GuardRails::Validate`(喂入扫描器同源的 size/mtime)全部 **allow**。
  - 沙箱 MoveFileExW(=MoveWithRetry 同式):双分隔符、无盘符根路径、`\\?\` 前缀均可**成功移动**。
  - **端到端**(EXP-4):构造单行无空格 rules.json(rule path 分别为上述变体,riskLevel=safe)→ `JunkRules::Load` 加载 7 条 → `JunkScanner::Scan` 把 **System32(实测 10.4GB)**、`\\?\` 形式 System32、`C:\PROGRA~3\MICROS~1`(9.3GB ProgramData\Microsoft)列入垃圾列表(recommended=true → UI 预勾选)→ `PlanBuilder::Build` 通过 → `GuardRails::Validate` **allow** → `QuarantineOp::UniqueTargetFor` 返回 `C:\MiniSys.Quarantine\System32`;QuarantineOp::Execute 的两道自检(存在性 + IsProtectedPath)对同样字符串同样放行(QuarantineOp.cpp:63-76)。唯一未执行的 MoveFileExW 步骤已由沙箱实验单独证明该语法可用。
  - 前提:rules.json 需为**单行、冒号/逗号后零空白**格式(T-C1 修复前,常规手写文件反而解析失败;修复后任意手写格式都可用)。
- 后果:管理员进程一条 rename 即可把真实 System32 / ProgramData\Microsoft 挪进隔离区,系统即刻损坏(可逆性存疑——Undo 记录存在,但系统盘已不可用)。
- 修复草案:
  1. 规范化后再比对:`GetFinalPathNameByW`(解析短名、`..`、变体分隔符)或 `RtlGetFullPathNameExW` + `GetLongPathNameW`,对规范化结果做前缀匹配;拒绝无法规范化的路径(deny-by-default)。
  2. 规则加载期校验(T-B3)与运行期规范化双保险。
  3. `LongPath()` 已意外挡掉正斜杠/`..`/尾点(因 `\\?\` 禁用归一化),不能依赖这种"巧合防御"。
- 置信度:Confirmed(三组独立实验 + 代码)。

### T-B2【Confirmed】保护黑名单覆盖不足,UI 文案过度承诺
- 位置:`GuardRails.cpp:22-41`(仅 Windows、WindowsApps、ProgramData\Microsoft、$Recycle.Bin、SVI、$WinREAgent、Recovery、3×MiniSys.Quarantine);`FolderTreeScanner.cpp:28-83`(枚举各盘**顶层目录**含 Users/Program Files/ProgramData);`Presenters.cpp:251-306`(右键"删除文件夹…"→单条 PlanBuilder→ExecutePlan);`MainWindow.cpp:283`(工作区 v2.1)文案"系统目录受保护名单拦截"。
- 实验:`Validate` 对 `C:\Users`、`C:\Program Files`、`C:\Program Files (x86)`、`C:\ProgramData`、`C:\PerfLogs` 全部 **allow**(实验输出见 §2 附录)。
- 触发条件:文件夹树页右键这些顶层目录 → 确认弹窗 → 隔离。无需 rules.json。另注意 FolderTreeScanner 不填 `lastWriteFiletime`(见 T-B5),mtime 复验也不设防。
- 后果:`C:\Users` 被 rename 后所有用户配置失效;`C:\Program Files` 同理(若句柄允许)。
- 修复:黑名单至少补 Users、Program Files×2、ProgramData;结构性白名单方案见 §5。
- 置信度:Confirmed。

### T-B3【Confirmed】rules.json 是零校验的攻击面(路径+策略+命令三合一)
- 位置:`JunkRules.cpp:124-153`(`ParseRulesJson` 仅检查 id/path 非空,不校验路径位置、不限制 riskLevel/strategy 组合);`JunkScanner.cpp:296-331`(riskLevel→recommended=true→UI 预勾选;command 原样进入 ScanItem);`SessionService.cpp:178-186`(strategy=Delegate 分支直接把 command 交给 DelegateOp,见 T-B4)。
- 实验:EXP-4 的 7 条恶意规则全部加载;`evil-*` riskLevel=safe → recommended=true。
- 后果:(a) 路径变体→T-B1;(b) plain `C:\Users` 路径→T-B2(即使索引修好,`TryGetEntry("c:\users")` 命中,照样列出);(c) command→任意命令管理员执行。
- 风险定性:rules.json 与 exe 同目录同 ACL,攻击者能写它就能换 exe——**不是提权边界突破**;但产品把"用户可编辑规则表"当特性(DESIGN-v2 §10"可热更新"),误改/下载替换/供应链都是现实输入,且当前解析器 bug 造成荒谬倒挂:诚实编辑永不生效,精心构造的恶意文件反而能生效。
- 修复:规则加载期校验——(1) 展开后的 path 必须 `GetFinalPathNameByW` 规范化且落在允许根(%TEMP%、%LOCALAPPDATA%、%USERPROFILE% 显式子树、%ProgramData% 非 Microsoft 子树)之内,否则拒绝该条并**可见告警**;(2) strategy=delegate 的 command 必须命中内置白名单(DISM/powercfg/cleanmgr 的**绝对路径**+参数前缀),其他一律拒绝;(3) riskLevel=advanced/info 的规则禁止 path 指向用户树(降级告警)。
- 置信度:Confirmed。

### T-B4【Confirmed,代码级】DelegateOp:无命令白名单、无超时、无 Job Object、相对 exe 走 CWD/PATH
- 位置:`DelegateOp.cpp:85-114`(`CreateProcessW` bInheritHandles=TRUE、CREATE_NO_WINDOW、`WaitForSingleObject(pi.hProcess, INFINITE)`);`SessionService.cpp:178-186`(command 仅来自规则表)。
- 后果:(a) rules.json command 任意命令以管理员运行(配合 T-B3);(b) `Dism.exe` 这类相对名经 CWD/PATH 解析,CWD 或 PATH 前部投放同名 exe 即劫持;(c) 命令挂起(如交互式 cleanmgr)→ INFINITE 等待 → SessionService 串行队列永久占用,且 RunPlan 循环不检查 `cancelScan_`(SessionService.cpp:163-215),UI 无取消入口;(d) 无 Job Object,主进程退出后子进程残留。
- 修复:白名单+绝对路径(并入 T-B3)、`WaitForSingleObject` 超时+可取消、`CreateJobObject` JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE、`UpdateProcThreadAttribute` 限定句柄继承集。
- 置信度:Confirmed(行为读码;未实际运行挂起命令,测试套件已覆盖正常/失败退出码)。

### T-B5【Confirmed】TOCTOU 复验在 3/5 输入通道上静默失效,且对 Temp 类目录高频误拦
- 位置:`GuardRails.cpp:142,149`(`pi.sizeAtScan != 0` / `pi.lastWriteAtScan != 0` 条件);对照各扫描器:`JunkScanner.cpp:317`、`LargeFileScanner.cpp:181`(top-N)填 mtime,而 **LargeFileScanner.cpp:249-260(去重行)、FolderTreeScanner.cpp:74-81、AppScanner.cpp:135-145 均不填** `lastWriteFiletime`。
- 后果:文件夹树右键、应用迁移、去重删除三通道的 mtime 复验静默关闭(去重行还有 size 兜底;前两者连 size 对目录也不比对)。
- 反向问题(实验观察):EXP-4 对照规则 `%TEMP%` 在 Validate 被拒,原因为"扫描后文件已被修改"——Temp 正是后台进程高频增删直接子项的目录,**最该清的目标最容易被误拦**,且拒绝理由对用户不可理解。建议目录类 mtime 复验放宽为"子项数/代表性文件 mtime"或对 Safe 级规则降级为提示。
- 置信度:Confirmed。

### T-B6【Confirmed,代码级】Undo 路径完全绕过 GuardRails
- 位置:`SessionService.cpp:298-314`(`UndoRecord` 直接构造 `MoveJunctionOp`;`QuarantineOp::UndoPaths`);`QuarantineOp.cpp:102-126`(还原目标不查保护路径,仅冲突时 `.restored`);`MoveJunctionOp.cpp:213-238`(删 junction→拷回→删 target,无预检)。
- 后果:history.jsonl 为用户可编辑文件,被改写的记录(source/target 任意)可让"撤销"把任意路径从隔离区/目标位恢复到任意位置、或删除任意 junction。同 ACL 前提下非提权,但属于"信任本地文件即任意文件操作"的面;正常流程内 Undo 也无任何闸(设计上可接受,但至少应复验 source 不在保护名单内、target 存在性)。
- 修复:UndoPaths 恢复前调用 `GuardRails::IsProtectedPath(restoreTo)`(白名单式豁免:原记录本身来自合法隔离);MoveJunctionOp::Undo 恢复 preflight 子集(protected/target-exists)。
- 置信度:Confirmed(代码),实际越权利用未演示(标注为设计面)。

### T-B7【Confirmed】QuarantineOp 无占用预检/无重试/无错误归因 —— 用户 Win32 5 失败的根因与修复
- 位置:`QuarantineOp.cpp:22-29`(`MoveWithRetry` **没有任何重试**,单次 MoveFileExW+MOVEFILE_WRITE_THROUGH);`:60-96`(Execute 只有存在性+IsProtectedPath 两道自检);对照 `MoveJunctionOp.cpp:118-126` 有 RmGetList 占用预检而 QuarantineOp 没有。
- 实验(沙箱,EXP-3 LOCK 段):文件自身被以无 FILE_SHARE_DELETE 打开 → 移动报 **Win32 32**;**目录内有打开文件时移动目录 → Win32 5**;目录只读属性 → 照常移动。即用户日志 `npm-cache 1.6GB "MoveFileEx failed (Win32 5)"` 的机制 = 目录内存在被占用文件(运行中的 node/npm、杀毒、索引器),与权限/损坏无关。
- 后果:错误提示无归因、无建议、无重试;用户两次尝试两次失败,1.6GB 无法释放——直接对应评审核心诉求"失败提示无原因分析与建议"。
- 修复:(1) Execute 前对目录做 RmGetList(或 `NtQueryInformationFile` 逐项探测)给出"被进程 X 占用"名单;(2) Win32 5/32 → 归因文案"文件正被程序占用(常见:node/npm/杀毒/搜索索引),关闭后重试;或重启后立即执行";(3) 指数退避重试 2-3 次;(4) 失败项在报告中给出"部分子项可先清理"的降级路径(children 模式逐项隔离)。
- 置信度:Confirmed(实验复现错误码;占用进程名归因未在真实 npm-cache 上验证)。

### T-B8【Confirmed】DESIGN-v2 不变量"所有 Operation 构造经 PlanBuilder+GuardRails"的正向执行成立
- grep 全部构造点:`SessionService.cpp:175`(EmptyRecycleOp,$RECYCLE.BIN 特判)、`:183`(DelegateOp)、`:198`(MoveJunctionOp)、`:202`(QuarantineOp)、`:311`(MoveJunctionOp,Undo)。MainWindow/Presenters **不再直接 new Operation**(工作区 v2.1 亦然)。正向文件操作均先过 `GuardRails::Validate`(SessionService.cpp:188-194)。
- 例外(与设计意图一致但需明示):$RECYCLE.BIN 与 Delegate 两分支**先于** GuardRails 文件校验路由(非文件操作);Undo 路径见 T-B6。
- 置信度:Confirmed(grep+读码)。

### T-B9【Confirmed】规则表与闸门自相矛盾:6 条内置 Cautious 规则永远执行不了
- 位置:`JunkRules.cpp:38-47,71-82`(windows-temp/font-cache/wu-download/cbs-logs/dism-logs/minidump 全在 `%SystemRoot%` 下)+ `GuardRails.cpp:25`(C:\Windows 整树保护)。
- 实验(EXP-4 内置表运行):WU Download(463MB)、CBS 日志(15MB)、DISM 日志被列出,size 已算,用户勾选执行 → `IsProtectedPath=1, Validate=deny "受保护系统路径"`,全数跳过。WinSxS 同样被拒(但它有 delegate 出路,由 Advanced 分支先路由,不受影响)。
- 后果:扫描-确认-全跳过的挫败循环;用户看到的"可清理项"实际是永久不可执行项。这正是"UI 上的按钮都不知道能不能执行"的机制性来源之一。
- 修复:这 6 条改为 delegate/age 策略或从规则表移除;或 GuardRails 增加按 ruleId 的细粒度豁免(明确列出可清理的系统子树,如 `%SystemRoot%\Temp`);至少在列表项上标注"将被安全闸拒绝"。
- 置信度:Confirmed。

### T-B10【Confirmed,代码级】清空隔离区在部分失败时让历史记录说谎
- 位置:`SessionService.cpp:246-282`。`fs::remove_all` 失败只 `rpt.failed++`,但字节照计入 `freedBytes`;随后把**所有** Success 的 QUARANTINE 记录一律改写为"[已释放] 隔离区已清空,文件已永久删除"(不论哪个盘失败),已隔离文件实际还在。
- 后果:用户看到"已释放 X"与"文件已永久删除",但磁盘上数据仍在;历史作为隔离区 manifest 的可信度受损。
- 修复:按盘判定;失败盘的记录保持 Success 并注明"清空失败";freedBytes 只累计成功盘。
- 置信度:Confirmed。

### T-B11【Confirmed,实验】`\\?\` 前缀(LongPath)意外构成对 3 类变体的"巧合防御",不可依赖
- 实验:正斜杠 `C:/Windows/System32`(GetFileAttributesExW/MoveFileExW 经 LongPath 加 `\\?\` 后报 123/属性失败)、`..` 路径、尾点/尾空格组件(报 3)、盘符相对 `C:xxx`(报 2,`\\?\` 不解析 CWD)——这四类在**存在性检查**即死,进不了列表也移不动。这是 `\\?\` 禁用路径归一化的副作用,不是设计的安全属性;一旦有人"修复" LongPath 或改用非 LongPath API,这层防御即消失。
- 置信度:Confirmed。

---

## 2. 门禁绕过统计与对抗样本语料

### 2.1 分类汇总(guard_probe.exe guard / chain / rules 实测)

| 变体类 | 样例 | IsProtectedPath | 存在性检查 | Validate | MoveFileExW(沙箱同式) | 判定 |
|---|---|---|---|---|---|---|
| 基线(应拦) | `C:\Windows\System32\cmd.exe` | ✓拦 | 过 | deny | — | 正确 |
| 大小写 | `c:\windows\...` / 大写 | ✓拦 | 过 | deny | — | 正确 |
| 正斜杠 | `C:/Windows/System32` | ✗不拦 | **死**(123) | deny | 死 | 被 LongPath 巧合挡住(T-B11) |
| 双分隔符 | `C:\\Windows\System32` | ✗ | 过 | **allow** | **可移** | **全链路绕过** |
| 无盘符根路径 | `\Windows\System32` | ✗ | 过 | **allow** | **可移** | **全链路绕过** |
| `\\?\` 前缀 | `\\?\C:\Windows\System32` | ✗ | 过 | **allow** | **可移** | **全链路绕过** |
| UNC | `\\localhost\C$\Windows\System32`、`\\?\UNC\...` | ✗ | 过 | **allow** | 未测(需提权;同卷 C$) | 字符串级绕过 Confirmed |
| 8.3 短名 | `C:\PROGRA~1\WI7DB9~1`、`C:\PROGRA~3\MICROS~1`(本机真实短名) | ✗ | 过 | **allow** | 同语法已证可移 | **全链路绕过** |
| 尾点组件 | `C:\Windows.\System32` | ✗ | **死**(3) | deny | 死 | 巧合挡住 |
| `..` 前缀 | `C:\Users\..\Windows\System32` | ✗ | **死**(3) | deny | 死 | 巧合挡住 |
| 尾点/尾空格/ADS 后缀 | `...cmd.exe.` / ` .` / `:ads` | ✓拦(前缀) | 死 | deny | — | 正确(拦于前缀) |
| 盘符相对 | `C:Windows\System32` | ✗ | **死**(2) | deny | 死 | 巧合挡住(`\\?\` 不解析 CWD) |
| CWD 相对 | `Windows\System32` | ✗ | 死(CWD=exe 目录) | deny | — | 取决于 CWD |
| 驱动器号 `\\.\` | `\\.\C:\Windows\...` | ✗ | 死 | deny | — | 无效语法 |
| 名单外系统目录 | `C:\Users`、`C:\Program Files`、`C:\ProgramData`、`C:\PerfLogs` | ✗ | 过 | **allow** | 可移 | **T-B2 设计缺口** |
| 白名单健全性 | %TEMP% 文件、npm-cache、pip\cache | ✗ | 过 | allow | 可移 | 正确放行 |

**统计:19 类应拦变体中,5 类全链路绕过(双分隔符/无盘符/\\?\/UNC/8.3),1 类字符串级绕过(UNC 移动层未测),4 类被 LongPath 副作用侥幸拦截,名单外目录 4 例直接放行;无误拦(overblock)样本。**

### 2.2 语料附录(节选自实验输出)

| 输入 | 当前判定 | 应判 |
|---|---|---|
| `C:\\Windows\System32`(规则 path) | 列出+预勾选+Validate allow+可隔离 | 拒绝 |
| `\Windows\System32` | 同上 | 拒绝 |
| `\\?\C:\Windows\System32` | 同上 | 拒绝 |
| `C:\PROGRA~3\MICROS~1` | 同上(9.3GB) | 拒绝 |
| `C:/Windows/System32` | 不列出(attr 失败) | 拒绝(现侥幸) |
| `C:\Users`(文件夹树右键) | Validate allow | 至少二次危险确认/拒绝 |
| `C:\Program Files`(同上) | Validate allow | 拒绝 |
| `C:\WINDOWS\SoftwareDistribution\Download`(内置规则) | 列出但执行被拒 | 规则层就不该以可执行策略列出 |
| `%TEMP%`(活跃目录) | Validate deny"扫描后文件已被修改" | 放行或给出可理解理由 |

---

## 3. 「输入通道 × 是否走门禁」表

| 输入通道 | PlanBuilder | planHash | GuardRails::Validate | QuarantineOp 自检 | 其他闸 |
|---|---|---|---|---|---|
| 垃圾清理勾选(Quarantine 项) | ✓ | ✓(整单过期即中止) | ✓ | ✓(存在+IsProtectedPath) | 确认弹窗(dangerous 泛化警告) |
| 垃圾清理勾选($RECYCLE.BIN 项) | ✓ | ✓ | **✗**(SessionService.cpp:173 特判直通 EmptyRecycleOp) | ✗ | 不可逆确认文案 |
| 垃圾清理勾选(Delegate 项) | ✓ | ✓ | **✗**(非文件操作;command 来自 rules.json,无白名单) | ✗ | T-B3/T-B4 |
| 大文件 top-N 勾选 | ✓ | ✓ | ✓(size+mtime) | ✓ | — |
| 去重 [DELETE] 行 | ✓ | ✓ | ✓(size ✓,mtime ✗ 未填) | ✓ | — |
| 应用迁移勾选 | ✓ | ✓ | ✓(mtime ✗ 未填) | —(MoveJunctionOp::PreflightCheck:存在/reparse/保护源+目标/目标不存在/空间/RM 占用) | 目标盘非系统盘(OnChooseTarget) |
| 文件夹树右键删除 | ✓(单条) | ✓ | ✓(mtime ✗ 未填;**黑名单缺口 T-B2**) | ✓ | 确认弹窗 |
| 历史撤销 QUARANTINE | ✗ | ✗ | **✗** | ✗(UndoPaths:仅存在性+.restored 冲突处理) | T-B6 |
| 历史撤销 MOVE_JUNCTION | ✗ | ✗ | **✗** | ✗(Undo 无 preflight) | T-B6 |
| 清空隔离区 | ✗ | ✗ | ✗(对 <各固定盘>\MiniSys.Quarantine 直接 remove_all) | — | 确认弹窗;T-B10 |
| 委派项执行 | ✓ | ✓ | ✗(见上) | ✗ | — |

结论:正向"文件操作"通道的门禁链完整且唯一(SessionService::RunPlan);旁路集中在 Undo、$RECYCLE.BIN、Delegate、清空隔离区四处,其中 Delegate 与 Undo 的输入(history/rules.json)都是用户可编辑文件。

---

## 4. (C) 配置与外部 IO 发现清单

### T-C1【Confirmed】Json 解析器不容冒号/逗号后空白 → 随包 rules.json 从未生效
- 位置:`MiniSys\src\util\Json.cpp:180-184`(`ParseObject` 消费 `:` 后未 SkipWs 即调 ParseValue;主审线索确认,本实验另发现 `:199-208` `ParseArray` 在 `,` 后同样不 SkipWs)。
- 实验:随包 `MiniSys\rules.json` → `bad number at offset 14`,`text[14]` 正是 `"version": 2` 的空格——与用户日志逐字节一致。`{"a": 1}`→offset 5;`[1, 2]`→offset 3;`{"a":[1, 2]}`→offset 8;tab 同理。**数组元素间的换行也致命**(实测我方多行手造文件失败),即任何"人写得像样"的 rules.json 都解析失败,只有单行零空白文件可用。
- 后果:外置规则特性(热更新)自 ae3ce19 完全失效;每次启动 WARN 后静默回退;内置表与外置表当前内容等价,掩盖了故障。
- 修复:`++pos_;` 后各补一次 SkipWs(对象值前、数组元素前);补回归测试直接断言"仓库 rules.json 能被解析且条数=27"。
- 置信度:Confirmed。

### T-C2【Confirmed】解析器健壮性:无深度限制(崩溃)、数字贪吃、编码不容
- 位置:`Json.cpp:152-166`(递归无深度限制)、`:288-308`(数字扫描吞 `[0-9.eE+-]` 任意序列,`stod` 前缀成功即接受,`end` 被忽略)、`StringUtils.cpp:10-16`(Utf8ToWide 不剥 BOM)。
- 实验:`deep 10000`/`100000` → 子进程退出码 **0xC00000FD(栈溢出)**,500 层正常报错;`{"a":1.2.3}` 静默解析为 1.2;BOM 文件 offset 0 失败;UTF-16(记事本"Unicode")文件解析失败;`+1`/`01` 被接受(非标准,低危);`1e999` 正确拒绝。
- 后果:损坏/恶意 rules.json(10k 字节)→ 每次启动崩溃 + 崩溃转储堆积(联动 T-C7);`minAgeDays: 3.0.0` 之类被静默截断。
- 修复:递归深度上限(如 64,超限报错)、数字语法严格校验(`end==len` 且格式合法)、读入时剥 UTF-8 BOM/检测 UTF-16 并拒绝且**可见告警**。
- 置信度:Confirmed。

### T-C3【Confirmed】解析失败静默降级,用户零感知
- 位置:`JunkRules.cpp:159-181`(`MS_LOG_WARN` 后 `return BuiltinRules()`,UI 无任何提示);DESIGN-v2 R-005 把"解析失败回退"当已覆盖风险,但用户核心诉求恰是"要能知道发生了什么"。
- 修复:规则加载结果进状态栏/横幅("rules.json 解析失败(原因),已使用内置规则;查看修复建议");首次分发覆盖用户编辑的风险在解析器修好后才真实存在,届时应做"用户改过则不覆盖/合并"策略。
- 置信度:Confirmed。

### T-C4【Likely】VolumeIndex FSCTL_ENUM_USN_DATA Win32 87:MFT_ENUM_DATA 版本字段全零
- 位置:`VolumeIndex.cpp:231`(`MFT_ENUM_DATA med{}` 零初始化,Min/MaxMajorVersion=0/0)。Win10 1709+ 要求显式 2..3,否则 ERROR_INVALID_PARAMETER(87)。
- 证据链:用户 23:48 提权运行日志无 "cannot open volume"(卷打开成功)、无 journal 告警(FSCTL_QUERY_USN_JOURNAL 成功)、唯 FSCTL_ENUM_USN_DATA 报 87 → 参数级失败,与版本字段理论吻合。未在本机复现(当前 shell 非提权,\\.\C: 打开即 Win32 5——测试套件日志里的 5/183 正是非提权测试进程所致)。
- 关联:(a) 索引恒失败 → Junk 扫描 57s(用户日志)vs 设计 ≤30s;(b) **安全互锁**:索引路径下变体语法规则会被 `NormKey` 键不匹配判为 known-absent 而丢弃(JunkScanner.cpp:172-175)——即索引修好会意外挡住 T-B1 的变体,而当前恒回退的 ExpandRule 正是绕过存活的路径。
- 附(主审补充已确认事实,本报告仅评估影响):VolumeIndex.cpp 手写 MSN_RECORD_V2/V3 与 winioctl.h 真实布局逐字段错位约 +8 字节——**不影响本报告任何门禁结论**(我的全链路实验都在索引失败回退路径上;若 FSCTL 修好,错位记录会产出乱名索引,变体规则照样 known-absent 丢弃,下游闸门不变),但意味着 FSCTL 修复必须与结构体修复一起做,否则索引产出垃圾数据。
- 修复:`med.MinMajorVersion=2; med.MaxMajorVersion=3;` + 直接改用 winioctl.h 的 `USN_RECORD_V2/V3`;补一条提权集成测试。
- 置信度:根因 Likely(日志+文档佐证,未提权复现);现象 Confirmed(用户日志)。

### T-C5【Confirmed,代码级】OperationLog:UpdateStatus 锁外快照可丢并发 Append;未知 type 静默变 DELETE
- 位置:`OperationLog.cpp:224-239`(`auto all = LoadAll();` 在 `mu_` **之外**,与之后持锁重写之间存在窗口,并发 Append 的记录会被重写吞掉);`:280-287`(`StrToType` 未知串默认 `DeleteToRecycleBin`);`:182-186`(`Instance()` 每次调用都执行 MigrateLegacyIfNeeded,锁+两次 stat,廉价但无谓)。
- 现状可达性:UI 按钮状态基本排除了 Append/UpdateStatus 并发(执行期间 undo/emptyQ 禁用或 busy 拒绝),属**潜伏**问题;但 `HistoryPresenter::UndoSelected`(Presenters.cpp:337-367)无 busy 检查且在 UI 线程同步执行(MoveJunctionOp::Undo 是整目录复制,会冻结 UI)。
- 修复:LoadAll 移入锁内或重写采用"读-改-写"全程持锁;StrToType 未知类型返回可辨识的 Unknown 并在加载时丢弃/标注;UndoSelected 走 SessionService 队列。
- 置信度:Confirmed(代码机制),丢记录竞争未实际触发(标注)。
- 已验证无问题部分:UpdateStatus 原子重写(temp+rename)正常、无 .tmp 残留;损坏行/TSV 混排行容错正常;size 以 double 序列化在 2^53 内无损(实测 1605435521 round-trip 正确)。

### T-C6【被证伪的假设 + 事实澄清】"每次启动都在迁移 TSV" 并非真实迁移循环
- 证据:用户 `%LOCALAPPDATA%\MiniSys\history\` 目录**只有 history.jsonl**(无 history.tsv / .bak);迁移幂等性沙箱验证通过(jsonl 存在即不再写、tsv→bak 一次);日志中 "Migrated 1 legacy TSV records" 的每次出现都伴随 "Scanner threw: boom"(SessionServiceTests 的抛异常扫描器)和分 Tab 扫描爆发——**是 MiniSysTests.exe 测试固件(OperationLogTests.LegacyTsvIsReadAndMigrated 写临时 tsv)写入共享生产日志所致**,真实 GUI 启动(23:48,含 "MiniSys starting; elevated=1")没有迁移行。
- 附带发现:单元测试通过 Logger 单例把测试输出写进用户真实 minisys.log(无测试重定向机制)——污染诊断数据、制造假象,建议 Logger 支持 SetLogPathForTesting。
- 置信度:Confirmed(目录清单+日志时间线+沙箱幂等实验)。

### T-C7【Confirmed,代码级】崩溃转储:写入失败静默、无保留上限、内容含路径
- 位置:`CrashDump.cpp:21-38`(MiniDumpNormal=线程栈+模块表,栈中可能含文件路径;`CreateFileW` 失败静默;文件名秒级时间戳、无数量上限;`EXCEPTION_EXECUTE_HANDLER` 静默退出)。
- 联动 T-C2a:损坏 rules.json → 每次启动栈溢出 → dumps 目录无界增长。
- 修复:保留最近 N 份(删最旧);写入失败记入 Event Log 或 stderr;隐私说明(转储可能含路径,不上传则风险低)。
- 置信度:Confirmed(代码),堆积场景为推演(基于已证实的崩溃)。

### T-C8【Confirmed,代码级】日志:全路径落盘 + 无轮转(v1 R-009/R-010 延续)
- `Logger.cpp:29-56`:追加写 minisys.log、逐条 flush、无轮转、无大小上限;扫描/执行日志含完整用户路径(隐私旁注:本地文件、无网络,风险低)。与 v2 无变化。
- 置信度:Confirmed。

### T-C9【Confirmed】权限模型与失败归因(对应用户 Win32 5 场景的模型层评估)
- requireAdministrator + 无沙箱(ADR-010,自用工具接受),一切文件操作以管理员身份执行——因此 T-B1/T-B2/T-B3 的绕过都直接是管理员级破坏,无纵深。QuarantineOp 对系统 ACL 目录的 MoveFileEx 失败归因与提示改进见 T-B7;`MoveWithRetry` 无 MOVEFILE_COPY_ALLOWED,跨卷必然失败(设计上同卷,正确,但错误码若为 17 应提示"跨卷不支持")。
- 置信度:Confirmed。

---

## 5. 加固建议:从黑名单到"结构化允许区"

产品承诺是"系统区域拒绝、默认隔离区可逆"。当前黑名单(8 个根)漏 Users/Program Files/ProgramData/PerfLogs,且字符串匹配可绕。建议分两步:

**第一步(保守,兼容现状):规范化 + 扩黑名单**
- `CanonicalizeForGuard(p)`:GetFinalPathNameByW(或 RtlGetFullPathNameExW+GetLongPathNameW)→ 统一大小写与分隔符 → 前缀匹配。拒绝无法规范化的输入(deny-by-default)。
- 黑名单补:`%SystemDrive%\Users`、`Program Files`、`Program Files (x86)`、`ProgramData`、`PerfLogs`、`Windows.old`(或按策略)。
- 漏报对比:仍依赖名单完备(未来新系统目录会漏);误报对比:几乎为零(这些根下的清理诉求本就应走规则表/委派)。

**第二步(推荐):结构化允许区(白名单)**
- Quarantine/Delete 类文件操作仅允许命中:① `%TEMP%`、`%LOCALAPPDATA%`、`%USERPROFILE%` 下**规则表显式声明的子树**(规则加载期把 path 规范化并登记为允许根);② `%ProgramData%` 非 Microsoft 子树的显式声明根;③ LargeFiles/FolderTree 通道仅允许 `%USERPROFILE%` 与用户显式选择的目录(选择时登记)。
- 漏报对比:名单外新缓存位置清不到(需加规则)——与"规则表可扩展"的产品方向一致;误报对比:误伤率高于黑名单(自定义位置需登记),但对"系统盘清理"场景可控。
- 迁移(Apps)通道保持现有目标盘白名单(非系统盘 + 非保护路径)。
- 无论哪种,规则加载期校验(T-B3)与确认弹窗逐项风险列(见下)都必须做。

**UX(直接回应用户诉求)**:确认弹窗从"N 项/总大小/泛化 dangerous"升级为逐项一行——`路径缩略 | 规则来源(ruleId/通道) | 风险级 | 为什么可逆/不可逆`;执行报告的"跳过原因"已较细(Verdict.reason),但扫描列表应在勾选前就标注"该将被安全闸拒绝"(T-B9)与"该目录正被占用"(预检前置)。

---

## 6. 可直接追加到 MiniSysTests 的测试草案

```cpp
// JsonTests.cpp —— 修复 T-C1/T-C2 的回归
TEST(JsonTests, WhitespaceAfterColonAndComma) {           // 当前必红
    Json j; std::wstring e;
    EXPECT_TRUE(Json::Parse(L"{\"a\": 1, \"b\": [1, 2]}", j, e)) << e;
}
TEST(JsonTests, ShippedRulesJsonParses) {                  // 读仓库 MiniSys/rules.json
    // 断言:Json::Parse 成功且 rules 数 >= 27 —— 当前必红(offset 14)
}
TEST(JsonTests, DepthLimitRejectsNotCrash) {               // 修复后:深嵌套返回 false 而非崩溃
    EXPECT_FALSE(Json::Parse(std::wstring(100000, L'['), j, e));
}
TEST(JsonTests, GarbledNumberRejected) { EXPECT_FALSE(Json::Parse(L"{\"a\":1.2.3}", j, e)); }

// GuardRailsTests.cpp —— 变体语料(修复后应全绿)
TEST(GuardRailsTests, PathVariantCorpusAllDenied) {
    // 取真实短名: GetShortPathNameW(L"C:\\Program Files\\WindowsApps")
    for (auto p : {L"C:\\\\Windows\\System32", L"\\Windows\\System32", L"\\\\?\\C:\\Windows\\System32",
                   L"\\\\localhost\\C$\\Windows\\System32", shortName, L"C:/Windows/System32",
                   L"C:\\Users\\..\\Windows\\System32", L"C:\\Windows.\\System32"}) {
        EXPECT_TRUE(GuardRails::IsProtectedPath(p)) << p;   // 当前 7/9 红
    }
}
TEST(GuardRailsTests, SystemDirsOutsideBlacklist) {          // 决策后:拒绝或至少 Advanced
    for (auto p : {L"C:\\Users", L"C:\\Program Files", L"C:\\ProgramData"}) { /* … */ }
}
// JunkRulesTests.cpp —— 规则加载期校验
TEST(JunkRulesTests, RulesPointingAtSystemAreaRejectedAtLoad) { /* path=C:\Windows\* 或变体 → 该规则被拒 */ }
TEST(JunkRulesTests, DelegateCommandWhitelist) { /* 非 DISM/powercfg/cleanmgr 绝对路径 → 拒绝 */ }
// QuarantineOpTests.cpp —— Win32 5 归因
TEST(QuarantineOpTests, OpenChildFileBlocksDirMove) {
    // CreateFile(child, share=read|write) 后 UniqueTargetFor/移动路径 → 错误串应含"占用"提示
}
TEST(QuarantineOpTests, UndoPathsRefusesProtectedRestore) { /* restoreTo 在保护名单 → 拒绝 */ }
// SessionServiceTests.cpp —— RunPlan 分支(现无覆盖)
TEST(SessionServiceTests, RunPlanRecycleBinSpecialCaseSkipsGuardRails);   // 现状固化
TEST(SessionServiceTests, RunPlanDenySkipsNotFails);                      // 现状固化
```

---

## 7. 修复顺序(建议)

1. **Json.cpp 两个 SkipWs + 深度/数字严格化 + 回归测试**(T-C1/T-C2)——一行级改动,解锁整个外置规则特性;同时消除启动崩溃面。
2. **GuardRails 规范化 + 黑名单扩充**(T-B1/T-B2)+ 变体语料测试——兑现"系统目录一律拒绝"。
3. **rules.json 加载期校验 + Delegate 命令白名单/绝对路径**(T-B3/T-B4)。
4. **QuarantineOp 占用预检 + 错误归因 + 重试**(T-B7)——直接回应用户 npm-cache 痛点与"风险分析提示"诉求。
5. **VolumeIndex MFT_ENUM_DATA 版本 + 结构体改用 winioctl.h**(T-C4)——恢复索引(57s→目标)、顺带闭合回退路径上的变体绕过温床。
6. UX:确认弹窗逐项风险行、扫描列表预标注不可执行项/占用项、rules.json 失败可见横幅(T-B9/T-C3/§5)。
7. 清理项:UpdateStatus 锁范围、Undo 保护路径复验、清空隔离区按盘判定、CrashDump 保留策略、Logger 测试重定向(T-C5/T-B6/T-B10/T-C7/T-C6 附带)。

---

## 8. 已验证无问题 / 被证伪的假设

| 项 | 结论 |
|---|---|
| PlanBuilder+GuardRails 不变量(UI 不直接 new Operation) | 成立(T-B8,grep 全构造点) |
| planHash 整单过期机制 | 代码正确(PlanBuilder.cpp:16-66);测试已覆盖 |
| UpdateStatus 原子重写 | 正常,无 .tmp 残留(沙箱实测) |
| history.jsonl 损坏行/TSV 混排容错 | 正常(实测:垃圾行跳过、TSV 行可读) |
| TSV→JSONL 迁移幂等 | 正常(jsonl 存在即不重写;tsv→bak 一次性) |
| "每次启动都在迁移" | **证伪**:系 MiniSysTests 写共享生产日志的测试固件噪音(T-C6) |
| "正斜杠可绕过 GuardRails 并移动文件" | **证伪一半**:字符串级确可绕 IsProtectedPath,但 LongPath 的 `\\?\` 使所有属性/移动 API 拒绝正斜杠、`..`、尾点、盘符相对路径(T-B11)——巧合防御,不可依赖 |
| "UNC 路径可完整走通隔离" | 字符串+Validate+存在性已证通过;移动层未测(需提权访问 C$),标注 |
| "尾点/尾空格可绕过" | 证伪(`\\?\` 下不剥离,存在性即失败) |
| "readonly 属性导致 Win32 5" | 证伪(实测 readonly 目录照常移动);Win32 5 根因为目录内打开句柄 |
| VolumeIndex 结构体错位对门禁绕过的影响 | 无(索引恒失败→实验全在回退路径;索引修好后变体规则 known-absent 丢弃,下游闸门不变)——主审已确认的缺陷,本报告仅评估链路影响 |

## 9. 未验证 / 局限

- 未提权:FSCTL_ENUM_USN_DATA Win32 87 根因(MFT_ENUM_DATA 版本字段)未在本机复现,仅有日志+API 文档佐证(Likely);UNC 变体的 MoveFileExW 层未实测;真实 C:\Windows\System32 的隔离**有意未执行**(安全红线),全链路结论由"只读闸门全绿 + 同语法沙箱移动成功"组合证明。
- 未运行 MiniSys.exe / GUI:确认弹窗逐项内容、FolderTree 右键的端到端体验为代码级结论(工作区 v2.1)。
- history 并发丢记录竞争未实际触发(UI 状态基本排除,标注潜伏)。
- DelegateOp 挂起/超时未实测(代码 INFINITE 自明);其测试套件已覆盖正常路径。
- CrashDump 堆积为推演;日志隐私为旁注。
- 实验产物全部位于 `C:\tjf\github\MiniSys\.tmp-review\exp\`(guard_probe.cpp/build.cmd/rules.json/sandbox/obj),未触碰 build\、未修改仓库任何文件;guard_probe 的 Logger 为本地 stub、OperationLog 重定向到沙箱,用户真实 %LOCALAPPDATA%\MiniSys 除只读读取外无任何写入。
