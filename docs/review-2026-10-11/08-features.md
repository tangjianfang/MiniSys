# 08 功能扩展机会评审（第四轮 · 产品角色首次出场）

> 角色：产品经理 + 资深工程师 · 日期：2026-10-11 · 基线：master 5cda187（v2.14）
> 对象：MiniSys v2.14（六 Tab，src 约 11,100 行 + 15 个测试文件）。定位已从"C 盘瘦身"升级为"磁盘瘦身助手"（**所有固定磁盘**，MainWindow.cpp:41-44 注释明确）。
> 方法：通读 需求.txt / DESIGN-v2.md / DESIGN-v2-实施报告.md / ARCHITECTURE.md / REVIEW_2026-10-06-3.md / evolve-log.md + 上轮 08-features.md（2026-09-30，F1–F18 体系）；全量读 MainWindow.cpp（1,975 行）/ Presenters.h/.cpp 关键段 / SessionService.h / VolumeIndex.h / GuardRails.h / JunkRules.h / rules.json / resource.h / main.cpp / LargeFileScanner.cpp / DevBuildCache.h；针对性 grep 核实每个"缺失/孤儿"断言。仓库只读、未启动应用、未读用户数据。

---

## 1. 现状盘点：现有功能清单表

成熟度口径：**成熟**（端到端可用且有测试/实测佐证）/ **可用**（能跑，覆盖面或体验有缺口）/ **骨架**（代码在、能力打折）/ **孤儿**（写了没接线）。

### 1.1 六 Tab 功能

| # | 功能 | 入口 | 成熟度 | 备注 |
|---|---|---|---|---|
| 1 | 垃圾规则扫描（27 条：Safe 15 / Cautious 7 / Advanced 3 委派 / Info 2；外置 rules.json + 校验拒绝 + 内置回退 + 降级可见化） | 垃圾清理 Tab「扫描」 | 成熟 | v2.14 后规则加载受路径白名单 + 委派命令白名单双重校验（JunkRules.h:49-64） |
| 2 | 开发构建缓存扫描（bin/obj/.vs/ipch/node_modules…，项目标记（*.sln/CMakeLists.txt）约束，24h 内动过跳过，DirSizeCache 加速） | 垃圾清理 Tab（随规则扫描） | 成熟 | DevBuildCache.cpp 352 行 + DevBuildCacheTests；devCacheRoots 有专属编辑对话框（RootsDlg，v2.12） |
| 3 | 回收站清空特殊项（EmptyRecycleOp，全系统，不可逆，默认不勾） | 垃圾清理 Tab | 可用 | 确认弹窗单列"⚠ 不可逆"组 |
| 4 | 扫描仪表盘（Safe/Cautious 项数与字节汇总、规则降级/索引异常提示） | 扫描完成 info 区 | 可用 | OnScanDone 内聚合（08-F6 落地） |
| 5 | 全固定盘 USN 索引（多卷 VolumeIndex::For 注册表；USN 枚举 + WalkBuild 回退 + 磁盘缓存 + USN 增量；实测 2,831,802 项） | 文件搜索 Tab（首次进入自动建；「重建索引」强制重建） | 成熟 | v2.14 起索引缓存带 MAC（SecretStore） |
| 6 | 即时搜索（多词 AND、`*?` 通配符、`folder:`/`file:`/`ext:` 过滤词、路径匹配开关、Enter 刷新/Esc 清空、防抖） | 文件搜索 Tab | 成熟 | 上限 1,000 行（SessionService.cpp:502 kMaxResults） |
| 7 | 快速筛选预设（16 项：.pdb/.obj/ilk/ipch/Debug/Release/bin/obj 文件夹/视频/图片/音频/文档/压缩包/安装包/清除） | 「快速筛选」下拉按钮 + 命令面板 | 成熟 | 活跃项打勾（X-8） |
| 8 | 搜索结果执行（勾选任意文件/文件夹→隔离区）+ 双击定位（explorer /select，引号消毒） | 文件搜索 Tab | 可用 | 确认文案区分"普通文件/文件夹"话术（L-7） |
| 9 | 大文件扫描（minMB/扩展名/盘符过滤、topN partial_sort、修改时间列、列头排序、排序按钮） | 大文件 Tab | 可用 | **默认根 = 仅系统盘**（见 §2.1-A1，与文案"默认全部固定磁盘"不符） |
| 10 | 精确去重（size → 头 64KB SHA-256 → 全量 SHA-256 三级，[保留]最新一份、预选其余副本） | 大文件 Tab | 成熟 | 仅在扫描根内去重（默认 C:） |
| 11 | 应用迁移（复制+永久删源+Junction/Symlink；四步预检含 RmGetList；迁移后自检；失败回滚；UWP/WindowsApps 自动排除） | 应用迁移 Tab | 成熟 | 目标盘强制非系统盘 |
| 12 | 迁移前系统还原点（TaskDialog 复选，默认勾选，失败降级不阻断） | 应用迁移确认弹窗 | 可用 | SystemRestore.cpp（SRSetRestorePoint） |
| 13 | 文件夹分析（顶层目录尺寸树 + 盘符节点容量 + 双击下钻子树） | 文件夹分析 Tab | 可用 | 下钻后「扫描」返回全盘（X-11 提示） |
| 14 | 右键"移入隔离区…"（走 PlanBuilder+GuardRails 同链路） | 文件夹分析 Tab 右键 | 可用 | |
| 15 | 操作历史（JSONL + 旧 TSV 迁移 + 原子重写 + v2.14 记录签名/防篡改 + "信任并签名旧记录"升级口） | 操作历史 Tab | 成熟 | 类型/状态/风险中文标注（L-2/L-11） |
| 16 | 批量撤销（多选、worker 线程、签名校验、不合格预分类提示） | 操作历史 Tab「撤销选中」 | 成熟 | UndoRecordsAsync（04-R5） |
| 17 | 清空隔离区（遍历各盘 MiniSys.Quarantine，标记已释放） | 操作历史 Tab | 可用 | **全有或全无**，无逐条永久删除、无内容浏览（见 C5） |

