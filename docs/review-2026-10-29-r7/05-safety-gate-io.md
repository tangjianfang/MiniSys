# r7 分报告 05：危险操作门禁 (B) + 配置与外部 IO (C) 对抗审查

> 日期：2026-10-29 · 对象：master（v2.23 工作树，src/res/build_number.txt 已滚动的未提交态）
> 方法：把真实 core/platform/util 源码（GuardRails / QuarantineOp / OperationLog /
> MoveJunctionOp / JunkRules / DelegateOp / PlanBuilder / SecretStore / RestartManager /
> Junction / Json / PathUtils / StringUtils / Win32Error / Logger）编进对抗 driver
> `.tmp-review\r7\exp\gate7.exe`（11 模式：corpus / fsop / mig / removeall / toctou / sign /
> prodcheck / json / delwl / drvrel / dotnames）。**零产品 stub**；历史经
> `SetHistoryPathForTesting` 重定向、日志经 `SetLogPathForTesting` 重定向；隔离区用
> `subst X:` → `.tmp-review\r7\exp\data` 完全隔离（QuarantineRootFor 按源盘计算，X: 盘的
> 隔离区即沙箱内目录）。真实 HKCU 机密**只读**（键已存在，GetOrCreateMachineSecret 短路于
> 读取）。仓库只读；未启动 MiniSys.exe。
> 工具链：VS2022 Community MSVC 14.44.35207（与仓库 build.bat 同源，非 build.bat）。

## 摘要（Top 风险，按处置优先级）

