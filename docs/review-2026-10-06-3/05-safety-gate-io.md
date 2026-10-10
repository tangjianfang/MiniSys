# 05 号补审:危险操作门禁对抗实测(2026-10-11)

> 对象:HEAD 76f6514(v2.13c)及工作树(评审进行中出现的 52435a2 已单独标注并实测两版)。
> 方法:把真实 core 源码(GuardRails/JunkRules/JunkScanner/VolumeIndex/OperationLog/QuarantineOp/
> MoveJunctionOp/DelegateOp + util)编译进对抗 driver(05-exp\sandbox.exe,9 个子模式),
> 攻击样本实际跑出门禁代码。唯一补丁:PathUtils.cpp AppDataDir 支持
> MINISYS_REVIEW_APPDATA 重定向(仅数据位置,不涉门禁逻辑;真实 %LOCALAPPDATA%\MiniSys 未写)。
> 仓库只读、未启动/结束 MiniSys.exe、未触碰 tools\ 与 build.bat。

## 摘要(Top 风险按处置优先级)

1. **T-A1 伪造历史记录→还原链任意落盘(提权闭环,CONFIRMED)**:标准用户伪造 history.jsonl/history.tsv
   + 在可写位置放 payload,管理员在历史页点"还原"(无确认框)→ 管理员权限把任意文件写入
   任意非保护路径(启动文件夹实测不受 GuardRails 保护)。QuarantineOp::UndoPaths 与
   MoveJunctionOp::Undo 双通道均零鉴真。
2. **T-A3 投毒索引→Finalize 死循环(进程挂死+内存暴涨,CONFIRMED)**:重复 FRN 构成可达环,
   TryLoadCache→Finalize 的 DFS 无环检测,实测 25s+ 不返回、工作集 1.9GB;catch(...) 兜不住循环。
3. **T-A2 投毒索引 walk=1 绕过一切校验(CONFIRMED)**:伪造 MISX 缓存(只需真实卷序列号)置
   walk 标志→RefreshFromUsn 直接 return true→无 journal 比对、无重建;假路径上搜索页、
   known-absent 把系统区清理条目全部隐藏(实测 user-temp/windows-temp/winsxs/hiberfil 等
   全消失)。修复版 52435a2 同样成立。
4. **T-A5/T-A7 rules.json 双重校验缝隙(CONFIRMED)**:ValidateRule 只看 rule.path,
   profileNames 绝对路径经 fs::path operator/ 根替换逃到任意路径;InfoOnly 规则零校验,
   System32\cmd.exe 实测出现在清理列表。执行层 GuardRails 均兜住(非保护路径除外——
   伪造规则可清理任意用户文件)。
5. **T-A8 OnOpenLocation 参数逃逸(CONFIRMED)**:缓存投毒的 path 含 `"`+空格→
   explorer 参数被切成多个 token,第二 token 可为任意 URL/路径(打开默认浏览器)。

## 发现清单

### T-A1(高危|CONFIRMED)伪造历史→还原 = 管理员权限任意文件落盘

- 位置:
  - `MiniSys/src/core/QuarantineOp.cpp:133-165`(UndoPaths:仅校验 restoreTo,quarantinePath 零校验,无 manifest 鉴真,recordId 仅用于改状态)
  - `MiniSys/src/core/MoveJunctionOp.cpp:224-249`(Undo:source 是 reparse point 即执行 target→source 整目录拷贝,无 GuardRails)
  - `MiniSys/src/ui/Presenters.cpp:851-881`(UndoSelected:多选批量,无确认对话框)
  - `MiniSys/src/core/OperationLog.cpp:188-212`(MigrateLegacyIfNeeded:伪造 TSV 原样迁入)