### 1.2 横切能力

| # | 功能 | 入口 | 成熟度 | 备注 |
|---|---|---|---|---|
| 18 | GuardRails 安全闸（规范化后保护名单、reparse/云占位拒、TOCTOU size/mtime 复验、InfoOnly/Delegate 拒直接文件操作、用户排除清单、FileOp/Migration 双上下文） | 内部（一切执行必经） | 成熟 | GuardRails.cpp 468 行 + 3 套对抗测试 |
| 19 | PlanBuilder + planHash（PlanStale 整单中止） | 内部 | 成熟 | |
| 20 | 预览执行（dry-run：批量 GuardRails.Validate，通过/被拒+原因，worker 化） | 右键菜单「预览执行」 | 成熟 | 08-F7 落地 |
| 21 | 列表校验刷新（手工 + WM_ACTIVATE 归来去抖 + worker 侧存在性检查，幽灵行修剪） | 右键「刷新列表」/Alt-Tab 自动 | 成熟 | v2.7/v2.8 |
| 22 | 命令面板（Ctrl+K，约 20 条：扫描/重建索引/全选/预览/清空隔离区/关于/开发缓存根/信任旧记录/切 6 页/5 个快速筛选） | Ctrl+K | 可用 | 命令**静态构造**，无自定义别名（见 C9） |
| 23 | 快捷键（F5 扫描/取消、Ctrl+A 智能全选、Ctrl+K、Tab 导航、Enter/Esc 搜索框） | 全局 | 成熟 | |
| 24 | 设置持久化（11 字段：迁移目标/symlink/大文件三参/排除清单/搜索词/matchPath/lastTab/devCacheRoots） | 自动（settings.json 原子写） | 成熟 | v2.9/v2.11 |
| 25 | 会话恢复（六 Tab 结果缓存 + 搜索词/勾选/页面还原，"上次扫描: …（缓存）"） | 启动自动 | 成熟 | 执行安全不依赖新鲜度（复验兜底） |
| 26 | 空闲维护（30s tick：空闲 5min+ 数据过期 30min → 10s 倒计时自动重扫；10-30min 陈旧 → USN 增量+幽灵修剪，15min 冷却，归来即取消） | 自动 | 成熟 | X-10 + B-3 修复后 |
| 27 | 单实例互斥（二次启动激活已有窗口） | main.cpp | 成熟 | 08-F13 落地 |
| 28 | 崩溃 minidump + 日志 + 风险行染色（高对比度关闭）+ 视图状态保持（滚动/焦点/选中跨刷新） | 内部 | 成熟 | |
| 29 | 状态栏四格（系统盘空间/迁移目标或隔离区占用/进度文本/进度条覆盖格） | 全局 | 成熟 | |
| 30 | -tour 自动巡页 + tools/screenshot.ps1 被动截图 | 命令行参数 | 可用 | **评审工具**，非用户功能（evolve Round 2） |

### 1.3 孤儿功能侦察（写了没接线/接了一半）

