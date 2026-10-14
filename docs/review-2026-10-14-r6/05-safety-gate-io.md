# r6 分报告 05：危险操作门禁 (B) + 配置与外部 IO (C) 对抗审查

> 日期：2026-10-14 · 对象：master `2a9d653`（v2.19a）工作树
> 方法：把真实 core/platform/util 源码（GuardRails/JunkRules/OperationLog/QuarantineOp/
> MoveJunctionOp/DelegateOp/PlanBuilder/Junction/Json/PathUtils 等）编进对抗 driver
> （`.tmp-review\r6\exp\gate6.exe`，9 模式，corpus/sign/undo/undo2/updstat/json/cmd/mig/migundo）。
> 仅两个本地 stub：SecretStore（固定测试密钥，用于诊断产品 HMAC——见 T-B1）、Logger（不落盘）。
> 历史经内置 `SetHistoryPathForTesting` 重定向到沙箱；真实 `%LOCALAPPDATA%\MiniSys` 只读
> （history.jsonl/日志副本/ACL 查询）；C:\ 根探针目录已全部清理；仓库只读、未启动 MiniSys.exe。

## 摘要（Top 风险按处置优先级）

1. **T-B1（严重|Confirmed）HMAC 信任链自 v2.14 从未工作过**：`SecretStore.cpp:80`
   `BCryptOpenAlgorithmProvider(...,0)` 缺 `BCRYPT_ALG_HANDLE_HMAC_FLAG`，带 pbSecret 的
   `BCryptCreateHash` 恒返回 `0xC000000D`（STATUS_INVALID_PARAMETER，本机 Win11 26200 实测：
   无标志 objLen=326 失败 / 有标志 objLen=422 成功）。后果链：**所有历史记录 0 条有签名**
   （真实 history.jsonl 实测 93 条 0 个 `_sig`，HKLM 密钥存在且 ACL 拒读=提权端拿到密钥也签不出）→
   历史页全部显示"未签名"、**全部还原被拒**（`UndoRecord` 双层闸全挡）→ 产品承诺
   "隔离区可逆"自 v2.14 起实际失效 4 个版本未被察觉；v2.16d"262MB 写失败"砍掉缓存 MAC 是
   对本 bug 的误诊。当前方向 fail-closed（无提权面），但信任链=0。
2. **T-B2（高|Confirmed）`SignUnsignedLegacy` off-by-one 死代码**：`OperationLog.cpp:265`
   `t.compare(3,17,L"minisys.quarantine")` 对 18 字符字面量永不相等 → 采纳恒为 0
   （driver：8 条语料含"完全正规形状"全被拒）。升级路径（Ctrl+K"信任并签名旧记录"）从未工作。
   T-B1 修复后，既有 93 条记录将**永远**无法恢复可撤销，必须与 T-B1 同批修。