- 复现(driver `history` 模式,实测输出):
  - `IsProtectedPath(REAL startup folder C:\Users\tjf\AppData\Roaming\Microsoft\Windows\Start Menu\Programs\Startup)=0`(启动文件夹不在保护名单)
  - 伪造 TSV 行(`id\t时间\tQUARANTINE\tSUCCESS\t999\t1\t<启动文件夹>\evil.dll\tC:\MiniSys.Quarantine\payload\t...`)迁入 jsonl:`h4: migrated ... reversible=1`
  - `UndoPaths(forged) -> 1`,`payload landed: exists=1`
  - `MoveJunctionOp::Undo(forged) -> 1`,`staged.exe landed at source: exists=1`
- 攻击链:标准用户写 `%LOCALAPPDATA%\MiniSys\history\history.tsv`(或 jsonl)+ 在
  `C:\MiniSys.Quarantine\`(C:\ 根默认 Authenticated Users 可建目录/文件)放 payload →
  管理员启动(迁移)→ 历史页看到"隔离 evil.dll"记录 → 选中还原 → payload 落启动文件夹 →
  管理员登录执行 = **提权闭环**。记录时间戳/title 可伪造降低警觉。
- 与"已修勿重报"核对:上轮 T-B1 残余面仅记 quarantine 方向("移入隔离区"),undo 方向
  (target→source 落盘)未覆盖;"GuardRails 仍拦系统目录"仅对 restoreTo 成立,启动文件夹等
  敏感用户态路径不拦。**不属重复上报**。
- 修复建议:①还原前重验记录真实性(隔离区落盘时写旁证 manifest:记录 id+源路径哈希,
  UndoPaths 比对);②quarantinePath 必须位于 `<盘根>\MiniSys.Quarantine\` 子树内
  (目前任意路径);③UndoSelected 加确认对话框并显示 source 完整路径;④启动文件夹/
  计划任务/Run 键位置纳入 IsProtectedPath 或至少还原时额外告警。
- 置信度:高(driver 全链实测;真实产品内入口为历史页右键/按钮,代码路径相同)。

### T-A3(高危|CONFIRMED)投毒缓存重复 FRN 环 → Finalize 无限循环

- 位置:`MiniSys/src/core/VolumeIndex.cpp` Finalize(≈196-247 行,v2.13c):显式栈 DFS 无环检测;
  `childrenOf_[parentFrn]` 按 push_back 构建,重复 FRN 的两个节点共享同一孩子列表 → 可达环。
- 复现(driver `poison-loop`):10000 filler + 三节点(A(frn=K,parent=root)、C(frn=G,parent=K)、
  B(frn=K,parent=G))→ TryLoadCache 通过 bounds 检查 → Finalize 处理 A 时枚举
  childrenOf_[K]={C}→C 枚举 childrenOf_[G]={B}→B 再枚举 childrenOf_[K] → 循环。
  实测 25 秒不返回;被杀时工作集 **1,941,136 KB(1.9 GB)** 且持续增长。
- 后果:SessionService::StartTask 的 catch(...) 无法捕获死循环 → UI 永久"执行中",
  内存耗尽方向 OOM。攻击成本=标准用户写一个 index-cache-*.bin。
- 修复建议:①加载侧构建 childrenOf_ 时按 (parentFrn 唯一化)或对 FRN 去重(重复 FRN 拒载);
  ②Finalize DFS 加已访问标记或步数上限(nodes_.size()*2);③两者都做(纵深)。
- 置信度:高(实测挂死+内存曲线)。

### T-A2(高危|CONFIRMED)walk 标志绕过 journal 校验 → 投毒索引被当权威

- 位置:`VolumeIndex.cpp` RefreshFromUsn 首行 `if (walkBuilt_) return true;`(≈649 行);
  TryLoadCache 接受 walk=1 的缓存后 EnsureBuilt 直接 `SaveCacheIfWorthwhile(); return true;`
  (≈274-277)——不比对 UsnJournalID、不触发任何重建。
- 复现(driver `poison-gen`/`poison-check`,实测输出):
  - `EnsureBuilt=1 valid=1 walkBuilt=1 entries=10012 (91 ms, no full rebuild => poisoned cache accepted)`
  - 搜索"SAM" → `path=C:\Windows\System32\config\SAM`、`SAM.log`(磁盘上不存在,纯伪造)
  - 搜索"机密" → `C:\Users\victim\Documents\机密合同.docx`
  - 名字含路径分隔符 → 显示路径拼接 `C:\weird\a\..\..\Windows\System32\cmd.exe`
    (OriginalCasePathOf 对 names_ 零字符过滤)
  - `TryGetEntry(%TEMP%)=0 (dir exists on disk=1)` → known-absent
  - 全链扫描(`rules` 模式):**user-temp、windows-temp、wu-download、cbs-logs、dism-logs、
    minidump、do-cache、winsxs、windows-old、hiberfil、pagefile、swapfile 条目全部消失**
    (82 项→64 项,消失的全是系统盘规则)——ExpandRuleIndexed 把"known absent"当权威,
    垃圾清理页近乎清空 = 大面积 DoS。
- 与"已修勿重报"核对:上轮已记档"index-cache 投毒仅致漏扫(HasDir 短路),不构成提权"——
  本轮实测把影响面展开:①漏扫范围=**所有索引盘规则**(不止 HasDir 一处);②新增**显示层
  欺骗**(假系统文件/假用户文档/名字带分隔符);③**walk=1 使投毒跳过 journal 校验长期存活**
  (上轮未涉及)。执行层:GuardRails.Validate 复验兜住假路径(不存在/受保护→拒),
  非保护且真实存在的路径会被真隔离(钓鱼勾选)。
- 附注(版本归属):评审期间 review-03 提交 52435a2"cache format symmetry"修复了
  Save/Load 的 walk 字节不对称(B-1,**勿重报**;本实验的 probe 模式对真实
  %LOCALAPPDATA% 缓存的错位读数独立复现了该缺陷)。**修复后版本已实测:投毒链依旧成立**
  (91ms 接受),且格式对称后 SaveCacheIfWorthwhile 会把投毒数据按对称格式**回写**,
  投毒跨会话存活(修复前重写丢 walk 字节反而自愈)。
- 修复建议:缓存文件加 HMAC(密钥=每安装随机,落 HKLM ACL 仅管理员)或至少:
  walk=1 时校验"该卷 USN journal 确实不可用"再跳过;nodeCount/目录数/名字池的
  统计健全性交叉校验;names_ 拒绝包含 `\`、`/`、控制字符的名字(FindFirstFile 不可能产生)。
- 置信度:高。

### T-A4(中危|CONFIRMED)cNameBytes 无上限 → bad_alloc 传播

- 位置:`VolumeIndex.cpp` TryLoadCache:`names_.resize(cNameBytes/2)`——cNodeCount 有
  5000 万上限,**cNameBytes 无上限**(仅查非零+偶数)。
- 复现(driver `poison-huge`):header 声明 nameBytes=0x7FFFFFFFFFFFFFFE(偶)→
  `EnsureBuilt THREW std::exception: bad allocation`;8GB 档进程异常终止(exit=127)。
- 后果:真实链路在 StartTask catch(...) 内 → 该次任务失败(不崩进程);与 T-A3 组合
  (先巨值耗内存)可放大。64MB 超长行同理见 T-A9。
- 修复:`cNameBytes > nodeCount * 512`(名字平均不可能超 512 wchar)之类的比例上限。
- 置信度:高。

### T-A5(中危|CONFIRMED)profileNames 逃逸:ValidateRule 只校验 rule.path

- 位置:`JunkRules.cpp` ValidateRule(227-249,仅展开 r.path 过 IsProtectedPath);
  `JunkScanner.cpp` ExpandRule Profiles 分支 `fs::path full = p / name;`(fs::path operator/
  对带根名的 RHS **整体替换**,pathsem 实测:`base / L"C:\Windows\System32"` → `C:\Windows\System32`)。
- 复现:`r5-prof-abs`(rule.path=用户沙箱,profileNames=`C:\Users\...\Tencent Files`)被
  LoadValidated **ACCEPTED**(校验只看用户空间 base);候选生成路径=profileNames 绝对路径,
  脱离 base 子树。管理员权限下 C:\Windows\System32\config 亦可作目标(本机非提权跑 size=0
  被 emit 逻辑丢弃;提权环境会出条目)。
- 减缓(实测,意外防御):①`..` 链逃逸被 LongPath 的 `\\?\` 前缀阻断(`\\?\` 不解析 `..`,
  QueryCandidate 失败→无候选)——**依赖实现细节,非设计防御**;②SplitNames 剥空格使含空格
  目标失效(见 T-A12,内置规则因此也坏了)。
- 危害:显示层出现 base 外条目;执行层 GuardRails 拦系统区,**但非保护路径(任意用户文件)
  会被真实隔离**——"诱导导入恶意 rules.json"或 exe 目录可写场景下成立。
- 修复:ValidateRule 对 Profiles 规则把每个 profileName 展开候选(base/name)后逐个过
  IsProtectedPath;或拒绝 profileNames 含 `:`、`..`、以 `\` 开头的成分。
- 置信度:高(校验放行+fs::path 语义实测;候选生成由 fs::path 规则保证)。

### T-A6(中危|CONFIRMED)委派白名单:前缀边界后放行任意尾缀

- 位置:`JunkRules.cpp` DelegateCommandAllowedImpl(168-224):boundary 检查只保证白名单
  前缀结束于 token 边界,**之后的 token 完全不查**。
- 复现(LoadValidated ACCEPTED):`powercfg.exe /h off /a extra`、
  `Dism.exe /Online /Cleanup-Image \r\n/x`(CreateProcess 无 shell,换行不注入,但尾缀任意)。
- 影响:`Dism.exe /Online /Cleanup-Image /ResetBase`(不可逆,移除所有更新卸载点)、
  `cleanmgr` 空参白名单(L"")等可经 rules.json 触发。非提权(管理员本可跑)但白名单
  "仅放行内置命令"的意图被架空。
- 修复:白名单从"前缀"改为"参数集精确匹配"(如 dism 仅
  {/Online,/Cleanup-Image,/StartComponentCleanup} 的子集;powercfg 仅 {/h,off});
  或尾缀超过白名单参数个数即拒。
- 置信度:高。

### T-A7(中危|CONFIRMED)InfoOnly 规则零校验 → 系统文件上清理列表

- 位置:`JunkRules.cpp` ValidateRule:235 `if (r.strategy != Quarantine) return true;`——
  InfoOnly 的 path 不做任何检查。
- 复现:`r5-info-sys` path=`C:\Windows\System32\cmd.exe` strategy=info → ACCEPTED;
  扫描输出 `item rule=r5-info-sys risk=3 strat=2 path=[C:\Windows\System32\cmd.exe] size=344064`。
- 执行层:GuardRails::Validate 对 riskLevel==InfoOnly 拒绝 ✓(兜住)。
- 危害:管理员看到"System32\cmd.exe 可清理 344KB"——钓鱼/恐慌/信任破坏;配合伪造
  detailHint 可给出任意指引文本。
- 修复:InfoOnly 规则 path 同样过 IsProtectedPath(拒绝或 UI 标记"系统位置,仅供参考")。
- 置信度:高。

### T-A8(中危|CONFIRMED)OnOpenLocation 参数拼接逃逸

- 位置:`MainWindow.cpp:1780` `std::wstring args = L"/select,\"" + path.wstring() + L"\"";`
  → ShellExecuteW(explorer.exe)。
- 复现(driver `selargs`,CommandLineToArgvW 实测切分):
  - path=`C:\x\fi" https://evil.example/pwn` → argv=[`/select,"C:\x\fi"`,`https://evil.example/pwn`]——**第二 token 独立成参**;
  - path=`C:\x\fi" ,calc.exe` → argv[1]=`,calc.exe`;
  - 含换行/Tab 的 path → 两 token。