| # | 孤儿 | 证据（grep） | 处置建议 |
|---|---|---|---|
| O1 | `DeleteOp`（回收站删除，IFileOperation 版） | 全仓无构造点（仅自包含 cpp + include）；v1 起即孤儿，三轮评审均记录 | 删除死代码，或留作"清空隔离区"的逐条实现底座（见 C5） |
| O2 | `OpType::MoveFilePath` | 枚举值 + OperationLog 序列化映射存在，无任何 Operation 实现产生该类型 | 删除或留给"计划回放"（C3）语义 |
| O3 | `IDC_BTN_REFRESH`(2006) / `IDC_BTN_DEDUP`(2010) | resource.h 定义，无 CreateWindow、无 WM_COMMAND 分支（v1 R-009 遗留至今） | 删除 |
| O4 | `ScanItem.dangerous` | JunkScanner 写入 + results-*.json 序列化往返，但**无读取点**（UI 已全面改用 riskLevel，OnExecute 的危险汇总走 `riskLevel==Advanced`） | 半孤儿：从序列化格式中移除需做缓存版本兼容，建议标注 deprecated 后清出 |
| O5 | 排除清单"管理" | 仅右键添加（IDM_LIST_EXCLUDE→OnExcludeSelected）+ settings.json 手改；**无查看/移除 UI**（上轮 F9 设计的"管理排除项"对话框未落地） | 升格为候选 C12（S 成本） |
| O6 | `-tour` 模式 | 有意保留的评审基建，窗口常开才停 | 不算孤儿；文档化即可 |

### 1.4 被推翻假设（本次盘点中纠正）

| # | 假设 | 事实 |
|---|---|---|
| A1 | "大文件页默认扫全部固定磁盘"（Tab 描述文案 v2.11 改为"默认全部固定磁盘"，OnAbout 同款话术） | **代码仅系统盘**：`LargeFileScanner::DefaultRoots()` 返回 `{ SystemDriveRoot() }`（LargeFileScanner.cpp:63-68，v2 R-006 修复产物）；`LargeFilesPresenter::BuildScanner` 仅当盘符框非空才覆盖 roots。v2.11 定位升级时**漏改这里**。跨盘大文件/重复文件默认根本搜不到 |
| A2 | "虚拟列表已落地，可承接百万行"（本轮任务题面预设） | **未实现**：grep `LVS_OWNERDATA`/`LVN_GETDISPINFO` 零命中；仍是逐行 Insert（RenderItems），搜索结果硬上限 1,000（kMaxResults）。X-7 只落地了视图状态保持，虚拟列表仍在评估名单 |
| A3 | "索引内存已做紧凑化"（题面预设） | 部分成立：`VolumeIndex` 本就是紧凑设计（Node 定长 40B + names_ 字符串池 + nameOff/nameLen，按 DESIGN §9 预算 100 万文件 <150MB）。但"紧凑化"作为独立改进项不存在，多盘全量常驻（实测 283 万条）无逐盘开关（见 C1） |
| A4 | "隔离区只有整体清空" | 部分推翻：**逐条还原**早已可用（History 多选批量 Undo）；缺的是隔离区**内容浏览/预览/单条永久删除**（RunEmptyQuarantine 遍历所有固定盘根，全删） |
| A5 | "定时自动清理已被永久否决" | 实施报告 §1.2 落定"不列入（超出本轮）"，属**顺延**而非终审；DESIGN-v2 §15 仍是待确认项。边界上受需求.txt 第 3 条（一切删除需确认）约束——全自动执行仍越界，但"空闲预备计划+一键确认"的折中形态可议（C4） |

---

## 2. 候选方向卡片总表

成本口径：S ≈ 1–2 天（纯接线/复用现成零件）；M ≈ 3–5 天（新小模块或消息协议扩展）；L > 1 周（新子系统）。边界符合性对照需求.txt / DESIGN-v2（确认制、GuardRails 全覆盖、默认隔离可逆、无后台服务、单实例、requireAdministrator）。