3. **T-B3（高|Confirmed，修复 T-B2 后即活）legacy 采纳检查可构造绕过 → 还原落盘链**：
   兄弟前缀 `C:\MiniSys.Quarantine-evil\payload`（无 21 位边界）、`C:\` 根非提权可建目录（实测）、
   真隔离区继承 `Authenticated Users: Modify`（ACL 实读）、restoreTo 只查保护名单而
   **Startup 文件夹不在名单**（实测 `IsProtectedPath(real Startup)=no`）。driver `undo2`
   全链实证：伪造记录→有效签名→eligible→`UndoPaths`→**payload 落地成功**（每环真实代码）。
4. **T-B4（中|Confirmed）`UpdateStatus` 不重签**：状态/备注变更后 RewriteAtomic 保留旧
   sig→HMAC 失配→已还原记录永久显示"被篡改"且无法再采纳（driver：verify=no、re-adopt=0）。
   T-B1 修复后立即显性化。
5. **T-B5（中|Confirmed）应用迁移自 v2.0 被第一道 FileOp 闸整体拒绝**：
   `SessionService.cpp:747` 对所有文件级操作先跑默认 `Context::FileOp`，Apps 表
   `C:\Program Files\*` 源全部"受保护系统路径"跳过（driver：FileOp allow=no / Migration
   allow=YES）。fail-closed 功能性故障（真实日志无迁移尝试故未察觉）。

## B 节发现清单

### T-B1（严重|Confirmed）HMAC 从未计算成功——整个签名信任链空转
- 位置：`MiniSys/src/platform/SecretStore.cpp:76-108`（HmacSha256 构造）、`:79-80`（缺标志）。
- 复现（driver diag，实测输出）：
  - `[stub] plain st=0xc000000d(objLen=326)  HMAC-flag st=0x0(objLen=422)`
  - `[diag] secret-size=32 hmac-len=0 streaming-ok=no`（产品同款代码路径）
  - 加标志后 `hmac-len=64 streaming-ok=YES`。
- 佐证（只读实测）：真实 `history.jsonl` 93 条 0 个 `_sig`；`HKLM\SOFTWARE\MiniSys`
  KEY-EXISTS 且非提权读被 PermissionDenied（密钥曾由提权会话创建，ACL 有效）→ 提权会话
  拿到密钥仍签不出；v2.16d 注释自认"v3 HMAC trailer 每会话写失败"即空 MAC 造成的误诊。
- 后果：a) 全部记录无签名→历史页全"未签名"、还原全拒（`Presenters.cpp:938`、
  `SessionService.cpp:962`）；b) `SignUnsignedLegacy` 输出（若非死代码）也无法验证；
  c) v2.14 声称闭合的 T-A1 链，其"新记录可信"半边从未生效。
- 测试为何没抓到：`GuardRailsAdversarialTests.cpp:232`（HmacContract）在密钥为空时直接
  return——测试进程非提权→恒跳过；MiniSysTests 从未用本地固定密钥跑过 HMAC。
- 修复草案：`BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr,
  BCRYPT_ALG_HANDLE_HMAC_FLAG)`（一行）；测试改用本地固定密钥断言非空+确定性（见测试草案 #1）。
- 注意：v2.17 起 asInvoker——非提权会话 `GetOrCreateMachineSecret()` 返回空，记录仍不签名
  （13/41 会话 elevated=0）。修复后需决策：接受"仅提权会话签名"或加每用户 DPAPI 备用密钥。

### T-B2（高|Confirmed）SignUnsignedLegacy 死代码（off-by-one）
- 位置：`MiniSys/src/core/OperationLog.cpp:261-268`。
- 复现（driver `sign`）：8 条语料（含 `C:\MiniSys.Quarantine\evil.dll` 正规形状、
  `C:\minisys.quarantine` 裸根、`D:\...`）全部 `signed=no`，`adopted 0 of 8`。
- 原因：`"minisys.quarantine"` 长 18，`compare(3,17,...)` 取 17 字符子串与 18 字符串比较恒 false
  （`t.size()>=21` 边界说明作者按 18 计数但写了 17）。
- 后果：升级采纳路径从未生效；与 T-B1 叠加表现为"所有旧记录永久未签名"。
- 修复：`compare(3, 18, ...)` 并按下述 T-B3 加固（不能只改数字）。

### T-B3（高|Confirmed）采纳检查设计弱点：兄弟前缀 + 可写隔离区 + restoreTo 名单缺口
- 位置：`OperationLog.cpp:261-268`（结构检查）、`QuarantineOp.cpp:133-165`（UndoPaths 仅
  `IsProtectedPath(restoreTo)`）、`GuardRails.cpp:75-103`（fileSubtree 无用户级自启动位置）、
  C:\ 根 ACL（继承 `Authenticated Users: Modify`，实读 `C:\MiniSys.Quarantine` 亦然）。
- 实测事实链：
  1. 非提权在 `C:\` 根建目录成功（`mkdir` 探针 OK，文件创建被拒）；
  2. `t.compare(3,18,L"minisys.quarantine")==0` 对 `C:\MiniSys.Quarantine-r6\payload`、
     `C:\MiniSys.Quarantine.evil\x` 均成立（driver undo2：`fixed check matches: YES`）；
  3. `IsProtectedPath(C:\Users\tjf\...\Startup)=no`（真实路径只读查询）；
  4. `QuarantineOp::UndoPaths` 对签名有效记录直接 Move（op 层无独立闸，属设计）；
  5. driver undo2 全链：伪造 QUARANTINE 记录（source=模拟 Startup、target=兄弟目录 payload）
     → Append（真实签名路径）→ `VerifySignature=YES` → eligible=YES → `UndoPaths=YES` →
     `payload-landed=YES`（内容校验通过）。
- 前提：需要管理员点一次"信任并签名旧记录…"（批量、无逐条确认，对话框文案声称"仅对隔离区
  目标位于 MiniSys.Quarantine 目录下的旧记录签名"——兄弟前缀下该声称不成立）+ 再点还原。
- 修复草案：①采纳前对 target 做 `GuardRails::Canonicalize` 并要求严格位于
   `<某固定盘>:\MiniSys.Quarantine\` 子树内且该根存在；②签名逐条列出（id/source/target）确认；
   ③`UndoPaths` 增加 quarantinePath 同样的子树校验（纵深）；④restoreTo 名单补
   Startup/Run 键/计划任务目录（至少告警）。
- 置信度：高（每环实测；当前被 T-B2 意外挡住，标注"修复 T-B2 后即活"）。

### T-B4（中|Confirmed）UpdateStatus 破坏签名（stale sig）
- 位置：`MiniSys/src/core/OperationLog.cpp:277-312`（改 status/note 后 `RewriteAtomic`
  保留旧 `_sig`，不重算）。
- 复现（driver updstat，工作 HMAC）：`after Append: verify=YES` →
  `after UpdateStatus: status=REVERTED sig-present=YES verify=no`；
  `SignUnsignedLegacy re-adopts: 0`（非空 sig 被跳过，永久）。
- 后果：每条被撤销/改状态的记录变成"看起来被篡改"；显示层污名化 + 无法恢复。
- 修复：UpdateStatus/UpdateStatusBulk 内对修改过的记录用当前密钥重签（无密钥时保留旧值并标注）。

### T-B5（中|Confirmed）Apps 迁移先撞 FileOp 策略——功能自 v2.0 全灭
- 位置：`MiniSys/src/core/SessionService.cpp:744-771`（`Validate(pi,si)` 默认 FileOp 先行，
  Apps 分支再跑 Migration）；`GuardRails.cpp:79-80`（fileSubtree 含 Program Files ×2，无豁免）。
- 复现（driver mig）：`C:\Program Files\Common Files` 真实属性 →
  `FileOp allow=no（受保护系统路径）| Migration allow=YES`。
- 后果：所有 `C:\Program Files\*` 应用迁移在第一道闸被跳过（R-xx fail-closed）；v1 时代
  迁移过（ADR-001），v2 重构后该功能实际不可用，真实日志无迁移记录所以未暴露。
- 修复：Apps 表直接用 `Context::Migration`（一次调用），或 FileOp 分支排除 `plan.tab==Apps`。

### T-B6（低|Confirmed）migrateTargetRoot 从 settings.json 恢复不复查
- 位置：`MainWindow.cpp:2217-2221`（对话框设置时查 `IsOnSystemDrive`）；`MainWindow.cpp:2073`
  （`LoadSettings` 直接采用）；`MoveJunctionOp.cpp:113`（preflight 只查 migSubtree——
  Program Files 非 WindowsApps 部分、C:\ 根均不在内）。
- 复现（driver migundo）：`prefflight(sandbox src, C:\Program Files\zz-tgt)=通过`；
  `prefflight(sandbox src, C:\zz-tgt-root)=通过`。
- 后果：settings.json 用户/恶意软件可写 → 迁移落点可指向系统盘/Program Files（仍需用户在
  UI 确认执行，含确认对话框）。非直接提权，属输入校验收口缺口。
- 修复：LoadSettings 时复查（系统盘拒绝+置空）；preflight 增加"目标根非系统盘"断言。

### T-B7（低|Confirmed）盘符相对路径的判定依赖进程 CWD
- 位置：`GuardRails.cpp:195-280`（Canonicalize 仅特判 2 字符裸盘符 `C:`；`C:Windows\System32`
  形式经 `GetFullPathNameW` 拼当前 CWD）。
- 复现（driver corpus）：`C:Windows\System32 → prot=no`（driver CWD=exp 目录）；若 CWD 恰在
  `C:\Windows` 下则结论翻转。执行层兜底存在（`\\?\C:foo` 解析失败→"路径已不存在"拒绝，
  规则路径同理无条目），当前无利用链。
- 修复：`X:foo`（无反斜杠）按裸盘符一样保守映射为 `X:\` 根。

### T-B8（已知未修，复现仍在）T-A6 委派白名单任意尾缀
- driver cmd：`Dism.exe /Online /Cleanup-Image /ResetBase`、`powercfg /h off /a extra`、
  `cleanmgr /VERYLOWDISK /d C: /x y` 全部放行（前缀+边界后尾缀不查）。上轮已立项未修，
  本轮确认仍在，不另计新发现。另实测 `\t` 也算边界（`/Cleanup-Image\tcmd /c x` 放行），
  与尾缀同类（CreateProcess 无 shell，无注入面）。

## C 节发现清单

### T-C1（中|Confirmed）索引缓存 MAC 已在 v2.16d 撤除（回归已修复项）
- 位置：`VolumeIndex.cpp:1138-1146`（注释明确回到 plain v2）、`:1268-1276`（writer 同）。
- 现状防线：卷序列号匹配 + nodeCount/nameBytes 绝对上限（5000 万 / 768MB，T-A4 修复形态）+
  FRN 环预算（T-A3 修复存活，`budget = nodes_.size()*2+16` 实读）。无完整性校验。
- 残余（上轮 T-A2 已知面的子集，无新增）：显示层欺骗（假搜索结果/假大文件条目）、
  垃圾规则 known-absent 假阴性（清理页条目消失）；内存放大上限≈nodeCount 40B+768MB≈~3GB。
- 注：v2.16d 撤 MAC 的理由（"262MB 写失败/加载卡顿"）与 T-B1 空 HMAC 高度吻合——
  MAC 恒空 → 读侧校验恒败 → 每会话重建重写。建议 T-B1 修复后重启 v3 MAC 并复测写性能，
  而非维持"无 MAC"现状。

### T-C2（低|Confirmed）签名未覆盖 timestamp / isReversible
- 位置：`OperationLog.cpp:90-95`（SignedFieldsOf）。
- 复现（driver updstat）：改 timestamp→`verify=YES`（1999 伪造生效）；reversible 翻转同理
  （type 分发使其不可利用，仅显示误导）。status/source/target/note/size/type/id 改动均失配 ✓。
- 修复：两字段纳入签名串。

### T-C3（低|Confirmed，整体健康）Json 解析器与配置 IO
- 27 条坏样本实测（driver json）：空文件/纯空白/BOM/根数组/根字符串/截断对象/截断字符串/
  布尔字符串/null/重复键/64 与 63 层嵌套/1.2.3/1e999/-0/前导+/孤立代理对/代理对/坏 \u 十六进制/
  原始控制字符/\u0000/尾逗号/尾随垃圾/单引号/注释/2^53+1 精度——无崩溃、无死循环、错误可读；
  64 层通过（kMaxDepth=64 拒第 65 层，符合设计）。
- 字段类型错→逐字段默认值（useSymlink "yes"→false）；整文件坏→全默认且**不覆盖原文件** ✓；
  settings/results/history/index 各写点均 tmp+MoveFileEx 原子 ✓；rules.json 全量替换 builtin，
  全部被 ValidateRule 拒绝时规则表为空（用户自编辑文件，自担）。
- 新增小项：`\u0000` 可把 NUL 注入字符串 → 后续 Win32 API `c_str()` 截断（路径前缀语义变化）；
  执行层被"存在性+保护名单"兜住，建议 JsonToScanItem/ValidateRule 拒绝含 NUL 的路径。

### T-C4（低|Confirmed）隔离区根目录 ACL 过宽
- 实读（只读）：`C:\MiniSys.Quarantine` 继承 `Authenticated Users: Modify, Synchronize
  (IsInherited=True)`（`QuarantineOp.cpp:107` `fs::create_directories` 默认继承 C:\ ACL）。
- 后果：非提权进程可改/删隔离区内容：删除=可逆性破坏（DoS）；替换=管理员撤销时把攻击者
  内容还原回原路径（需 T-B1/T-B3 修复后才有签名可信的 undo 面）。
- 修复：创建隔离区根后显式 SetNamedSecurityInfo（BA/SY/所有者全权+Users 只读），或至少
  撤销继承。

### T-C5（info，信任根验证）HKLM 密钥 ACL 有效；"预建键抢注"假设被证伪
- 实测：非提权 `New-Item HKLM:\SOFTWARE\MiniSysR6ProbeZZ` 被拒（访问被拒绝）→ 标准用户
  无法在 HKLM\SOFTWARE 下预建 `MiniSys` 键，`WriteSecret`（`SecretStore.cpp:40-60`，
  RegCreateKeyExW 对已存在键忽略 SA）的首跑抢注竞争在本机默认策略下不可达；
  读 `HKLM\SOFTWARE\MiniSys` 值 PermissionDenied → BA+SY ACL 生效。
- WriteSecret 未检查 `disp==REG_CREATED_NEW_KEY` 属纵深弱点，建议补（低成本）。

### 其余 C 节核对（无新发现）
- 隐私：日志副本（949 行）当前周期**无任何完整用户路径**；history.jsonl / results-*.json
  含完整路径（还原必需，上轮已知接受）；dumps 目录单个 MiniDumpNormal 106KB（最小转储）。
- 子进程：DelegateOp `CreateProcessW`（无 shell、lpApplicationName=nullptr 但 exe 白名单为
  全限定无空格系统路径+引号包裹）、job 对象 KILL_ON_CLOSE、30min 上限、读管道线程+EOF 超时
  ——注入面仅在 T-A8/T-A6 尾缀类；`explorer /select` 已剥 `"`（MainWindow.cpp:2062-2066，
  代码核对存活）；`-takeover` `FindWindowW(L"MiniSysMainWnd")` 按**窗口类**匹配而非标题，
  伪造窗口至多吸收 WM_CLOSE 使 takeover 失败/退避，无提权面（main.cpp:18-58 读码）。