- 路径来源:列表项 path 可来自用户可写的 results-*.json(T-B1 同源,JsonToScanItem 仅查
  非空)或投毒索引(名字零过滤)——NTFS 真实文件名不可能含 `"`/换行,故**唯一入口是投毒数据**。
- 后果:explorer 将多余 token 当作第二对象打开(URL→默认浏览器=钓鱼页;路径→弹窗)。
  解析层实测;explorer 自身语义按文档推断(未弹窗实测,保守标注)。
  尾 `\` 目录(`/select,"C:\dir\"`)引号转义混乱→定位失败(功能 bug)。
  COMSPEC 不适用(lpFile 固定 explorer.exe,不经 cmd)。
- 修复:转义 path 中的 `"`(双写)并拒绝含控制字符的 path;或改用
  `explorer.exe /select,"<path>"` 的 SHEllExecuteExW + SEE_MASK_NOASYNC 并对 path 做
  `PathCleanupSpec`/白名单字符校验;目录去尾反斜杠再拼。
- 置信度:切分逃逸=高;explorer 打开 URL=中高(未活体弹窗)。

### T-A9(中危|CONFIRMED)history.jsonl 超长行内存放大

- 位置:`OperationLog.cpp` ReadAllLines(119-130)无行长上限。
- 复现:64MB 单行 → LoadAll 全部 5 条记录正常解析(**不丢记录**),进程 WS 峰值 **647MB**
  (std::string 64MB + wstring 128MB + Json 解析拷贝)。伪造 1-2GB 行 → 数 GB 峰值 OOM。
  重复 id×1000 + UpdateStatusBulk 重写无损(loss=0);嵌 NUL 行被宽容解析成记录
  (id 带 NUL 进内存,无崩溃);无死循环。