| ID | 候选 | 价值 | 频次 | 成本 | 主要复用点 | 风险 | 边界 | 优先级 |
|---|---|---|---|---|---|---|---|---|
| C5 | **隔离区浏览器**（内容浏览/预览/单条永久删除；逐条还原已有） | 4 | 每次清空决策前 | M | HistoryPresenter、OpRecord.target、QuarantineOp::UndoPaths、RunEmptyQuarantine 单条化、TaskDialog 模板 | 低（全走既有确认+签名校验） | 符合 | **P1** |
| C2 | **按扩展名空间分析报表**（哪类文件吃掉磁盘） | 4 | 每次瘦身决策 | S–M | LargeFileScanner 的 entries（已在内存）、CategoryFor 表、OnScanDone 仪表盘模式、剪贴板导出 | 低（纯内存聚合） | 符合 | **P1** |
| C12 | **排除清单管理 UI**（查看/移除/批量导入） | 3 | 一次配置长期受益 | S | RootsDlg 对话框模板、settings.exclusions、GuardRails::SetUserExclusions | 低 | 符合 | **P1** |
| H3 | **大文件/去重全盘默认**（修 A1：DefaultRoots→全部固定盘，或至少与文案一致） | 4 | 每次大文件扫描 | S | EnumerateDrives、FastWalk 多线程、DirSizeCache | 扫描时长×盘数（可提示） | 符合（定位已升级） | **P0 卫生** |
| C3 | **清理计划导出/回放**（复用例行清理） | 4 | 例行高频人群 | M | ScanItem↔Json 序列化（SessionService.cpp:134/156 已有）、PlanBuilder、GuardRails 逐条复验、ExecuteReport | 中：回放必须重走复验+确认，防"无人值守化"漂移 | 符合（设计上强制再确认） | **P2** |
| C1 | **选择要索引的磁盘**（索引范围粒度） | 3 | 一次性设置 | S | VolumeIndex::For 天然按卷、index-cache 按卷、RootsDlg 模板、EnumerateDrives | 低（内存收益：不索引的盘省下全部常驻） | 符合 | **P2** |
| C4' | **空闲预备计划**（空闲窗口自动算好 Safe 级清理计划，归来一键确认执行；不做自动执行） | 3 | 每日 | M | OnIdleCheck/IdleMaintenanceAsync、Junk 扫描、确认弹窗 | 中：与"确认制"仅一线之隔，文案必须"预备不执行" | 折中后符合 | **P2** |
| C7' | **云文件占用只读报告**（从索引聚合 OneDrive/占位符属性：已下载体量排行，引导官方"释放空间"） | 3 | OneDrive 用户每月 | S–M | VolumeIndex 的 attrs（USN 记录自带）、GuardRails 云占位拒绝已有、DelegateOp(attrib) 暂不做 | 低（只读） | 符合 | **P3**（贴近 Top10 尾） |
| C9 | **命令面板扩展**（自定义别名 + 把新功能入口挂进来） | 2 | 高频用户每日 | S | palette::Show、settings.json | 低 | 符合 | **P3** |
| C10 | **规则导入/导出**（社区规则：国产软件缓存扩展；先无签名版） | 3 | 低频 | M | JunkRules::LoadValidated（路径+委派双白名单已拦截恶意规则）、Json::Dump | 低（校验面已硬）；**签名暂不做**（跨机密钥模型不成熟，SecretStore 是机器级） | 符合 | **P3** |
| C11 | **CLI 伴侣**（minisys-cli 只读报告：可释放量/大文件 top/索引状态） | 3 | 脚本化人群 | M | results-*.json、index-cache、Logger 基准点 | 低（只读）；requireAdministrator manifest 使 `MiniSys.exe --report` 仍弹 UAC，独立 exe 需抽公共代码 | 符合 | **Backlog** |
| C6 | **近重复检测**（pHash 感知哈希） | 3 | 中 | L（WIC 解码+新哈希）或 M（文件名/大小邻近启发式） | 三级去重管线、ParallelHash | **高**：误报=用户删错，直接砸"信任"主线 | 启发式版可符合 | **Backlog**（先做零成本同名聚集报表可议） |
| C8 | **TRIM/磁盘健康只读报告** | 2 | 极低频 | M（SMART 需物理驱动器句柄/WMI） | QueryDiskSpace | TRIM 由系统自动调度，手动触发无收益；健康报告与"瘦身"定位偏离，目标用户自有 CrystalDiskInfo | 只读不越界，但性价比低 | **不做/Backlog** |
| F10' | 定时自动清理（**自动执行**版） | — | — | L | — | 动摇需求.txt 第 3 条确认制根基 | **越界** | **不做** |
| F14' | 托盘常驻/开机自启 | — | — | M | — | 与"每次操作亲自确认"信任叙事冲突；用户已确认走应用内空闲维护 | 越界 | **不做** |

---

## 3. Top 10 深入（排序 + 接口/数据流/UI 入口/测试策略草案）

> 排序原则：命中"所有固定磁盘"新定位与开发者画像 > 补齐既有链路的最后一公里 > 批处理复用 > 锦上添花。#0 为卫生前置。

### #0（前置卫生）H3 大文件/去重全盘默认 + 孤儿清扫（O1–O3）

- **问题**：A1 文案与行为不符——定位升级到"所有固定磁盘"后，大文件页默认仍只扫 C:，跨盘重复文件（开发者 D:/E: 项目镜像）根本查不到。
- **接口**：`LargeFileScanner::DefaultRoots()` 返回 `{ SystemDriveRoot() }` → 改为遍历 `EnumerateDrives()`（PathUtils 已有，FolderTree 同款用法）；`DefaultExcludes()` 已按 EnumerateDrives 扩过（v2.13 精神一致）。用户在盘符框输入仍可收窄。
- **数据流**：纯 roots 集合变化，FastWalk/去重管线零改动。
- **UI 入口**：无新入口；Tab 描述文案已是"默认全部固定磁盘"（改完即自洽）。多盘扫描时长增加 → 复用进度条百分比阶段文案（"正在扫描 D: …"）。
- **测试**：LargeFileScanner 现有单测加"多根时去重组跨根成立"；手工验证双盘环境 top-N 含 D: 文件。孤儿清扫：删 DeleteOp/MoveFilePath/IDC_BTN_REFRESH/IDC_BTN_DEDUP 后 54+ 用例全绿 + 手工回归 History 渲染（`DeleteToRecycleBin` 分支保留用于读旧 JSONL）。