- 写入点收口（grep 全部 12 个破坏性 API 调用点）：QuarantineOp::Execute 自查
  IsProtectedPath ✓、MoveJunctionOp Preflight ✓、EmptyQuarantine 固定枚举路径 ✓、
  Junction.cpp RemoveDirectoryW 仅建链失败清理 ✓、OperationLog/Settings/SessionService/
  VolumeIndex 全部 tmp+rename ✓、CrashDump 只清 .dmp ✓、Logger 5MB 轮换 ✓；
  DeleteOp 无构造点（孤儿，上轮已记档）。

## 门禁绕过统计表

| 语料组 | 条数 | 门禁挡住 | 放行（符合预期） | 命中缺陷 |
|---|---|---|---|---|
| GuardRails 路径归一化（含回归+新增） | 23 | 21 | 2 | 1（T-B7 CWD 相对） |
| Canonicalize/dotdot/8.3 探针 | 4 | 4 | 0 | 0 |
| legacy 采纳结构检查 | 8 | 0（全拒） | 0 | 全拒=T-B2 死码；修复后 6/8 应拒会放行=T-B3 |
| 撤销链（undo/undo2 分步） | 10 | 5 | 5 | T-B3（undo2 落地成功） |
| HMAC/签名完整性（updstat） | 6 | 3 | 0 | 3（T-B1 空、T-B4 stale、T-C2 ts） |
| Json 坏样本 | 27 | 22（拒/限深） | 5 | 0（仅 \u0000 注入备注） |
| 委派白名单 | 21 | 14 | 5 | 2（T-A6 尾缀已知 + tab 边界同类） |
| 迁移闸/preflight | 5 | 0 | 3 | 2（T-B5 FileOp 先行、T-B6 目标根） |
| 合计 | ≈104 | 69 | 15 | 新增 7 项（T-B1..T-B7）+已知 1（T-A6） |