1. **r7-B1（高|Confirmed，现场已发生）「清空隔离区」被毒化名字永久卡死 + 整盘一刀切标记**：
   真实 `C:\MiniSys.Quarantine` 内残留 r6 探针文件 `tdot.`（尾点名，0 字节，mtime 10-06 21:56，
   实读）。`fs::remove_all` 对尾点/尾空格名返回 **uint64(-1)** + ec="The directory is not
   empty"（driver dotnames 实测），根目录残留；`RunEmptyQuarantine`（SessionService.cpp:929-942）
   见 ec 即把整盘计为失败 → **一条记录都不标 [已释放]** → 状态栏/历史继续显示"隔离区: N 项/2.5GB"。
   这正是今日 02:0x 用户"清空后列表未清空"投诉的门禁侧根因（UI 面 04/07 另查）。且该失败是
   **永久的**：每次重试都在毒名字处失败，同名文件任何同用户进程可经 `\\?\` 前缀植入
   （隔离区根 ACL 继承 Authenticated Users: Modify，r6 T-C4 未修）。
2. **r7-B3（中|Confirmed）还原落点闸的规范化盲区：穿过父目录 junction 的不存在尾段绕过保护判定**。
   Canonicalize（GuardRails.cpp:250-277）对"最终组件不存在"的路径只做 `GetFullPathNameW` +
   最长存在祖先的 `GetLongPathNameW`——**两者都不解析 reparse point**。driver 实测：还原目标
   `X:\...\home3\win\zzdest`（win 是指向 `C:\Windows` 的 junction，zzdest 不存在）通过
   `IsProtectedPath` 判定（未拦），移动仅在**本实验进程非提权**时被 ACL 挡下（Win32 5）；
   **产品以管理员运行，ACL 拦不住 → 隔离内容可落地 C:\Windows 内**。r6"T-B3 深链"假设由
   实验证实。同因也削弱 GuardRails 主闸对"不存在尾段 + 中间 junction"路径的判定。
3. **r7-B4（中|Confirmed）旧记录采纳边界仍有两个可签形状：根精确 + dotdot 逃逸**。
   `IsUnderQuarantineRoot`（OperationLog.cpp:102-107）修好了兄弟前缀（driver：`-evil`/`.evil`
   均拒 ✓），但 `target = X:\MiniSys.Quarantine`（根本身，21 字符边界放行）与
   `X:\MiniSys.Quarantine\..\esc`（dotdot，目标存在）均**被签名采纳**（driver：adopted 3/6）。
   根精确 + 有效签名的记录点一次还原即**把整个隔离区根 MoveFileExW 到 source 位置**（driver
   一次性实做：X: 隔离区根被整体搬到伪造 source `C:\e`；已迁回沙箱，见事故清单）。dotdot 形状
   的还原被 `\\?\` 前缀不折叠 `..` 的意外防线挡住（"找不到该项目"）。
4. **r7-B2（中|Confirmed）盘根系统文件不在保护名单**：`C:\pagefile.sys / hiberfil.sys /
   swapfile.sys / bootmgr / C:\Boot\BCD / Config.Msi / MSOCache` 全部 `IsProtectedPath=free`
   （corpus 实测）。pagefile/hiberfil/swapfile 目前靠"GetFileAttributesW 对锁定的分页文件失败"
   这一**意外防线**被 Validate 拒为"路径已不存在"；`bootmgr`（BIOS 机器）/`Boot\BCD` 存在且
   未锁 → 搜索页/大文件页勾选执行会**真的把它移进隔离区**（本机 UEFI 无 bootmgr，属条件触发）。
5. **r6 遗留确认仍在**：T-B5（Apps 迁移先撞 FileOp 闸，`C:\Program Files\*` 全灭，driver
   FileOp=DENY/Mig=ALLOW 复现）、T-B6（migrateTargetRoot 载入不复查，driver mig：
   `C:\zz-tgt-root`、`C:\Program Files\zz-tgt` Preflight 全 PASS）、T-B7（`C:Windows\System32`
   判定随进程 CWD 翻转，driver drvrel 实测 CWD=C:\ 时 PROT、否则 free）、T-A6（委派白名单
   任意尾缀放行，含 `/ResetBase`、`extra tail`、tab 边界后接 `cmd /c calc`——无 shell，仅
   字面参数，低危但与 r6 结论一致）。

**重大"已验证无问题"**：`fs::remove_all`（MSVC 14.44）**不跟随目录内 junction/中间 junction/
根本身是 junction**（driver removeall A/B/C：victim 全部存活，仅链接被删）——"隔离区内被植入
junction → 清空时删穿系统目录"的灾难场景**不成立**。r6 T-B1/T-B2/T-B3(边界+存在性)/T-B4 修复
全部实证存活（prodcheck：真实 143 条记录 50 条已签名、0 条篡改失配）。

---

## B 节发现清单

### r7-B1（高|Confirmed）EmptyQuarantine：毒化名字永久失败 + 整盘一刀切 [已释放] 标记
- 位置：`MiniSys/src/core/SessionService.cpp:929-942`（fs::remove_all + ec 判定 + 按盘标记）、
  `MiniSys/src/core/QuarantineOp.cpp:107`（隔离区根 create_directories 继承 C:\ ACL，
  r6 T-C4 未修使植入可行）。
- 复现（driver dotnames，实测输出）：
  - 经 `\\?\X:\DOT\tdot.` 等创建 4 个毒名字后 `fs::remove_all(dir, ec)` →
    `removed=184467440737095551615（即 -1 哨兵） ec="The directory is not empty"`，目录残留；
  - 普通路径 `fs::remove("...tdot.")` 静默失败（Win32 名字规范化剥掉尾点后查无此文件）；
  - `DeleteFileW("\\?\...tdot.")` 成功——`\\?\` 前缀是唯一正确删法。
- 现场（只读实查）：`C:\MiniSys.Quarantine` 现存唯一内容 `tdot.`（0 字节，mtime 2026-10-06
  21:56，r6 探针残留），根 mtime 2026-10-29 02:11:06.995——用户 02:0x 的清空把 2.5GB 真实
  内容删掉了，但卡死在 `tdot.`，整盘计失败、零记录标记；`QuarantineUsageText`
  （SessionService.cpp:979-1009）按记录统计 → 状态栏继续显示占用；历史页列表无变化。
- 后果：功能永久 DoS（重试无效）+ "已释放"状态与磁盘事实脱节 + 用户对"唯一不可逆操作"的
  信任受损；攻击面 = 任何同用户进程（或恶意解压包）经 `\\?\` 在隔离区根植一个尾点/尾空格名。
- 修复草案：①EmptyQuarantine 改用 `\\?\` 前缀的显式枚举删除（FindFirstFileW + DeleteFileW/
  RemoveDirectoryW，原生名字不规范化），或删前把根 `rename` 为 `<root>.emptying-<时间戳>`
  再 remove_all 新根（同卷 rename 原子，失败可重试且不影响后续新隔离）；②按"目标是否仍在磁盘"
  而非按盘整体成败标记记录；③隔离区根创建后收紧 ACL（同 r6 T-C4）。
- 置信度：Confirmed（机制实测 + 现场残留物证 + 时间线吻合）。

### r7-B2（中|Confirmed）盘根系统文件/引导文件不在保护名单
- 位置：`MiniSys/src/core/GuardRails.cpp:75-103`（Policy 表只有目录型条目 + Users 精确 +
  盘根）；无按文件名的保护；Validate 亦无 HIDDEN/SYSTEM 属性闸（GuardRails.cpp:420-436）。
- 复现（corpus + fsop，实测）：`C:\pagefile.sys` 等 8 个路径 `IsProtectedPath=free`；
  Validate(FileOp) 对 pagefile/hiberfil/swapfile 拒绝的原因是 `路径已不存在`——即
  `GetFileAttributesW` 对运行中被锁定的分页文件返回 INVALID（FileAttributesOf，
  GuardRails.cpp:361-363），**属意外防线而非策略**；对不存在的 `C:\bootmgr` 同样报"不存在"。
  扫描侧：FastWalk/LargeFileScanner 不过滤 HIDDEN/SYSTEM（FastWalk.cpp:138-149 仅跳 reparse），
  搜索索引（USN 全卷枚举）天然包含它们，SearchAsync 给任意结果 riskLevel=Cautious
  （SessionService.cpp:621）→ 可勾选执行。
- 后果：BIOS/双启机器上 `bootmgr`、`C:\Boot\BCD` 可被移进隔离区 → 下次启动失败；UEFI 机器
  实际暴露面主要是误导性失败（pagefile 移动 Win32 5）。产品文案承诺"系统目录一律拒绝"，
  盘根系统文件是承诺外的灰色带。
- 修复草案：fileExact 增加 `%SystemDrive%\pagefile.sys|hiberfil.sys|swapfile.sys|bootmgr、
  C:\Boot、C:\Config.Msi、C:\MSOCache`（按存在性生成）；或 Validate 对
  HIDDEN|SYSTEM 且位于盘根前两层的文件要求 dangerous 二次确认。
- 置信度：Confirmed（判定实测；bootmgr 实害为条件性——本机无此文件，未实测移除真机引导文件）。

### r7-B3（中|Confirmed）Canonicalize 祖先解析不穿 junction → 还原落点/不存在尾段判定盲区
- 位置：`MiniSys/src/core/GuardRails.cpp:250-277`（fallback：GetFullPathNameW +
  GetLongPathNameW(最长存在祖先)，均不解析 reparse）；消费方 `QuarantineOp.cpp:148`
  （UndoPaths 的 restoreTo 闸）、GuardRails.Validate 步骤 1。
- 复现（driver sign case B/D，实测）：restoreTo=`X:\...\home2\proj\file.txt`（proj→沙箱
  "system" 目录的 junction）→ 闸放行 → 移动成功，内容**经 junction 落到原路径之外**
  （landed-through-junction=YES）；restoreTo 经 `home3\win`（junction→**真** `C:\Windows`）
  + 不存在尾段 → `IsProtectedPath` 仍放行（link 字符串不匹配任何保护条目），移动仅因实验进程
  非提权被 ACL 拒（Win32 5）——**产品提权运行时此防线不存在**。对照：直接 `C:\Windows\zzdest`
  与已存在的最终路径都能正确拒绝（canonicalize 主路径走 GetFinalPathNameByHandleW 解析
  reparse ✓）。
- 触发条件：同用户攻击者把某条已签名待还原记录的 source **父目录**替换为 junction（源内容
  已在隔离区，父目录通常可写）+ 用户点还原；或任何"最终组件不存在 + 中间是 junction"的
  待删路径。r6"T-B3 深链/undo 目的地过 GuardRails"建议已实施一半（restoreTo 过闸了），但闸
  本身在这个形状下失明。
- 修复草案：fallback 分支找到最长存在祖先后，用 `CreateFileW(祖先, 0,
  FILE_FLAG_BACKUP_SEMANTICS)`（不带 OPEN_REPARSE_POINT，允许穿越）+
  `GetFinalPathNameByHandleW` 解析祖先真实路径再拼尾段；或对"祖先链中含 reparse point 且
  尾段不存在"的输入直接 deny-by-default。
- 置信度：Confirmed（driver 两形状实测；对真 C:\Windows 的写被非提权 ACL 挡属环境限制，
  提权下无阻的结论由 ACL 语义推出，未提权实测——见局限）。

### r7-B4（中|Confirmed）SignUnsignedLegacy 采纳边界：根精确与 dotdot 形状可签
- 位置：`MiniSys/src/core/OperationLog.cpp:102-107`（IsUnderQuarantineRoot）、`:265-289`
  （SignUnsignedLegacy）、`MiniSys/src/core/QuarantineOp.cpp:133-165`（UndoPaths 无
  quarantinePath 子树校验）。
- 复现（driver sign，实测）：6 条伪造无签名记录中 f1（正规）✓ 采纳；f2/f3（兄弟前缀）拒 ✓；
  f6（根下不存在）拒 ✓（r6 T-B2/T-B3 修复实证）；**f4 `X:\MiniSys.Quarantine\..\esc`（目标
  存在）采纳**、**f5 `x:\minisys.quarantine`（=隔离区根本身）采纳**。f5 + 有效签名 →
  `UndoPaths` 成功把**整个隔离区根** MoveFileExW 到伪造 source（driver 一次性实做，根被搬到
  `C:\e`，内容完好，已迁回沙箱）；f4 的还原被 `\\?\` 不折叠 `..` 的意外防线拒
  （"隔离区中找不到该项目"）。
- 触发前提：管理员执行一次"信任并签名旧记录"（批量，无逐条确认）+ 用户点还原；伪造
  history.jsonl 需同用户写权限（AppData 可写，成立）。
- 修复草案：①采纳前对 target 做 `GuardRails::Canonicalize` 并要求**严格位于**
  `<盘>:\MiniSys.Quarantine\` 之下（≥1 个子组件、规范化后无 `..`、非根本身）；②`UndoPaths`
  对 quarantinePath 做同样子树校验（纵深）；③采纳改逐条列出 id/source/target 确认。
- 置信度：Confirmed（每环真实代码实测）。

### r7-B5（低|Confirmed）QuarantineOp::Execute 的"纵深复验"缺 reparse 复查（TOCTOU 窗口）
- 位置：`MiniSys/src/core/QuarantineOp.cpp:87-127`（只查存在性 + IsProtectedPath）；
  对照 `GuardRails.cpp:427-430`（Validate 查 FILE_ATTRIBUTE_REPARSE_POINT）。
- 复现（driver toctou，实测）：Validate 对合法目录放行 → 目录被换成指向 victim 的 junction →
  Validate 复验正确拒绝（reparse）✓；但直接调 `QuarantineOp::Execute`（纵深路径）**不拒**，
  把 junction 链接本身移进隔离区（victim 完好，链接从原位消失）。
- 后果评估：MoveFileExW 是改名不跟随 → 目标内容无损；危害=原路径目录"消失"（可再还原 junction）
  + 隔离区内出现链接。低危，但与 GuardRails 注释声称的"safe standalone"不完全一致。
- 修复草案：Execute 开头补 `IsReparsePoint(source_)` 拒绝（一行，与 Validate 对齐）。
- 置信度：Confirmed。

### r7-B6（低|Confirmed，r6 遗留复核）Startup 等自启动位置不在保护名单
- 位置：`GuardRails.cpp:75-103`（fileSubtree 无用户级自启动目录）；`QuarantineOp.cpp:148`
  只查保护名单。
- 实测：`IsProtectedPath(C:\Users\tjf\...\Start Menu\Programs\Startup)=free`，
  Validate(FileOp)=ALLOW。隔离该目录内容属"用户可清理区"的设计选择，但**还原方向**配合 r7-B3
  可把隔离内容写进 Startup（签名记录 + junction 父目录）。维持 r6 建议：至少在 undo 落点为
  Startup/Run 相关目录时告警。
- 置信度：Confirmed。

### 已知遗留复核（r6 立项、本轮实测确认仍在，不另计新发现）
| 项 | 本轮证据 | 位置 |
|---|---|---|
| T-B5 Apps 迁移先撞 FileOp 闸 | driver fsop：`C:\Program Files\Windows NT`/`7-Zip` FileOp=DENY、Mig=ALLOW；RunPlan 先跑默认 FileOp | SessionService.cpp:846+853 |
| T-B6 migrateTargetRoot 载入不复查 | driver mig：`C:\zz-tgt-root`、`C:\Program Files\zz-tgt` Preflight PASS；MainWindow.cpp:2325 直接采用（对话框路径 2470 有查、载入路径无查） | MainWindow.cpp:2323-2326 |
| T-B7 盘符相对路径 CWD 依赖 | driver drvrel：CWD=exp 目录 → free；CWD=C:\ → PROT | GuardRails.cpp:203-208 仅 2 字符特判 |
| T-A6 委派白名单尾缀 | driver delwl：`/ResetBase`、`/a extra`、`extra tail`、`/Cleanup-Image\tcmd /c calc` 全 ALLOW（无 shell，字面参数） | JunkRules.cpp:168-229 |
| T-C4 隔离区根 ACL 过宽 | 本轮未重测 ACL；r6 实读 Authenticated Users: Modify（r7-B1 的植入前提） | QuarantineOp.cpp:107 |

### r6 已修项存活核验（本轮实证）
| 项 | 证据 |
|---|---|
| T-B1 HMAC flag + HKCU 机密 | SecretStore.cpp:81-82 有 `BCRYPT_ALG_HANDLE_HMAC_FLAG`；driver：hmac len=64 确定性；**prodcheck：真实 history 143 条中 50 条带签名全部验签通过、0 篡改**；今日 01:58 的 25 条真实 QUARANTINE 均有 `_sig` |
| T-B2/T-B3 采纳闸（边界+存在性） | driver：兄弟前缀/根下不存在目标均拒（OperationLog.cpp:102-107 + :279 exists 检查在位） |
| T-B4 状态翻转重签 | driver：UpdateStatus/UpdateStatusBulk 后验签 YES（OperationLog.cpp:304-309/328-333） |
| T-C2（未修，r6 已知） | driver：篡改 timestamp / reversible 后**验签仍通过**（SignedFieldsOf 未覆盖两字段，OperationLog.cpp:90-95）——维持低危开放 |

---

## C 节发现清单

### r7-C1（中|Confirmed）= r7-B1 的 C 面投影
EmptyQuarantine 的失败语义：部分删除已发生（2.5GB 已释放）但记录全不标记 → `已释放字节/
隔离区占用`两组账面数字与磁盘永久脱节；`remove_all` 返回值被忽略（uint64(-1) 未参与判定）。
修复随 r7-B1。

### r7-C2（低|Confirmed）settings.json：未知键不往返、保存无 fsync、错误静默
- 位置：`MiniSys/src/core/Settings.cpp:33-51`（Load 只读已知键）、`:54-81`（Save 只写已知键，
  tmp+rename ✓ 原子，但 `MoveFileExW` 返回值忽略、写后无 flush/fsync——崩溃可留空 tmp 或
  半新文件；半新文件能被解析则丢字段，不能则整文件回默认）。
- 实测：Json 解析层 30 坏样本（见语料附录）零崩溃；布尔写字符串→默认 false、字符串 null→空、
  数组写字符串→非数组、根数组→非对象 Load 直接走默认且**不覆盖原文件** ✓；BOM 剥除 ✓（v2.2
  修复存活）；`"largeFilesMinMB":200.9`→201（llround）；`lastTab:-7/99999`→MainWindow.cpp:2339
  边界钳制 ✓。
- 未知键（如未来版本字段、用户手加注释键）在下次 Save 时**静默丢弃**——降级安装会丢配置。
- 修复草案：Save 前 Load 原文件保留未知键合并写出；检查 MoveFileExW 返回并告警日志。
- 置信度：Confirmed（代码读 + json 模式实测；Save 行为为代码读，未对真实文件写测）。

### r7-C3（低|Confirmed）Json 解析器宽松点与数值精度（整体健康）
- 30 坏样本无崩溃、无挂死、错误可读（附录）；拒绝尾逗号/单引号/注释/前导+/hex/NaN/
  65 层深（kMaxDepth=64 ✓）。
- 三个残余宽松点：①**字符串接受原始控制字符**（`"b\x01c"` 解析通过，JSON 规格应拒）；
  ②`\u0000` 注入 NUL 后 `AsString()` 长度 9 / `c_str()` 长度 4——路径字段带 NUL 下游
  Win32 调用统一截断（显示与动作不一致但方向一致，r6 T-C3 建议的 JsonToScanItem 拒 NUL
  **未实施**，SessionService.cpp:154-177 无 NUL 检查）；③`9007199254740993` →
  `9007199254740992`（double 精度，size 字段失真 ±1，化妆性问题）。
- 置信度：Confirmed（json 模式逐条实测）。

### r7-C4（低|Confirmed）history.jsonl：重写回退非原子、无 fsync、重放不查
- 位置：`MiniSys/src/core/OperationLog.cpp:168-183`（RewriteAtomic：rename 失败 → **直接
  覆盖写**回退，崩溃窗口内可截断整文件；恢复=逐行跳过坏行，尾部记录丢失）、`:237-253`
  （Append 无 flush/fsync，断电丢最后若干条）。
- 损坏行恢复实测 ✓：坏行跳过、好行保留（3/3）；截断尾行丢弃；`history.tsv` 混入行按 TSV
  解析兜底 ✓。UpdateStatusBulk 全量重写一次（O(记录数)，143 条毫秒级；r6 的 267MB 问题
  是 index-cache，与 history 无关——核实无回归）。
- 重放实测：**同一签名行复制后两条都验签通过**（无序号/防重放，HMAC 语义如此；影响=重复
  还原请求，第二次被"找不到该项目"兜住，可接受，记录在案）。
- 修复草案：回退路径改为先备份再写；Append 后 flush；可选：VerifySignature 处加
  (id,timestamp) 唯一性检查。
- 置信度：Confirmed。

### r7-C5（低|设计说明）HKCU 机密的威胁模型与写路径
- 位置：`MiniSys/src/platform/SecretStore.cpp:22-71`。
- 实况：HKCU\SOFTWARE\MiniSys\RecordKey 存在（只读实查）；driver 读到 32 字节并正确产出
  HMAC。ACL 面为 HKCU 默认（同用户完全访问）——**签名链防的是"其他用户/意外篡改"，防不了
  同用户恶意进程**（可读 key 重签任意记录）。这是 v2.20 有意选择的范围，报告存档，不计新漏洞。
- `WriteSecret`（:45-57）仍不检查 `disp==REG_CREATED_NEW_KEY`（r6 T-C5 建议未做；HKCU 范围
  下抢注面=同用户，低）。
- 降级路径：机密读不到 → Append 不签名 → UndoRecord 拒绝（fail-closed ✓，driver sign 的
  空机密路径与代码读一致）。
- 置信度：Confirmed。

### r7-C6（info|实测）rules.json / 缓存 / dump / 隐私核对
- **rules.json**：代码只读不写（grep 全源码无写点）——**无"首次播种覆盖"风险**；文件在 exe
  目录，坏/缺 → builtin 回退；逐条 ValidateRule（JunkRules.cpp:232-298）把非隔离策略锁死在
  内置路径、quarantine 规则锁死在用户区/豁免根、profileNames 拒绝 `:`/`\`/`..`（r6 修复存活）。
  用户改坏全部规则 → 规则表空（自担，r6 已知）。
- **results-\*.json**：SaveResultsCache tmp+rename ✓；损坏 → parse fail → 不加载（优雅降级，
  SessionService.cpp:256-260）；Delegate 项加载时二次过白名单（:278-295，T-B1 缓存修复存活）；
  上限 5000 条。残留：tmp 写失败（盘满）仍 rename 半截文件——下次解析失败自愈，低。
- **index-cache-C.bin**：plain v2 无 MAC（v2.16d 撤除，r6 T-C1 建议复核未做）；读侧上限
  kMinCachedEntries=10000/kMaxNameBytes=768MB/kMaxNameLen=1024/FRN 环预算（VolumeIndex.cpp:
  227/1159-1161/1207-1209/1242）存活；写侧 tmp+remove+rename，失败保旧。
- **minidump**：MiniDumpNormal（最小集，CrashDump.cpp:56）；保留 5 个 .dmp（:25-40，只删
  .dmp）；真实 dumps/ 仅 1 个 10-12 的 106KB 旧 dump（实查）。**未做字符串级敏感扫描**（局限）。
- **隐私**：真实 minisys.log 本周期 0 条完整用户路径（grep `C:\Users` = 0）；history.jsonl/
  results-\*.json 按功能必需含全路径；搜索词经 `SearchAsync enter: query='%s'` 落日志
  （SessionService.cpp:585）；委派失败时子进程输出尾 2000 字符进 history note。均同用户 ACL
  可读，本地工具可接受，存档。
- **外部命令**（DelegateOp.cpp 实测+读码）：无 shell、exe 白名单全限定路径+引号包裹、
  CREATE_NO_WINDOW ✓、Job Object KILL_ON_JOB_CLOSE 杀树 ✓、30min 上限 + 500ms 轮询 ✓、
  读管道独立线程 + 64KB 上限（保留尾 32KB）✓、EOF 3s+2s 超时后 `reader.detach()` +
  `CloseHandle(outRead)` ——**r6 B-7 的分离线程句柄竞态仍在**（DelegateOp.cpp:153-163，detach
  后线程可能仍在 ReadFile 已关闭句柄；触发需孙进程死握管道 >5s，低概率）；调用线程=
  SessionService worker（StartTask 内 CoInitializeEx STA）。尾缀注入面见 T-A6。
- **MiniSysTests main.cpp**：重定向了 Logger 与缓存目录，但 **OperationLog 无全局重定向**
  （main.cpp:23 注释"per-test"）——现有触历史测试各自重定向 ✓，但任何未来测试调
  QuarantineOp/MoveJunctionOp/DelegateOp 的 Execute 而忘设路径，就会像本轮 driver 事故一样
  **直接写用户真实 history.jsonl**（见事故清单与测试草案 #9）。
- 置信度：除标注外均 Confirmed（读码 + 只读实查 + delwl/json/driver 实测）。

---

## 门禁绕过统计

| 语料组 | 条数 | 拦截正确 | 放行正确 | 命中缺陷 |
|---|---|---|---|---|
| GuardRails 路径变体（大小写/尾斜杠/`\\?\`/UNC/正反斜杠/双斜杠/尾点尾空格/dotdot/8.3/NTFS 流/盘根/裸盘符/穿透 junction） | 29 | 27 | 2（穿透 junction 拦 ✓、junction 本身 free 属 Validate 兜底） | 0 |
| 系统目录判定（Windows/WinSxS/System32/SysWOW64/PF×2/ProgramData/PerfLogs/$Recycle.Bin/SVI/$WinREAgent/Recovery/Windows.old/Users 根/隔离区自身/豁免穿透+逃逸） | 18 | 18 | 0 | 0 |
| 盘根系统文件（pagefile/hiberfil/swapfile/bootmgr/Boot/Config.Msi/MSOCache/Intel/Startup） | 10 | 0 | 3（按设计 free：Intel/Startup/用户区） | **r7-B2**（7 个应拦而 free） |
| Validate 上下文（FileOp vs Migration ×9 路径） | 9 | 6 | 1 | T-B5（2 组误拒）、r7-B2（pagefile 系"意外拦"） |
| 迁移 Preflight 目标根 | 5 | 1 | 1 | T-B6（3 个 C: 目标 PASS） |
| remove_all reparse 语义（junction 内嵌/根为 junction/中间 junction/symlink/对照） | 5 | — | 5（victim 全存活） | **0（重要阴性）**；is_symlink(junction)=false 记录在案 |
| 毒化名字 vs remove_all | 4 名 | 0 | 0 | **r7-B1**（-1 + 目录残留） |
| TOCTOU（换 junction 前后 ×4 步） | 4 | 2（Validate 两查） | 0 | r7-B5（Execute 纵深缺 reparse） |
| 签名链（roundtrip/7 字段篡改/剥 sig/重放/坏行/截断） | 13 | 10 | 1（重放双过=设计） | T-C2（ts/reversible 未覆盖） |
| SignUnsignedLegacy 采纳 | 6 | 3 | 1（f1 正规采纳） | **r7-B4**（f4/f5 可签） |
| 撤销边缘（dest 存在/父 junction/真 C:\Windows/junction→C:\Windows/已释放/FAILED） | 6 | 3 | 1 | r7-B3（2 个穿透形状仅靠环境/意外拦） |
| Json 坏样本 | 30 | 13 拒+11 限 | 6 | r7-C3（控制字符/NUL/精度，无崩溃） |
| 委派白名单 | 21 | 10 | 8 | T-A6（尾缀 3 条） |
| **合计** | **165** | — | — | 新增 6（r7-B1..B6 有效 5 + C1 联动）+ 遗留确认 4 |

### 语料附录（输入 | 当前判定 | 应判）
见 `.tmp-review\r7\exp\out_corpus.txt / out_fsop.txt / out_mig.txt / out_removeall.txt /
out_toctou.txt / out_sign.txt / out_prodcheck.txt / out_json.txt / out_delwl.txt /
out_dotnames.txt`（GBK 控制台输出，需 iconv）。代表性条目：
- `C:\pagefile.sys | free（Validate 靠 attr 失败误拦）| 应 PROT`
- `C:\bootmgr、C:\Boot\BCD、C:\Config.Msi、C:\MSOCache | free | 应 PROT（条件）`
- `C:\Program Files\7-Zip × FileOp | DENY | 迁移场景应 ALLOW（T-B5）`
- `C:\Program Files\zz-tgt（迁移目标）| Preflight PASS | 应拒（T-B6）`
- `X:\...\home3\win\zzdest（win→C:\Windows junction）| 还原放行 | 应拒（r7-B3）`
- `target=X:\minisys.quarantine（根本身）| 采纳+可还原整根 | 应拒（r7-B4）`
- `target=X:\MiniSys.Quarantine\..\esc | 采纳（还原被意外防线拦）| 应拒（r7-B4）`
- `dir 含 "tdot." | remove_all=-1 残留 | 应可清空（r7-B1）`
- `qroot 内嵌 junction→victim | victim 存活 ✓ | 正确（阴性）`
- `Dism.exe /Online /Cleanup-Image /ResetBase | ALLOW | 白名单尾缀应收紧（T-A6）`

## 已验证无问题 / 被证伪的假设

| # | 项目 | 结论 |
|---|---|---|
| 1 | **"隔离区内植入 junction → 清空时 remove_all 删穿目标"**（本轮任务书点名） | **证伪**——MSVC 14.44 remove_all 不跟随内嵌/中间/根本身的 junction，victim 全存活（3 组实测）。注意：directory_iterator 对 junction 报 `is_symlink=false, is_directory=true`（实测），即 STL 不把 junction 当符号链接——不跟随是 remove_all 自身实现行为，**依赖工具链版本**（本机=产品构建链 14.44.35207），升级 VS 后应回归本测试 |
| 2 | "EmptyQuarantine 对根为 junction 的目标会跟随删除" | 证伪（removed=1，仅链接） |
| 3 | "签名链修复只在代码里，生产未生效" | 证伪——真实 history 143 条，50 条带 `_sig` 全部验签通过、0 篡改（prodcheck，真实 HKCU 机密只读参与） |
| 4 | "r6 T-B2/T-B3 采纳闸修复后旧记录可安全采纳" | 部分证伪——兄弟前缀/不存在目标确实拒了，但根精确与 dotdot 两形状仍可签（r7-B4） |
| 5 | "还原目标已过 GuardRails（r6 建议）因此深链已闭合" | 证伪——r7-B3 形状（父 junction + 不存在尾段）下闸失明 |
| 6 | 27+2 个路径变体/系统目录（含 `\\.\C:` 类、ADS、8.3、dotdot 死尾、UNC、尾点尾空格） | 全部正确（r6 结论在本版本存活） |
| 7 | 豁免目录逃逸（`C:\Windows\Temp\..\System32`） | 正确拦截（canonicalize 折叠 dotdot） |
| 8 | 用户排除清单前缀绕过（plain2 兄弟前缀） | 无（child 命中需 `\` 边界 ✓） |
| 9 | Json 30 坏样本崩溃/挂死 | 无（含 65 层深、截断、BOM、控制字符） |
| 10 | "settings 坏样本会覆盖原文件" | 无（整文件坏→默认值，不回写；代码读） |
| 11 | rules.json 首次播种覆盖 | 无此面（代码不写该文件） |
| 12 | pagefile.sys 会被立即移进隔离区 | 现机不成立——attr 查询失败先拦（"路径已不存在"）；但这是意外防线（r7-B2） |
| 13 | 委派命令注入（引号逃逸/相对路径/UNC/SysWOW64/尾点 exe/cmd.exe） | 全拒 ✓；仅尾缀类放行（T-A6，无 shell） |

## 加固建议与测试草案（可直接追加到 MiniSysTests）

```cpp
// 1) [P0·r7-B1] 毒化名字不阻断清空（当前必 FAIL）
TEST(EmptyQuarantine, PoisonNamesDoNotBlockRemoval) {
    fs::path root = TempBase() / L"qroot"; fs::create_directories(root);
    Touch(root / L"a.txt");
    CreateFileW((L"\\\\?\\" + (root / L"tdot.").wstring()).c_str(), ...);
    std::error_code ec;
    EXPECT_TRUE(EmptyQuarantineRootViaVerbatim(root));   // 待实现的 \\?\ 枚举删除
    EXPECT_FALSE(fs::exists(root, ec));
}
// 2) [P0·r7-B3] Canonicalize 穿透父 junction 的不存在尾段
TEST(GuardRailsAdv, FallbackResolvesAncestorThroughJunction) {
    auto j = MakeJunction(sandbox/"win", L"C:\\Windows");
    EXPECT_TRUE(GuardRails::IsProtectedPath(sandbox/L"win"/L"zz-not-exist"));
}
// 3) [P0·r7-B4] 采纳拒绝根精确与 dotdot；UndoPaths 拒绝根本身
TEST(TrustChain, AdoptionRejectsRootExactAndDotdot) {
    SeedUnsigned({target: L"X:\\MiniSys.Quarantine", target: L"X:\\MiniSys.Quarantine\\..\\e"});
    EXPECT_EQ(0, OperationLog::Instance().SignUnsignedLegacy());
}
// 4) [P1·r7-B2] 盘根系统文件入保护表
TEST(GuardRailsAdv, RootSystemFilesProtected) {
    for (auto* p : {L"C:\\pagefile.sys", L"C:\\hiberfil.sys", L"C:\\swapfile.sys",
                    L"C:\\bootmgr", L"C:\\Boot", L"C:\\Config.Msi", L"C:\\MSOCache"})
        EXPECT_TRUE(GuardRails::IsProtectedPath(p)) << p;
}
// 5) [P1·r7-B5] QuarantineOp::Execute 拒绝 reparse（TOCTOU 纵深）
TEST(QuarantineOp, ExecuteRejectsReparsePoint) { /* junction 作 source → Execute=false */ }
// 6) [P1] T-C2 收口：篡改 timestamp/reversible 必须验签失败（改 SignedFieldsOf 后启用）
// 7) [P1] T-A6 尾缀：DelegateCommandAllowed 拒绝白名单前缀后的额外 token（枚举合法完整命令集）
// 8) [P2] Json 拒绝字符串内原始控制字符与 \u0000 路径（JsonToScanItem/ValidateRule 双处）
// 9) [P2·防再犯] MiniSysTests main.cpp 增加 OperationLog 默认重定向：
//    OperationLog::SetHistoryPathForTesting(tmp/"history-default.jsonl");
//    （任何未来测试忘设路径时兜底，杜绝写真实 %LOCALAPPDATA%）
// 10) [P2] remove_all 语义回归钉：junction 内嵌/中间/根本身三形状 victim 存活断言
//     （工具链升级时的 STL 行为哨兵）
```

## 修复顺序

- **P0（当天）**：r7-B1（`\\?\` 枚举删除 + 按磁盘实况标记 + 隔离区根 ACL 收紧/撤继承）——
  用户功能当前即坏；顺手清掉真机 `C:\MiniSys.Quarantine\tdot.`（用 `\\?\` 前缀删）。
- **P1（本周）**：r7-B3（Canonicalize 祖先穿透或 deny-by-default）、r7-B4（采纳/UndoPaths
  子树校验 + 逐条确认）、r7-B2（fileExact 补盘根系统文件）、r7-B5（一行 reparse 复查）。
- **P2**：T-C2 签名字段扩容、T-A6 尾缀枚举收紧、Json 控制字符/NUL 拒绝、settings 未知键
  合并回写、RewriteAtomic 回退路径去直接覆盖、T-B6/T-B7（顺位保持 r6 建议）、测试 main.cpp
  的 OperationLog 默认重定向、B-7 reader detach 窗口。

## 实验对用户数据的意外写入清单（主审要求单列；已写入项未自行清理）

| # | 时间 | 内容 | 来源 | 处置 |
|---|---|---|---|---|
| 1 | 2026-10-29 02:29:26.207 | 真实 `%LOCALAPPDATA%\MiniSys\history\history.jsonl` 追加 1 行（+283 字节，39450→39733）：`{"_sig":"47fa136d…","id":"20261029-022926-0","size":1,"source":"X:\\TOCTOU\\swapme","status":"SUCCESS","target":"X:\\MiniSys.Quarantine\\swapme","type":"QUARANTINE"}` | **本 driver（toctou 模式）**——QuarantineOp::Execute→OperationLog::Instance().Append 走了默认真实路径，当时该模式未重定向 | **未清理**（按指令保留待主审处置）。driver 已于 02:30 修复：main() 无条件 `SetHistoryPathForTesting(沙箱)`，复跑验证真实文件 mtime 不再变化 |
| 2 | 2026-10-29 02:31 | 真实 `C:\e` 目录被创建（内容=沙箱隔离区根整体：legit\ + 2 个 junction） | **本 driver（sign 模式 f5 根精确采纳 + UndoPaths 实做）**——这本身是 r7-B4 的证据 | 已于 02:36 用 `move C:\e …\.tmp-review\r7\exp\data\moved_by_undo_f5` **原样迁回沙箱**（纯改名，无删除），C:\ 根恢复原状 |
| 3 | 2026-10-29 02:11:06 | 真实 history.jsonl 的 `{"id":"20261029-021106-5","source":"C:\\some\\file.txt","type":"DELETE","target":"C:\\MiniSys.Quarantine\\x"}` 行 + `C:\MiniSys.Quarantine` 根 mtime 02:11:06.995 | **非本 driver**——我的第一个可执行文件 02:25:53 才产出（out_corpus.txt mtime 为证），且我在 02:0x 源码审阅阶段已在真实文件里读到该行。疑似另一并行进程/子代理的探针 | 未清理，报主审核查（该行带有效签名，写入者用了真实 HKCU 机密） |
| 4 | — | 真实 minisys.log（mtime 02:00:40 未变）、C:\MiniSys.Quarantine、settings.json、缓存文件 | 本 driver 全程未触碰（Logger/历史/缓存全部重定向；隔离区走 subst X:） | — |

## 未验证 / 局限

- 未启动 MiniSys.exe；UI 侧（历史页显示、还原按钮链）由代码+日志推断。
- r7-B3 的"提权下必成"结论：实验进程非提权，对 C:\Windows 的最终写入被 ACL 拒（Win32 5）；
  提权下无此 ACL 阻力的判断基于 Windows ACL 语义推理，未提权实测（约束禁止）。
- remove_all 结论绑定 MSVC STL 14.44.35207；其他工具链/未来 STL 升级未测（已给回归钉 #10）。
- symlink（非 junction）形状：本上下文无 SeCreateSymbolicLinkPrivilege（Win32 1314），
  A2 用例跳过；junction 语义与 dir symlink 在 NTFS reparse 层同构，风险外推标注假设。
- r7-B2 未对真机引导文件做实际移动验证（不做破坏性动作）；bootmgr 危害为条件性（BIOS/双启）。
- minidump 未做字符串级敏感信息扫描（strings 工具缺）。
- 真实隔离区/历史只读约束下，未能验证"用户 02:0x 那次清空"的完整交互（仅凭残留物证+机制
  复现还原因果链；UI 侧由 04/07 报告交叉）。
- DelegateOp 未活体运行（白名单/拼接经 JunkRules::DelegateCommandAllowed 全量语料静态实调；
  CreateProcess 行为为代码读）。

## 实验产物

- driver 与构建脚本：`.tmp-review/r7/exp/{gate7.cpp, build.cmd}`；可执行 `gate7.exe`（11 模式）。
- 输出：`out_{corpus,fsop,mig,removeall,toctou,sign,prodcheck,json,delwl,drvrel,dotnames}.txt`（GBK）。
- 沙箱：`.tmp-review/r7/exp/data/`（含 moved_by_undo_f5、hist_default 等）；`subst X:` 已解除。
- 真实用户数据：除上表 #1/#2 外零写入；#1 行未清理待主审处置。