### #1 C5 隔离区浏览器（浏览/预览/部分处置）

- **价值论据**：隔离区默认策略 = 产品信任基石，但用户视角它是个黑箱：只有"源→目标"两行路径文本。真实使用中隔离区曾积压 1.5GB（上轮实测），用户"想看一眼再决定清不清"没有入口；且 RunEmptyQuarantine 全有或全无，想留一件也不行。
- **接口**：不新建子系统——**History Tab 增强**。`HistoryPresenter::Refresh` 对 `type==Quarantine && status==Success` 的行加右键子菜单：①"打开隔离区位置"（explorer /select target，复用 OnOpenLocation 引号消毒）；②"预览文件"（ShellExecute 打开 target——隔离区文件是普通文件，可安全打开）；③"永久删除此项"（新 `SessionService::PurgeQuarantinedAsync(const OpRecord&)`：删 target → `OperationLog::UpdateStatus` 置 `Purged`，逻辑从 RunEmptyQuarantine 抽出单条化，O1 的 DeleteOp 可作永久删除底座）；④"还原此项"（已有 UndoPaths）。
- **数据流**：OpRecord.target 即 manifest（ADR-005），无需新持久化；v2.14 `VerifySignature` 先行——未签名记录禁止处置（与撤销同规）。
- **UI 入口**：History 右键（当前 History 无右键菜单，纯增量）；命令面板加"浏览隔离区…"（切 History + 过滤 Quarantine 类型）。排序/大小列复用现有 ListView。
- **测试策略**：单测：PurgeQuarantined 的 target 计算、冲突（target 已手删→记 Failed 不炸）、签名不过→拒绝；集成：quarantine→purge→`QuarantineUsageText` 数字减一；手工：1.5GB 场景逐条删+整体清空并存。

### #2 C2 按扩展名空间分析报表

- **价值论据**：开发者画像的核心问题不是"哪个文件大"（top-N 已答）而是"**哪类东西**吃掉我的盘"——.pdb 占 40GB、node_modules 占 120GB 这类结论现在要人肉滚列表。索引已有全量文件名（无 size，ADR-004），大文件扫描已有 ≥阈值的 size 数据——两条现成数据源。
- **接口**（两档）：
  - 档 1（S）：`LargeFileScanner::Scan` 末尾对 entries（全量内存中，非仅 topN）按扩展名聚合 `struct ExtStat { count, bytes, maxFile }`，写进一个 `ScanItem`（riskLevel=InfoOnly，title="扩展名分析报表"，detail=多行 top-15）；同时 OnScanDone 仪表盘加一行"Top 扩展名: .pdb 38GB/1.2万 · node_modules…"。
  - 档 2（M）：搜索页命令"ext:report"——从 VolumeIndex 按扩展名聚**条数**（无 size，诚实标注"仅计数"），点行跳转预填 `ext:xxx` 查询。两档互不阻塞。
- **数据流**：档 1 纯内存聚合，零新增 I/O；档 2 走索引线程契约（worker-only）。
- **UI 入口**：仪表盘行（自动）+ 列表 InfoOnly 行 + 右键"复制报表"（剪贴板，复用 F4 的复制模式）。
- **测试**：ExtStat 聚合纯函数单测（分组/排序/空集）；InfoOnly 项不可执行的既有拒绝路径天然兜底。

### #3 C12 排除清单管理 UI

- **价值论据**：排除是"永不清理"的**永久性**决定，而现在加错了没有任何撤销入口（O5）——只能手改 settings.json。信任工具的反面就是"它有个我看不见改不了的名单"。
- **接口**：新 `OnManageExclusions()`——RootsDlg 模板复刻（多行编辑框=路径清单，浏览添加，确定时校验绝对路径+存在性，与 devCacheRoots 同款 flush 逻辑）；确定后 `Settings::Save` + `GuardRails::SetUserExclusions`（两者在 SaveSettings 已有接线）。
- **数据流**：settings.exclusions ↔ 对话框往返，无新持久化字段。
- **UI 入口**：命令面板"管理排除清单…" + History 页按钮（与"清空隔离区"并排，历史上 F9 原设计位置）。
- **测试**：往返单测（写→读相等，无效行丢弃计数）；手工：移除排除项后重扫该项复现。