- 修复:ReadAllLines 单行截断(如 4MB)并丢弃超限行记 WARN。
- 置信度:高。

### T-A10(低危|CONFIRMED)日志注入

- 位置:`util/Logger.cpp:70` 消息原样落盘。
- 复现(driver `loginject`):`MS_LOG_WARN` 载荷含 `\r\nFAKE INFO 2099-01-01 [INFO ] ...` →
  `forged log line present=1`。可控来源:rules.json 的 command/title
  (DelegateCommandAllowedImpl 的 reason 拼 args)、路径名(伪造缓存/规则)。
- 后果:伪造日志行误导排障(日志是排障事实来源);隐私:日志/dump 落用户可写目录,
  含完整路径与命令(MiniDumpNormal 最小转储,可接受)。
- 修复:写入前把消息中 `\r`/`\n` 替换为 `\x0D`/`\x0A` 字面。
- 置信度:高。

### T-A11(低危,功能,CONFIRMED)TSV 迁移条件缺陷

- `OperationLog.cpp:204` `if (!out.empty() && !FileExists(jsonl))` 才写 jsonl,但
  `tsv→.bak` 的 MoveFileEx **无条件执行** → jsonl 已存在时 tsv 内容被静默丢弃(仅 .bak)。
  实测复现(migrated=0 而 .bak 出现)。升级/双版本混用场景丢历史。