## 已验证无问题 / 被证伪的假设

| # | 项目 | 结论 |
|---|---|---|
| 1 | 标准用户预建 `HKLM\SOFTWARE\MiniSys` 抢注密钥 ACL | 证伪——非提权建键被拒（实测）；现有键 ACL 拒读 ✓ |
| 2 | legacy 采纳 `..\` 逃逸变体全链 | 签名检查可过（修复后），但 `MoveWithRetry` 的 `\\?\` 前缀不解析 `..`→存在性失败→undo 拒绝（driver 实测）——意外防线，建议仍显式拒绝 |
| 3 | dotdot 死尾绕过 | `GetFullPathNameW` 实测**会**折叠 `..`（`C:\a\b\..\c`→`C:\a\c`）+祖先 GetLongPathNameW 展开→死尾也进保护（driver c14n） |
| 4 | 8.3/尾点尾空格/中间点/正斜杠/双斜杠/UNC/`\\?\`/`\\.\`设备名/ADS/大小写/盘根 | IsProtectedPath 全部正确拒绝（21/21，含 `\\.\C:` 落 UNC 分支） |
| 5 | 上轮已修项存活核验 | T-A5 profileNames 裸名（JunkRules.cpp:269-289）✓、T-A7 InfoOnly 镜像内置（:240-261）✓、T-A12 SplitNames 保留空格（JunkScanner.cpp:64-84）✓、T-A8 /select 剥引号（MainWindow.cpp:2062）✓、T-B1 缓存 Delegate 双层复验（SessionService.cpp:274/729）✓、T-A3 Finalize 环预算（VolumeIndex.cpp:226）✓、T-A4 nameBytes 上限 ✓、05-T-C5 UpdateStatus 锁内读改写 ✓ |
| 6 | 混选撤销绕过（选中含合法+伪造记录） | 不成立——执行收口 `UndoRecord` 逐条 VerifySignature（SessionService.cpp:962），伪造项单独拒绝 |
| 7 | 崩溃转储敏感面 | 单个 106KB MiniDumpNormal（最小堆栈）；日志本期无完整用户路径 |
| 8 | Json 解析器崩溃/挂死（27 坏样本） | 无 |
| 9 | 假设"签名链已在生产运行" | 被推翻——0/93 签名 + HMAC 恒空（T-B1） |
| 10 | 假设"v2.16d 砍 MAC 是性能权衡" | 部分推翻——根因疑为 T-B1 空 MAC（MAC 恒失配→每会话重建），建议修复后复核 |

## 加固建议与测试草案（可追加至 GuardRailsAdversarialTests.cpp）

```cpp
// 1) [P0] 本地固定密钥下 HMAC 必须产出（T-B1 回归——当前必 FAIL）
TEST(TrustChain, HmacProducesMacWithLocalSecret) {
    std::vector<uint8_t> key(32, 0x42);
    EXPECT_FALSE(HmacSha256Hex(key, L"hello", 10).empty());   // 修复前为空
    HmacSha256 h(key);
    ASSERT_TRUE(h.Ok());                                      // 修复前 false
}
// 2) [P0] Append→LoadAll→VerifySignature 往返（沙箱 history）
TEST(TrustChain, AppendVerifyRoundTrip) {
    fs::path tmp = TempBase() / "h.jsonl";
    OperationLog::SetHistoryPathForTesting(tmp);
    OpRecord r; /* 填 QUARANTINE/SUCCESS */ r.id = OperationLog::NewId();
    OperationLog::Instance().Append(r);
    EXPECT_TRUE(OperationLog::VerifySignature(OperationLog::Instance().LoadAll()[0]));
    OperationLog::SetHistoryPathForTesting({});
}
// 3) [P0] UpdateStatus 后签名仍有效（T-B4）
TEST(TrustChain, UpdateStatusResigns) {
    /* Append 后 UpdateStatus(id, Reverted, note)，LoadAll 后 VerifySignature==true */
}
// 4) [P1] 采纳正规形状 legacy 记录（T-B2 回归）
TEST(TrustChain, LegacyAdoptionSignsGenuineQuarantine) {
    /* 植入 target="Q:\\MiniSys.Quarantine\\x" 无签名记录，SignUnsignedLegacy()==1 */
}
// 5) [P1] 采纳拒绝兄弟前缀与 dotdot（T-B3）
TEST(TrustChain, LegacyAdoptionRejectsSiblingAndDotdot) {
    /* "C:\\MiniSys.Quarantine-evil\\x"、"C:\\MiniSys.Quarantine\\..\\pwn" 均不被签名 */
}
// 6) [P1] 盘符相对路径按根拒绝（T-B7）
TEST(GuardRailsAdv, DriveRelativeTreatedAsRoot) {
    EXPECT_TRUE(GuardRails::Instance().IsProtectedPath(L"C:Windows\\System32"));
}
// 7) [P1] UndoPaths 拒绝隔离区子树外的 quarantinePath + Startup 落点（T-B3 纵深）
TEST(GuardRailsAdv, UndoPathsQuarantineSubtreeAndStartupGuard) { /* ... */ }
// 8) [P1] Apps 迁移源（Program Files）过 Migration 闸（T-B5 回归）
TEST(GuardRailsAdv, MigrationProgramFilesSourceAllowed) {
    /* Validate(pi, si, Context::Migration).allow == true（现 FileOp 误拒在 RunPlan 层） */
}
// 9) [P2] 委派白名单尾缀收紧（T-A6）
TEST(DelegateWhitelist, TailArgumentsRestricted) {
    std::wstring why;
    EXPECT_FALSE(JunkRules::DelegateCommandAllowed(
        L"C:\\Windows\\System32\\Dism.exe /Online /Cleanup-Image /ResetBase", why));
    EXPECT_FALSE(JunkRules::DelegateCommandAllowed(
        L"C:\\Windows\\System32\\powercfg.exe /h off /a extra", why));
}
// 10) [P2] JsonToScanItem / ValidateRule 拒绝内嵌 NUL
```

## 修复顺序

- **P0（当天，一个提交内）**：T-B1（HMAC 标志一行）+ T-B2（17→18 且按 T-B3 加固边界——
  不能只改数字，否则立即打开 undo2 实证的落盘链）+ T-B4（UpdateStatus 重签）+
  测试草案 #1-#3。T-B1 单独上线会把"全未签名"变成"旧记录永久不可撤销"。
- **P1（本周）**：T-B3 剩余加固（canonicalize 包含性检查、逐条确认、Startup 名单）、
  T-B5（Apps 直走 Migration 上下文）、T-C1（T-B1 修复后恢复缓存 MAC 并实测 262MB 写）、
  隔离区 ACL（T-C4）。
- **P2**：T-B6（migrateTargetRoot 载入复查）、T-B7（盘符相对）、T-C2（签名扩字段）、
  T-A6 尾缀收紧、NUL 路径拒绝、WriteSecret 检查 REG_CREATED_NEW_KEY。

## 未验证 / 局限

- 未启动 MiniSys.exe 本体，产品内 UI 行为（历史页"未签名"显示、还原拒绝文案）由代码+日志
  推断，未活体截图。
- SecretStore stub 与产品的唯一差异：测试密钥固定 + HMAC 标志诊断分支；HMAC 恒空结论基于
  同款 API 序列在同等 Windows 26200 的实测 + 真机 0/93 签名佐证（未在提权进程内复测，受限
  于不得以提权运行实验）。
- 崩溃转储内容仅查目录/大小，未做字符串级敏感信息扫描（strings 工具不可用）。
- 投毒缓存的显示层欺骗（T-C1 残余）本轮未重建（上轮 T-A2 已实测，格式未变，仅 MAC 撤除）。

## 实验产物

- driver 源码与构建脚本：`.tmp-review/r6/exp/{gate6.cpp, SecretStore_stub.cpp, Logger_stub.cpp, build.cmd}`
- 可执行：`gate6.exe`（9 模式）；输出样本：`out_*.txt`
- 沙箱数据 `exp/data/` 已清理；C:\ 根探针目录已全部移除；真实用户数据零写入。