### #4 C3 清理计划导出/回放

- **价值论据**：dev-cache/浏览器缓存清理是**周期性重复操作**（月度例行），每次重新扫+勾选 30 项是纯损耗；中文开发者有强烈的"批处理复用"习惯。导出物也是团队/多机资产（同款开发环境）。
- **接口**：
  - 导出：OnExecute 确认弹窗加"保存此计划…"（复选）→ `{ schema:1, tab, exportedAt, items:[{path, ruleId, riskLevel, strategy}] }` 落 `%LOCALAPPDATA%\MiniSys\plans\*.json`（Json::Dump；**只存路径+规则元数据，不存 command**——委派命令回放时从当前规则表重取，杜绝缓存投毒教训 T-B1 复发）。
  - 回放：命令面板/右键"回放清理计划…"→ 文件选择 → **加载后仅生成预选勾选**（对当前扫描结果按 path/ruleId 匹配；磁盘上已变的自然不匹配）→ 用户仍走完整确认弹窗+GuardRails 逐条复验。**回放=勾选记忆，不是执行记忆**——这条设计红线让 C3 与确认制完全兼容，也避开 planHash 语义冲突（新扫描新 hash）。
- **数据流**：ScanItem↔Json 序列化已有（results-*.json 管线）；匹配是 O(plan×results) 字符串比对。
- **UI 入口**：确认弹窗复选（导出）+ 命令面板（回放）+ 扫描完成 hint("检测到 2 个可回放计划")。
- **测试**：导出往返单测；回放匹配单测（路径大小写、已删除项不匹配、ruleId 漂移降级为路径匹配）；安全：注入伪 plan 文件（InfoOnly 伪装/系统路径）→ GuardRails 拒绝（复用 GuardRailsP0 测试模式）。

### #5 C1 选择要索引的磁盘

- **价值论据**：v2.11 起索引**所有**固定盘（BuildIndexAsync 遍历 EnumerateDrives，SessionService.cpp:356）。多盘大仓库用户（3×2TB）常驻内存数百 MB；数据盘/备份盘根本不需要秒搜。架构天然支持：VolumeIndex::For 按卷注册、缓存 per-drive、ValidVolumes 快照——只差一个用户开关。
- **接口**：`Settings.indexDrives`（空=全部固定盘，兼容默认）；BuildIndexAsync 的 EnumerateDrives 循环按集过滤；RootsDlg 模板改为盘符多选（复选框列表）。已建索引的盘保留缓存文件，重新勾选即秒载。
- **UI 入口**：命令面板"设置索引磁盘…"；搜索页状态栏文案显示当前范围。
- **测试**：单测：过滤集为空/全量/子集三种；手工：取消勾选 D: → 重建 → TotalEntries 下降、D: 搜索无结果且状态栏不谎报。

### #6 C4' 空闲预备计划（不做自动执行）

- **价值论据**：空闲维护（v2.11）已把"数据保鲜"自动化了，最后一公里"执行"仍需用户主动开应用→扫描→勾选→确认。折中形态：空闲窗口里把 Safe 级扫描**预先算好**，用户归来时状态栏/命令面板一键"应用预备的清理计划（28 项·3.8GB）"——仍是一次确认，但砍掉了 30s 扫描+勾选。
- **接口**：OnIdleCheck 已有 5min/30min 判定 + 倒计时 UX；扩展为空闲且 Junk 数据新鲜时直接生成 `CleanPlan`（仅 riskLevel==Safe 且 recommended 的项）存 SessionService（`pendingPlan_`，随任何新扫描失效）；UI 侧 hint + 命令面板入口，确认弹窗复用 OnExecute 全链路。
- **红线**：绝不自动 ExecutePlan；Advanced/Cautious 不进预备集；文案必须是"已预备，待确认"。
- **测试**：单测：预备集过滤（Safe-only）、新扫描使 pendingPlan_ 失效、planHash 过期路径；UX 手工：空闲→归来见 hint→一键确认→隔离区数字正确。

### #7 C7' 云文件占用只读报告

- **价值论据**：OneDrive/网盘"按需下载"用久后本地不知不觉 rehydrate 几十 GB。GuardRails 已把云占位符当**操作风险**拒掉（RECALL_ON_DATA_OPEN/OFFLINE，GuardRails.cpp:432），但产品从未把它当**清理机会**呈现。只读报告零风险：USN 记录自带 attrs，VolumeIndex 无需额外 I/O 即可统计每个目录下"非占位符（已下载）"的规模。
- **接口**：索引聚合（worker）：按 OneDrive 根（`%USERPROFILE%\OneDrive*` 探测 + Known Folder）统计 hydrated 项数/字节数（size 仍需对命中子树 FastWalk 精扫，同 ADR-004 两层法）；产出 InfoOnly ScanItem（"OneDrive 已下载缓存约 X GB，右键打开 OneDrive 设置→释放空间"）。**不做自动 dehydrate**（attrib/云 API 操作影响同步状态，交给官方入口）。
- **测试**：属性位聚合单测（合成索引注入 ONLINE/OFFLINE 位）；手工 OneDrive 机器对照资源管理器"状态"列。