- 修复:仅当实际迁移成功后才挪 tsv。

### T-A12(低危,功能,CONFIRMED)SplitNames 剥空格 → 内置规则 Code Cache 永不生效

- 位置:`JunkScanner.cpp:64-76` `else if (ch != L' ') cur += ch;` 丢弃所有空格。
- 复现:磁盘建 `Profile1\{Cache,Code Cache,GPUCache}`,规则 profileNames=
  `"Cache;Code Cache;GPUCache"`(与内置 edge/chrome 完全一致)→ 扫描只出
  **Cache、GPUCache**,Code Cache(磁盘存在)被漏——内置 edge-profiles/chrome-profiles
  的 "Code Cache" 清理自上线从未工作。安全面:剥空格意外阻断含空格路径的逃逸(T-A5 减缓)。
- 修复:SplitNames 只按 `;`/`,` 切分,保留空格;同步修 T-A5(去掉这个"意外防御"后必须
  补 profileNames 成分校验)。

## 语料附录(输入|当前判定|应判)

| # | 输入(样本) | 当前判定 | 应判 |
|---|---|---|---|
| 1 | history.tsv 伪造 `QUARANTINE/SUCCESS/reversible=1,source=<启动文件夹>\evil.dll,target=C:\MiniSys.Quarantine\payload` | 迁入 jsonl,可还原,UndoPaths 成功落盘 | 迁入前/还原前鉴真(manifest 比对);还原加确认;target 必须在隔离区子树 |
| 2 | 投毒 index-cache(walk=1,重复 FRN 环) | TryLoadCache 通过→Finalize 死循环(挂死+OOM) | 拒载(FRN 去重/DFS 环防护) |
| 3 | 投毒 index-cache(walk=1,假 SAM/假文档/名字含 `\`) | EnsureBuilt 接受;搜索显示假路径 | 拒载或校验(缓存签名;名字字符白名单) |
| 4 | 投毒 index-cache(不含 %TEMP% 等) | 系统区规则条目全部 known-absent 消失 | 索引缺失≠不存在:known-absent 至少对磁盘可验证路径降级为 fs 复查 |
| 5 | index-cache nameBytes=0x7FFFFFFFFFFFFFFE | bad_alloc 抛出(任务级失败) | 拒载(nameBytes/nodeCount 比例上限) |
| 6 | rules.json profiles 规则 profileNames=`C:\Users\...\Tencent Files`(绝对路径) | ValidateRule ACCEPT(只看 base) | REJECT(候选展开后逐个过 IsProtectedPath 或成分黑名单) |
| 7 | rules.json profiles 规则 profileNames=`..\×8\Windows\System32` | ACCEPT;扫描被 `\\?\` 不解析 `..` 意外阻断 | REJECT(显式;现防御依赖实现细节) |
| 8 | rules.json info 规则 path=`C:\Windows\System32\cmd.exe` | ACCEPT 且出现在清理列表(size=344064) | 拒绝或标记系统位置(执行层已拒,显示层裸奔) |
| 9 | rules.json command=`powercfg.exe /h off /a extra` | ACCEPT(白名单前缀+边界即过) | REJECT(参数集精确匹配) |
| 10 | rules.json command=`Dism.exe /Online /Cleanup-Image\r\n/x` | ACCEPT | REJECT(同上;/ResetBase 类尾缀风险) |
| 11 | rules.json path=`%UNDEFINED_VAR_ZZ%\Windows\System32` | ACCEPT(字面保留,扫描无条目) | 可接受(自愈);建议加载时告警未定义变量 |
| 12 | rules.json path=`%SystemRoot%/System32/config`、`\\?\C:\Windows\...`、`%TEMP%\..×8\Windows` | REJECT(受保护系统区域) | 正确 ✓ |
| 13 | 缓存 results-*.json path=`C:\x\fi" https://evil.example/pwn` → 双击定位 | explorer 参数切成两 token,URL 独立成参 | 转义 `"`/拒控制字符;URL 不应可达 |
| 14 | 目录 path 尾 `\`(真实目录)→ OnOpenLocation | `/select,"C:\dir\"` 引号转义错乱,定位失败 | 去尾 `\` 再拼;转义 |
| 15 | history.jsonl 64MB 单行 | 全部记录保留,WS 峰值 647MB | 截断超长行(上限~4MB) |
| 16 | history.jsonl 重复 id×1000 → UpdateStatusBulk | 重写无损(loss=0) | 正确 ✓ |
| 17 | history.jsonl 嵌 NUL 行 | 宽容解析为记录(带 NUL),无崩溃 | 可接受;建议拒载含 NUL 行 |
| 18 | 日志载荷含 `\r\nFAKE INFO ...` | 伪造日志行落盘(present=1) | 换行转义 |
| 19 | 磁盘 `Profile1\Code Cache` + profileNames=`Cache;Code Cache;GPUCache` | Code Cache 被漏(空格被剥) | 修 SplitNames(保留空格) |
| 20 | jsonl 已存在时放置 history.tsv | tsv→.bak 但内容未迁移(静默丢弃) | 未迁移则不动 tsv |

## 已验证无问题 / 被推翻假设

1. **dotdot 逃逸(规则路径与 Profiles)**:`%TEMP%\..×8\Windows` 被 ValidateRule 拒;
   Profiles 的 `..` 候选被 LongPath `\\?\` 前缀阻断(NTFS API 不解析 `..`)——**意外防御**,
   非设计,修复 T-A12 时须保留替代防线(见 T-A5 修复)。
2. **%ENV% 未定义/嵌套变体**:`%UNDEFINED%\Windows\System32`、`%%SystemRoot%%`→`%C:\WINDOWS%`
   等,ExpandEnvironmentStringsW 保留字面 → 扫描枚举失败无条目,无提权路径(实测)。
3. **fwd-slash / `\\?\` 前缀 / 尾点尾空格规则路径**:IsProtectedPath 全拦(实测 3 条 REJECTED)。
4. **委派白名单既有语料**:引号 exe、CWD/PATH 裸名、cmd/powershell、copycat 路径、
   `/deleteacing`、`/Apply-Image`、`\ncmd /c calc` 均拒(语料+本轮复跑)。
5. **results-*.json→Delegate 提权(T-B1)**:双层修复存活(SessionService.cpp:696 执行点
   复验 + TryLoadCachedResults:274 消毒,代码核对);本轮无新增绕过。
6. **history 破坏样本**:不丢记录、不死循环、不崩(64MB 行/NUL/重复 id 实测);仅内存放大(T-A9)。
7. **非系统盘/索引外路径**:ExpandRuleIndexed known=false 时 fs 回退自愈(thumbnails、
   qq-files 在投毒索引下仍出条目,实测)。
8. **TryGetEntry bounds**:TryLoadCache 的 nameOff/nameLen 检查完整(代码审读+合法文件实测;
   nameOff=UINT32_MAX / nameLen 溢出值均落入拒绝分支)。
9. **假设"index-cache 投毒仅漏扫"被部分推翻**:显示层欺骗(假系统路径)与 walk=1 长期
   存活使影响面上调(见 T-A2);仍无提权执行路径(执行层复验兜底)。
10. **B-1 缓存格式不对称**(review-03 已修,52435a2):本轮 probe 独立复现了"真实缓存头
    错位→永不命中"现象,归属 review-03,此处不重复立项;注意其修复使投毒**跨会话存活**
    (见 T-A2 附注)。

## 修复顺序

- **P0(当天)**:T-A1(还原链:manifest 鉴真 + quarantinePath 限定隔离区 + 确认框);
  T-A3(Finalize FRN 去重/环防护——一个文件挂死整个管理员工具);
  T-A2(缓存 HMAC 或 walk=1 时强制验证 journal 不可用性)。
- **P1(本周)**:T-A4(nameBytes 比例上限);T-A5+T-A12 一起修(ValidateRule 校验
  profileNames/childPattern 展开候选 + SplitNames 保留空格);T-A8(OnOpenLocation 转义);
  T-A6(白名单参数集精确匹配)。
- **P2**:T-A7(InfoOnly path 校验);T-A9(行长上限);T-A10(日志换行转义);
  T-A11(迁移条件);名单扩展(启动文件夹等纳入还原告警)。

## 实验产物

- driver 源码+可执行:`%TEMP%\minisys-review-20261011\05-exp\sandbox.cpp`(9 模式:
  poison-gen/check/loop/huge、rules、selargs、history、loginject、pathsem、probe)
- 编译:`05-exp\build.cmd`(MSVC 18,x64,链接真实 core 副本;副本仅改 AppDataDir 重定向)
- 所有实测输出见本报告各条"复现"行;沙箱数据在 `05-exp\data\`(可清理)