### #8 C9 命令面板扩展（别名 + 命令集扩充）

- **价值论据**：命令面板是"零深度入口"基建（v2.10），本报告 Top10 有一半新功能的最自然入口就是它。先扩命令集（隔离区浏览/计划回放/排除管理/索引磁盘选择/扩展名报表），别名（`settings.commandAliases: { "qf": "筛选：.pdb" }`）作为 S 级糖挂在后面——命令越多别名价值越大，顺序不可倒。
- **测试**：别名解析纯函数单测；palette Refill 过滤含别名命中。

### #9 C10 规则导入/导出（无签名版）

- **价值论据**：27 条规则是"通用盘+开发者"基线；社区长尾在国产软件（钉钉/企微/百度网盘/迅雷/网易云缓存…）。**不建市场**，只做"导出我的 rules.json 差异 + 导入合并"——LoadValidated 的双白名单（隔离类规则仅限用户空间或豁免系统清理根；委派命令必须匹配内置白名单）已把恶意规则的破坏面压到"诱导清理用户自己的目录"，签名属过度设计（跨机密钥模型≠SecretStore 机器密钥）。v2.14 的教训恰好相反：**校验比签名有效**（缓存投毒链是被白名单复验杀死的）。
- **接口**：命令面板"导入规则…"→ 选择 json → LoadValidated 试载 → 预览（接受 N 条/拒绝 M 条+原因，复用 rejected 管线）→ 合并写回 rules.json（原文件 .bak）。
- **测试**：恶意规则语料单测（系统路径/委派伪装/重复 id 合并策略）。

### #10 C11 CLI 伴侣（暂缓版）

- 结论：**Backlog 但给出触发条件**。`minisys-cli --report`（只读：各 Tab 缓存摘要/索引状态/隔离区占用，stdout JSON）对脚本化人群有价值，但 requireAdministrator manifest、公共代码抽取（Json/PathUtils/Settings）与维护成本使其在当前自用阶段性价比不足。触发条件：用户首次提出"我想在终端看报告"或 CI 集成需求。轻量替代：`--dump-report` 参数直接把 LastReport/仪表盘写 `%LOCALAPPDATA%\MiniSys\report.json`（S，无新工程）——可作为 C3 导出的附属先行落地。

---

## 4. 用现有零件可拼的"低成本高价值"三件

1. **H3 全盘大文件/去重**（≈半天）：改 `DefaultRoots()` 一处（LargeFileScanner.cpp:63）+ EnumerateDrives（PathUtils 已导出）；文案已经写好；FastWalk 多线程、排除表（含各盘隔离区根）、DirSizeCache、三级去重全部现成。收益立竿见影：跨盘重复文件从"搜不到"变"默认可查"，与"所有固定磁盘"定位对齐。
2. **C2 档 1 扩展名报表**（≈1 天）：LargeFileScanner::Scan 里 entries 全量在手上（top-N 只是它的切片），加一个 `unordered_map<ext, ExtStat>` 聚合 + OnScanDone 仪表盘多一行 + 右键复制。零新增 I/O、零新线程、零新持久化。
3. **C12 排除清单管理**（≈1 天）：OnDevCacheRoots 的 RootsDlg（MainWindow.cpp:845-1020）就是模板——多行编辑+浏览+校验+SaveSettings+SetUserExclusions 的接线在 SaveSettings/LoadSettings 里已存在，只需新对话框 + 两个入口（命令面板/History 按钮）。

（候补第四件：C5 的"预览/打开隔离区位置"两个右键项 ≈ 半天，单条永久删除另计。）

---

## 5. 分阶段路线图

| 阶段 | 内容 | 主题 | 预估 |
|---|---|---|---|
| **Now（对齐定位 + 堵信任漏洞，1–1.5 周）** | H3 全盘大文件/去重；孤儿清扫（O1/O2/O3，DeleteOp 或转作 C5 底座）；C12 排除清单管理；C2 档 1 扩展名仪表盘 | "文案说的和做的一致；黑箱都有窗户" | S×4 |
| **Next（隔离区透明化 + 批处理复用，2–3 周）** | C5 隔离区浏览器（含单条永久删除）；C3 计划导出/回放；C1 索引磁盘选择；C9 命令集扩充（把前述入口全部挂入面板） | "隔离区看得见；例行清理一次配置" | M×3+S |
| **Later（自动化边界内压榨，3–4 周）** | C4' 空闲预备计划；C2 档 2 索引计数报表；C7' 云文件占用报告；X-7 虚拟列表（评估后若上，承接搜索 >1000 行与报表渲染） | "回来即一键；只读报告拓宽视野" | M×3 |
| **Backlog** | C9 别名；C10 规则导入导出；C11 CLI 或 `--dump-report` 轻量版；零字节/同名聚集报表；隔离区 7 天过期提醒（DESIGN §15 遗留）；日志轮转；F18"上次清理过同规则项"提示 | 触发条件驱动 | — |

依赖关系：C5 依赖签名校验（已有）；C3 依赖"回放=勾选记忆"红线设计；C4' 依赖 C3 的计划语义成熟后更稳（可调序）；虚拟列表是 C2 档 2 的放大器而非前置。

---

## 6. "不该做"清单及理由

1. **定时自动清理（自动执行版）** — 需求.txt 第 3 条用户亲书"所有的迁移和删除都需要我确认才能执行"。即使只隔离不删，自动移动用户文件同样是未确认的破坏性行为（快捷方式失效/程序行为改变）。C4' 的"预备+一键确认"是边界内最大公约数。
2. **托盘常驻/开机自启/后台服务** — 用户已明确选择应用内空闲维护；清理工具常驻后台与其"每一步都可见可逆"的信任叙事根本冲突。
3. **TRIM 触发** — Windows 对 SSD 的自动 retrim（每周计划任务）已最优，手动触发零收益，伪功能。
4. **感知哈希近重复（现阶段）** — 误报一次（把不同照片判为相似）就砸掉"GuardRails 拦截率 100%"攒下的信任；WIC 解码+新哈希是 L 级投入服务低频场景。轻量"同名文件聚集报表"可作 Backlog 替身。
5. **$MFT 直读 / EV 签名** — 维持 DESIGN-v2 与上轮结论：前者 USN+精扫已达标（实测 283 万项索引建成）；后者是发布期问题（触发条件：决定对外发布）。
6. **删除类（回收站）自动撤销** — DeleteOp 是孤儿（O1），QuarantineOp 是默认且更优；为逆向 IFileOperation 投资无意义，正确动作是清码或转用。
7. **规则签名/规则市场（完整版）** — LoadValidated 双白名单已把恶意规则破坏面压到最小；跨机发布者签名密钥模型复杂且解决的不是当前威胁模型。先做无签名导入导出（C10）。
8. **健康监控仪表盘类（温度/S.M.A.R.T./磁盘寿命）** — 定位漂移：MiniSys 是"瘦身助手"不是"硬件管家"，目标用户（高级用户/开发者）已有专用工具。只读不越界，但性价比为负。
9. **执行前全量 RM 预检** — 维持上轮结论：批量执行反白数秒；失败后诊断已覆盖主要场景。

---

## 7. 未核实项

1. **A1 的"文案错"还是"行为漏改"**：LargeFileScanner 默认根仅系统盘是从 v2 R-006 修复继承的；v2.11 定位升级 commit 是否有意保留单盘默认（性能考虑）未见讨论记录——按"文案与定位一致"推断为漏改，需用户/维护者拍板 H3 方向（改代码 or 改文案）。
2. **多盘全量索引的实测内存**：283 万项是活体实测（evolve-log），但来自几块盘、RSS 多少未见记录；C1 的内存收益量化需实测（GetProcessMemoryInfo 前后对比）。
3. **隔离区单条永久删除与 v2.14 签名记录的交互细节**：`PurgeQuarantined` 更新记录状态后是否破坏既有 HMAC（需重签）——OperationLog 的 UpdateStatus 签名路径未逐行核对，实施 C5 时需先读 SecretStore MAC 覆盖面。
4. **C7' 的 OneDrive 根探测覆盖率**：Known Folder / 注册表 PinDirs / 多账户场景未调研实现细节，S–M 估算是基于"索引 attrs 已有"的推断。
5. **JunkScanner 对 LargeFiles 磁盘过滤的联动**：LargeFiles 盘符框只影响本 Tab；垃圾扫描的驱动范围（索引全盘 vs 规则路径展开）未逐条核对 rules.json 规则在非系统盘的命中情况（规则路径均为 %SystemDrive%/%USERPROFILE% 系，推断垃圾清理实际仍以 C: 为主，DevBuildCache 才覆盖全盘——影响"全盘定位"的下一步优先级判断）。
6. 上轮报告"未核实项"中 DISM 端到端、二扫 ≤5s 实测等手工验收清单状态仍以文档自述为准，本轮未复测。
