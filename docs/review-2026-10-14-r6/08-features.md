# 08 功能扩展机会评审（第六轮 r6）

> 角色：产品经理 + 资深工程师 · 日期：2026-10-14 · 基线：master `2a9d653`（v2.19a，42 提交）
> 方法：通读 docs/ARCHITECTURE.md、DESIGN-v2.md、evolve-log.md、REVIEW_2026-10-11.md、上两轮 08 报告（`.tmp-review/08-features.md` 2026-09-30、`docs/review-2026-10-11/08-features.md`）+ 全量读 ui/Controls.cpp、CommandPalette.cpp、MainWindow.cpp 关键段、core 各公开头（SessionService/VolumeIndex/Scanner/JunkRules/GuardRails/Settings/TabId/LargeFileScanner/FolderTreeScanner/DevBuildCache）+ rules.json 规则 id 全表 + 针对性 grep 核实每个"缺失"断言。仓库只读；未启动应用；未读用户数据（数据面 3M 项索引/267MB 缓存、隔离区 50 项 2.13GB、历史 93 条引自任务给定）。

---

## 1. 现有功能清单表

成熟度：**成熟**（端到端可用）/ **可用**（能跑，覆盖或体验有缺口）/ **骨架**（代码在、能力打折）/ **孤儿**（写了没接线）。

### 1.1 六 Tab 功能

| # | 功能 | 入口 | 成熟度 | 备注 |
|---|---|---|---|---|
| 1 | 垃圾规则扫描（28 条规则：15 Safe + wechat/qq 接收文件 + 10 Cautious + 3 委派 + 3 InfoOnly；外置 rules.json + 双白名单校验 + 降级可见化） | 垃圾清理 Tab「扫描」 | 成熟 | 规则 id 全表已核对（rules.json）；H1 Json 修复后外置规则真加载 |
| 2 | 开发构建缓存扫描（bin/obj/.vs/ipch/debug/release/x64 等 Safe 名 + build/out/target/node_modules/cmake-build-* 等 Cautious 名；项目标记约束；24h 动过跳过；DirSizeCache 加速；用户自定根 RootsDlg） | 垃圾清理 Tab | 成熟 | DevBuildCache.cpp 分类表逐行核对 |
| 3 | 回收站清空（EmptyRecycleOp，全系统聚合，不可逆） | 垃圾 Tab 特殊项 | 可用 | 仅整体清空；SHQueryRecycleBin 聚合，无逐项 |
| 4 | 扫描仪表盘（风险分组汇总 + 降级提示；v2.19 owner-drawn 排版） | info 条 | 成熟 | |
| 5 | 多卷索引（USN 枚举 + WalkBuild 回退 + 磁盘缓存 + USN 增量 + MAC 签名；v2.17 修复端到端搜索链路） | 文件搜索 Tab | 成熟 | v2.17b 修 parent chain bool 截断（Critical 级 bug） |
| 6 | 即时搜索（多词 AND、通配符、folder:/file:/ext: 过滤词、路径匹配、防抖、1000 行预算；**v2.19 起全行补大小**：≤6 线程 GetFileAttributesExW 池） | 文件搜索 Tab | 成熟 | kMaxResults=1000（SessionService.cpp:510） |
| 7 | 快速筛选 16 项预设 | 「快速筛选 ▾」+ 命令面板 | 成熟 | **硬编码**（MainWindow.cpp:881 kPresets），无用户自定义 |
| 8 | 搜索结果执行（勾选→隔离区）+ 双击定位 | 文件搜索 Tab | 可用 | |
| 9 | 大文件扫描（minMB/扩展名/盘符、topN 200、修改时间列带年份、列排序、虚拟列表） | 大文件 Tab | 成熟 | H3 已修：默认全部固定盘（aa992e3） |
| 10 | 三级去重（size→头 64KB→全量 SHA-256，[保留]最新/预选其余） | 大文件 Tab | 成熟 | **对硬链接盲**（全仓无 nNumberOfLinks——见 §2-D2） |
| 11 | 应用迁移（复制+永久删源+Junction/Symlink、四步预检含 RmGetList、还原点、失败回滚、迁移后自检、UWP 排除） | 应用迁移 Tab | 成熟 | |
| 12 | 文件夹分析（v2.18 起 depth=2 两级 + 双击下钻） | 文件夹分析 Tab | 可用 | 仍**串行**定大小（上轮 #3 遗留未做） |
| 13 | 右键"移入隔离区…"（走 PlanBuilder+GuardRails） | 文件夹分析 Tab | 可用 | |
| 14 | 操作历史（JSONL+签名+TSV 迁移；类型/状态/风险中文标注） | 操作历史 Tab | 成熟 | |
| 15 | 批量撤销（worker 线程、签名校验） | 历史页 | 成熟 | |
| 16 | 清空隔离区（遍历各盘 MiniSys.Quarantine 全删） | 历史页按钮 | 可用 | **全有或全无**；无浏览/预览/单条永久删除（History 页无右键菜单，MainWindow.cpp:2180 明示跳过） |

### 1.2 横切能力

| # | 功能 | 入口 | 成熟度 | 备注 |
|---|---|---|---|---|
| 17 | GuardRails（规范化/TOCTOU 复验/云占位/用户排除清单/双上下文） | 内部 | 成熟 | SetUserExclusions API 在（GuardRails.h:76） |
| 18 | PlanBuilder + planHash（PlanStale 整单中止） | 内部 | 成熟 | |
| 19 | 预览执行 dry-run（GuardRails 批量校验，worker） | 右键/命令面板 | 成熟 | 无字节量预测 |
| 20 | 列表校验刷新（手工 + WM_ACTIVATE 归来 + 空闲修剪幽灵行） | 右键 | 成熟 | |
| 21 | 命令面板 Ctrl+K（**18 条**：8 操作 + 6 切页 + 4 筛选） | Ctrl+K | 可用 | 命令静态构造，无别名 |
| 22 | 快捷键（F5 扫描/取消、Ctrl+A 全选切换、Ctrl+K、Tab 导航） | 全局 | 成熟 | Ctrl+A 是"全/无"切换（MainWindow.cpp:2112），**无反选** |
| 23 | 设置持久化（11 字段含 exclusions/devCacheRoots） | 自动 | 成熟 | Settings.h 核对：无 indexDrives/别名/预设字段 |
| 24 | 会话恢复（六 Tab 结果缓存 + 查询/勾选/页面） | 启动自动 | 成熟 | TryLoadCachedResults×6 仍在 UI 线程启动（MainWindow.cpp:817，上轮 #4 遗留） |
| 25 | 空闲维护（5min 空闲→倒计时重扫；10-30min→USN 增量+幽灵修剪；冷却可取消） | 自动 | 成熟 | 只保鲜数据，**从不执行任何清理** |
| 26 | 单实例 + `-takeover` 构建轮换接管 | main.cpp | 成熟 | 用户工作流核心 |
| 27 | 虚拟列表 LVS_OWNERDATA（千行 70ms；勾选 map 权威化） | 全列表 | 成熟 | v2.15 + L-1/L-3 路由修复 |
| 28 | 风险列+行染色 / Win32ErrorText 人话 / RM 占用诊断（LockingProcesses 已入 QuarantineOp 失败路径）/ minidump | 内部 | 成熟 | Round-3 F2/F4/F5 落地核实 |
| 29 | -tour 巡页 / -probe 搜索自检 / -takeover | 命令行 | 可用 | **评审与工作流工具**，无 -scan/-report 等用户 CLI |
| 30 | 孤儿：DeleteOp（仍无构造点）、OpType::MoveFilePath、IDC_BTN_REFRESH/DEDUP（resource.h:16/20 仍在）、ScanItem.dangerous（写而不读，ui/ 零读取点） | — | 孤儿 | 上轮 #10 清理未做 |

### 1.3 会误导提议的「文档 vs 代码」差异

| # | 差异 | 证据 | 对提议的影响 |
|---|---|---|---|
| D1 | **v2.17 起应用是 asInvoker**（app.manifest:8），ARCHITECTURE.md §1/ADR-010 仍写 requireAdministrator/管理员运行 | grep 核实 | 任何依赖"反正已提权"的提议（HKLM 写、Program Files 迁移、DISM 委派）都要考虑未提权时的降级路径 |
| D2 | **去重对硬链接零感知** | 全仓 grep nNumberOfLinks/BY_HANDLE_FILE_INFORMATION 零命中 | 硬链接对（WinSxS、编译器输出、OneDrive 遗留）会被判"重复"，[DELETE] 行声明的可释放字节是**虚的**（删一个名字释放 0） |
| D3 | **无任何导出通道**：全仓 grep Clipboard/剪贴板/CSV/导出 零命中 | grep | "导出报告"是真空白，不是"已有部分" |
| D4 | ARCHITECTURE.md 仍是 v1 基线（其自身已声明），以 DESIGN-v2-实施报告 + 代码为准 | 文档自述 | 沿用上轮 D4 结论 |
| D5 | evolve-log 曾有失实叙事（V1"修好不存在的按钮"、283 万项出处），REVIEW_2026-10-11 §1.3 已更正 | 文档 | 引用历史数据以 REVIEW 更正后为准 |
| D6 | 需求.txt 写"C 盘"，产品定位已是"所有固定磁盘瘦身"（v2.11）；用户实际使用横跨多盘 | 需求.txt vs MainWindow 注释 | 新功能按多盘定位评估 |

---

## 2. 上轮提议落地情况复核

### 2.1 Round-3（2026-09-30，F1–F18，基线 v2.1）

| 提议 | 状态 | 证据 |
|---|---|---|
| F1 风险摘要弹窗 / F2 风险列 / F3 解释面板 / F4 错误人话 / F6 仪表盘 / F7 预览 / F9 排除(右键) / F11 设置 / F12 取消+进度 / F13 单实例 / F15 降级可见化 | **全部落地** | 代码逐一核实（Controls.cpp 风险列、Win32Error.cpp、单实例 main.cpp 等） |
| F5 RM 占用诊断 | **落地** | QuarantineOp.cpp:45 LockingProcesses；platform/RestartManager.h |
| H1 Json 解析修复 / H2 USN 索引 | **落地**（v2.13a walk 回退补强、v2.17 端到端修复） | rules.json 真加载；v2.17b Critical 修复收尾 |
| F17 隔离区专属视图 / F18 上次清理提示 | **未落地** | 见 §2.2 C5 |

### 2.2 Round-4（2026-10-11，C1–C12，基线 v2.14）

| 提议 | 状态 | 证据（grep/读码） |
|---|---|---|
| H3 大文件默认全盘 | **落地**（aa992e3） | 上轮执行总账 |
| C5 隔离区浏览器 | **未落地** | 无 PurgeQuarantined；History 页无右键菜单 |
| C2 扩展名空间报表 | **未落地** | 无 ExtStat 聚合 |
| C12 排除清单管理 UI | **未落地** | 无 ManageExclusions；仍只有右键添加+手改 settings.json |
| C3 计划导出/回放 | **未落地** | 无 plans/ 目录与回放入口 |
| C1 索引磁盘选择 | **未落地** | Settings 无 indexDrives |
| C4' 空闲预备计划 | **未落地** | 无 pendingPlan |
| C7' 云占用报告 / C9 别名 / C10 规则导入导出 / C11 CLI | **未落地** | grep 零命中 |

**结论**：v2.15→v2.19a 共 12 个提交全部是性能（虚拟列表）、修 bug（搜索链路、L-1/L-3）、视觉（v2.16/2.19）、工作流（rotate/takeover）——**零新用户功能**。上轮 08 系 P0（十件事 #6/#7/#8/#10）整体未动，功能管线停滞约 3 周。本轮这些项**仍有效且优先级应继承**，下文不重复展开、只给增量评估。

---

## 3. 候选功能卡片总表

成本：S≈1-2 天（接线/复用现成零件）/ M≈3-5 天（新小模块）/ L>1 周。价值 1-5。边界对照需求.txt #3（确认制）。

| ID | 候选 | 价值 | 频次 | 成本 | 主要复用 | 风险 | UI 复杂度 | 优先级 |
|---|---|---|---|---|---|---|---|---|
| N1 | **开发/游戏环境缓存规则包扩展**（vcpkg downloads+buildtrees、conda pkgs、pnnpm/yarn store、uv cache、Go mod cache、Docker Desktop WSL vhdx、Unity Library、UE DerivedDataCache、Steam workshop/compatdata、NVIDIA/DX 着色器缓存） | 5 | 每次清理 | S（rules.json 数据 + DevBuildCache 名表增行） | JunkRule 结构/Subtree 模式/minAgeDays、DevBuildCache kSafe/kCautious 表、LoadValidated 校验 | 低（路径均为用户空间，天然过 Quarantine 白名单）；Docker/Steam 需 age>7d 且 Cautious | 无新 UI | **P0** |
| N2 | **空间释放预测 + 执行对照**（确认弹窗分三层：立即释放 Y(回收站/委派)、隔离待确认 Z、拒项 W；执行报告对照预测） | 5 | 每次执行 | S | ScanItem.sizeBytes/riskLevel/strategy、ExecuteReport 已有 quarantinedBytes/freedBytes | 低（纯内存聚合）；话术必须讲清"隔离≠释放" | 弹窗文本升级，无新控件 | **P0** |
| N3 | **列表/报告导出**（右键"导出 CSV"：当前列表全列→文件/剪贴板；扫描仪表盘导出） | 4 | 每周/复盘 | S | snapshot_/cells_ 现成、Json::Dump、results-*.json 序列化管线 | 低（UI 线程纯内存遍历 1000 行 <10ms；写盘原子化沿用 Settings 模式） | 一个菜单项 | **P0** |
| N4 | **去重硬链接感知**（重复簇成员 GetFileInformationByHandle 比 file index；同 ID→标注"硬链接，删除不释放空间"并扣减可释放字节） | 4 | 每次去重 | S | 去重三阶段后追加第 4 步；仅对已过全量哈希的簇成员开句柄（数量小） | 低-中：句柄打开失败降级为现状；只改显示与字节口径，不自动跳过（保留用户判断） | 一列/徽标文本 | **P0** |
| N5 | **批量选择扩展**（右键+面板：反选、只勾"安全"级、只勾 >N MB、按分类勾/不勾） | 4 | 每次大列表清理 | S | checkStateByPath_ map、SetAllChecked 批量写模式、右键菜单框架（MainWindow.cpp:2183） | 低 | 3-4 个菜单项 | **P0** |
| N6 | **隔离区浏览器**（=上轮 C5：浏览/预览/单条永久删除/到期策略提示） | 4 | 清空决策前 | M | HistoryPresenter、OpRecord.target、RunEmptyQuarantine 单条化、签名校验 | 低（全走既有确认链）；单条 Purge 后记录重签需核对 SecretStore 覆盖面（上轮未核实项 #3 仍开） | History 右键子菜单 | **P1**（继承） |
| N7 | **自定义快速筛选预设 + 保存视图**（用户把当前 query+matchPath 命名保存；菜单下半区列出；可含排序意图） | 4 | 高频（用户主路径是搜索） | S-M | kPresets 表改为静态+用户两段渲染、Settings 增 searchViews 字段、ToggleQueryToken | 低 | 菜单两段 | **P1** |
| N8 | **扩展名空间报表**（=上轮 C2 档1：LargeFileScanner entries 内存聚合 top-15 进 info 条；档2：搜索页 ext:report 借 v2.19 大小池） | 4 | 瘦身决策 | S | entries 全量在内存；v2.19 并行 GetFileAttributesExW 池是**新零件** | 低（零新增遍历） | info 条一行+右键复制 | **P1**（继承） |
| N9 | **磁盘空间趋势**（每次启动 QueryDiskSpace×盘 采样→JSONL 追加（每盘每日一行）；info 条/关于页显示"较上次 +X GB 可用"；系统盘低于阈值红色提示=磁盘满告警的应用内形态） | 3 | 每次启动 | S-M | QueryDiskSpace、Logger 目录、info 条 | 低（<1ms 采样）；文件增长 365 行/盘/年可忽略 | 一行文本 | **P1** |
| N10 | **排除清单管理 UI**（=上轮 C12） | 3 | 一次配置 | S | RootsDlg 模板（已修好的版本）、Settings.exclusions、SetUserExclusions | 低 | 一个对话框 | **P1**（继承） |
| N11 | 搜索结果"继续加载"（突破 1000 行：全量计数 + "显示更多"分页；虚拟列表已可承载） | 3 | 搜索命中>1000 时 | S-M | Search sink 预算参数化、LVS_OWNERDATA | 中：全量计数=整索引扫（实测 76ms/百万条，可接受但需可取消） | 一个按钮/提示行 | P2 |
| N12 | 清理计划导出/回放（=上轮 C3，"回放=勾选记忆"红线） | 3 | 例行人群 | M | ScanItem↔Json、PlanBuilder、复验链 | 中（防无人值守化漂移） | 弹窗复选+面板入口 | P2（继承） |
| N13 | 空闲预备计划（=上轮 C4'，预备不执行） | 3 | 每日 | M | OnIdleCheck、确认弹窗 | 中（与确认制一线之隔） | hint 一条 | P2（继承） |
| N14 | 迁移后深度校验（复制完成后 DirectorySize 双向对比源/目标 字节+项数，不等→标记警告） | 2 | 迁移失败罕见 | S | DirectorySize、OpRecord.note | 低（迁移本就慢，加秒级可接受） | 报告一行 | P2 |
| N15 | CLI 伴侣（-scan/-report JSON）/ `--dump-report` 轻量版 | 2 | 脚本化 | M/L | results-*.json、LastReport | asInvoker 后无 UAC 障碍，但公共代码抽取仍欠 | 无 | Backlog（触发：用户首次要终端报告；轻量版可搭 N3 顺车） |
| N16 | 云文件占用只读报告（=上轮 C7'） | 3 | OneDrive 用户 | S-M | 索引 attrs、GuardRails 云占位拒绝 | 低 | info 行 | P2/Backlog（继承） |
| N17 | 规则导入/导出（=上轮 C10 无签名版） | 3 | 低频 | M | LoadValidated 双白名单 | 低 | 面板入口 | Backlog（继承；N1 落地后价值上升） |
| N18 | 回收站逐项按大小排序清理 | 2 | 低 | M（IShellFolder 枚举回收站，API 冷僻） | — | 与"整体清空"的主用法重叠；收益/复杂度比差 | 新视图 | **不做**（见 §6） |
| N19 | 托盘常驻+磁盘满告警 | 2 | — | M | — | 与信任叙事冲突（连续三轮否决）；告警需求由 N9 应用内形态承接 | — | **不做** |
| N20 | 定时自动清理（自动执行版） | — | — | L | — | 违反需求.txt #3 确认制 | — | **不做** |
| N21 | 多语言 | 1 | — | L（全 UI 字符串资源化） | — | 目标用户即中文单语；重构风险>收益 | — | **不做**（发布国际化时再议） |
| N22 | AI 辅助风险解释 | 2 | 低 | M | — | "说明…"对话框+detailHint+Win32ErrorText 已覆盖规则级解释；本地规则再包一层 AI 文案是伪需求 | — | **不做**/Backlog |
| N23 | 启动项/DLL 侧载扫描 | 2 | 低 | L | — | 定位漂移到安全工具域；Autoruns 已是免费标杆 | 新 Tab | **不做** |
| N24 | 硬链接"合并释放"（把内容相同的独立副本转成硬链接以省空间） | 3 | 低 | L | N4 的检测层 | 高：硬链接语义对普通用户不可见，应用更新/部分写入会互踩；与"100% 安全可逆"承诺冲突 | — | **不做**（N4 只读感知保留） |
| N25 | 近重复 pHash / $MFT 直读 / EV 签名 / TRIM / SMART | — | — | — | — | 维持三轮前结论不变 | — | 不做 |

---

## 4. Top 10 深入（排序 + 实现草案）

排序原则：命中用户主路径（搜索→勾选→清理）与既证价值观（信息完整/可逆/速度）> 信任正确性缺陷 > 上轮遗留 M 项 > 新视图。

### #1 N1 开发/游戏环境缓存规则包扩展（P0，S）

- **价值论据**：目标用户=中文 Windows 个人开发者。现规则覆盖 npm/pip/cargo/maven/gradle/nuget + VS 系产物（rules.json id 表核对），但 2026 年开发者栈的磁盘大头**全部缺席**：vcpkg（buildtrees/packages 常年 20-50GB）、conda pkgs、pnpm/yarn 全局 store、uv cache、Go mod cache、Docker Desktop 的 ext4.vhdx（动辄 30-60GB）、Unity Library/、UE DerivedDataCache/、Steam compatdata/workshop、显卡着色器缓存。用户近期轨迹（dev-cache 根 UI、ext:pdb 工作流）证明这是其核心痛点。
- **接口**：纯数据——rules.json 增 Subtree 规则（路径 %USERPROFILE%\.conda\pkgs、%LOCALAPPDATA%\vcpkg、%LOCALAPPDATA%\pnpm\store、%LOCALAPPDATA%\uv、%USERPROFILE%\go\pkg\mod、%LOCALAPPDATA%\Docker\wsl\data\ext4.vhdx* 用 Children+childPattern、Unity/UE 走 DevBuildCache 名表增行 Library/DerivedDataCache/obj/Intermediate→Cautious）。Docker vhdx/Steam 设 minAgeDays≥7、Cautious、recommended=false；着色器缓存 Safe（自动重建）。
- **数据流**：零代码改动即可上大半（rules.json 热加载）；DevBuildCache 名表增行是其唯一代码点（kCautious set 加 Library/DerivedDataCache/Saved/Intermediate——需验证不误伤：Library 含 Settings 资产，应 Cautious 不预勾）。
- **UI 入口**：无新 UI。垃圾 Tab 规则行自然出现。
- **测试**：JunkRulesTests 增规则条目断言；对抗面关键——**每条新路径必须过 LoadValidated 的 Quarantine 白名单**（用户空间天然通过）；Unity Library 手工验证"隔离后 Unity 打开自动重建无报错"。
- **风险**：Docker vhdx 是文件不是目录且被 wsl 服务锁定——规则应 InfoOnly 提示（"请在 Docker Desktop 中 Compact"）而非可执行；Steam compatdata 删除导致 Proton 重装，Cautious+detailHint 写明后果。

### #2 N2 空间释放预测 + 执行对照（P0，S）

- **价值论据**：用户价值观="信息完整"。当前确认弹窗只说 N 项+总大小；执行后 ExecuteReport 有 quarantinedBytes/freedBytes 但**执行前不区分"真释放"与"进隔离区暂不释放"**——对"隔离区 2.13GB 积压"的这位用户，这是最需要的一句话。
- **接口**：新纯函数 `SummarizeRelease(items, checked) → {immediate, quarantined, denied, irreversible}`（immediate=EmptyRecycle+Delegate 项字节；quarantined=Quarantine 策略字节；irreversible 标志）。OnExecute 弹窗三行："立即释放约 X GB / 移入隔离区 Y GB（清空隔离区后才真正释放，当前已积压 Z GB）/ 不可逆 N 项"。OnPlanDone 报告追加"实际 vs 预测"一行。
- **复用**：QuarantineUsageText（Z 值现成）、riskLevel/strategy/sizeBytes、ExecuteReport。
- **测试**：SummarizeRelease 纯函数单测（混合策略分组、空集、全 InfoOnly）；手工：勾隔离+清回收站混合项弹窗三行正确、执行后对照行数字自洽。
- **护栏**：零新增 I/O，O(n) 内存聚合。

### #3 N3 列表/报告导出（P0，S）

- **价值论据**：全仓零导出通道（§2-D3）。用户是开发者——把 ext:pdb 结果发给同事/存档复盘/在 Excel 里二次分析是自然动作；仪表盘导出满足"清理前证据留存"。
- **接口**：右键菜单两项：「复制为 TSV」（剪贴板，Excel 直贴）、「导出 CSV…」（GetSaveFileNameW → 原子写）。数据源=presenter 的 snapshot_（含未在视口内的行）；列=分类/风险/项目/大小/路径/修改时间/groupKey。仪表盘侧：info 条右键「导出当前摘要」。
- **复用**：snapshot_/cells_、格式化函数、Settings 的 temp+rename 原子写模式。
- **测试**：CSV 转义单测（逗号/引号/换行路径）；1000 行导出 <50ms 断言（性能护栏）。
- **边界**：只读操作，不进 GuardRails。

### #4 N4 去重硬链接感知（P0，S，正确性缺陷）

- **问题**：§2-D2——硬链接对被判"重复"后 [DELETE] 行虚报可释放字节；用户删完发现盘没变，信任受损。
- **接口**：去重簇全量哈希通过后，对每成员 `CreateFileW(READ_ATTRIBUTES)+GetFileInformationByHandle` 取 (VolumeSerial, FileIndex)；同 ID 成员归并为一"硬链接组"：组内除一个名字外均标注 detail 前缀 `[硬链接组·删除不释放空间]`，并从该簇的可释放字节合计中扣减。
- **数据流**：仅对已是"重复簇"的文件开句柄（topN 200 内的簇，数十次），非全量。
- **UI**：不自动取消勾选（保留用户判断权，与 GuardRails deny-only 哲学一致），仅信息标注+弹窗汇总里单列"其中硬链接 0 字节项 N 个"。
- **测试**：单测用 CreateHardLinkW 构造硬链接对断言归并与扣减；句柄失败（分享冲突）→ 行为=现状（无回归）。

### #5 N5 批量选择扩展（P0，S）

- **接口**：右键菜单"选择"子菜单 + 命令面板各加：反选 / 只勾"安全"级 / 只勾 >100MB / 按当前分类勾选。实现=对 checkStateByPath_ 的批量谓词写（复用 SetAllChecked 的一次重绘模式，天然无消息风暴）。
- **测试**：谓词纯函数单测；1000 行下批量勾选 <50ms。
- **理由**：1000 行搜索结果里手挑 30 项是用户现役痛点；反选是 Everything 用户肌肉记忆。

### #6 N6 隔离区浏览器（P1，M，上轮继承，简述增量）

上轮 C5 设计仍成立（History 右键：打开隔离区位置/预览/单条永久删除/还原；PurgeQuarantinedAsync 抽自 RunEmptyQuarantine）。**增量**：①积压已从 1.5GB 涨到 2.13GB/50 项，紧迫性上升；②到期策略提示（>14 天的隔离项在 History 行加"建议确认"标记）可一并做，数据=OpRecord.timestamp 现成；③前置核对项=上轮未核实 #3（Purge 后记录重签 vs HMAC）。

### #7 N7 自定义快速筛选预设（P1，S-M）

- **接口**：Settings 增 `searchViews: [{name, query, matchPath}]`；快速筛选菜单渲染为"内置 16 项 ─ 用户预设 ─ 保存当前筛选…/管理…"三段。保存入口=菜单项（把 editSearch 文本+matchPath 存档）；管理=RootsDlg 模板列表对话框（可与 N10 同批做）。
- **数据流**：ToggleQueryToken/ParseFilterTerms 全部现成；应用=填 editSearch+RunSearch。
- **测试**：settings 往返单测；查询串含特殊字符（分号）保存/恢复不变形。

### #8 N8 扩展名空间报表（P1，S，上轮继承）

档 1（LargeFileScanner entries 聚合）不变；**新增可行性**：v2.19 的并行属性池证明了"任意 1000 行批量取大小 = 几十毫秒"，档 2（搜索页 `ext:report`：全索引按扩展名计数 + 头 N 行取大小外推）现在成本更低。验收：`.pdb 1.2万项/38GB` 一行式摘要 + 点行预填 `ext:pdb`。

### #9 N9 磁盘空间趋势 + 应用内磁盘满提示（P1，S-M）

- **接口**：启动时（TryLoadCachedResults 之后、可挪 worker）QueryDiskSpace×每固定盘 → `cache/space-trend.jsonl` 追加 `{date, drive, free, total}`（同日幂等覆盖）；info 条/关于页显示"C: 可用较上次 +12.3 GB"；系统盘 free<15GB 时 info 条首行红色警示（承接"磁盘满告警"诉求的应用内形态，不做托盘）。
- **价值**：闭环"清理到底有没有用"——把一次性的清理动作变成长期曲线，契合用户"信息完整"偏好；数据量微不足道。
- **测试**：幂等写单测；日期边界（跨午夜启动两次只记一行）。

### #10 N10 排除清单管理 UI（P1，S，上轮继承）

RootsDlg 模板复刻 + Settings.exclusions 往返 + SetUserExclusions 热更新（SaveSettings 链路已接）。上轮设计无需修订，此处仅确认仍缺失（§2.2）。

---

## 5. 分阶段路线图

| 阶段 | 内容 | 主题 | 预估 |
|---|---|---|---|
| **Now（1-1.5 周）** | N1 规则包（数据为主）；N2 释放预测+对照；N3 导出；N4 硬链接感知；N5 批量选择 | "数字是真的，动作是批的，证据带得走" | S×5 |
| **Next（2-3 周）** | N6 隔离区浏览器（含到期提示）；N7 自定义预设；N8 扩展名报表（两档）；N9 空间趋势；N10 排除管理 UI；上轮遗留工程项（UI stall watchdog / 启动异步 / 孤儿清理——归 02/06 报告，此处不抢） | "黑箱开窗，路径记忆" | M×2+S×3 |
| **Later（1 个月）** | N11 搜索分页；N12 计划导出/回放；N13 空闲预备计划；N14 迁移深度校验；N16 云占用报告 | "例行自动化到确认制边界为止" | M×4 |
| **Backlog** | N15 CLI/--dump-report；N17 规则导入导出；索引盘选择（上轮 C1）；FolderTree 并行化/内存紧凑化（03 遗留，工程侧）；日志轮转 | 触发条件驱动 | — |

依赖：N6 依赖签名重签核对；N12/N13 依赖"勾选记忆"红线设计；N8 档 2 依赖 N11 的全量计数能力可复用。

---

## 6. "不该做"清单及理由

1. **定时自动清理（自动执行版，N20）** — 需求.txt #3 用户亲书确认制；自动移动文件同样是未确认破坏性行为。N13 是边界内最大公约数。
2. **托盘常驻/开机自启（N19）** — 连续三轮否决不变；磁盘满告警改由 N9 应用内形态承接，避免为一个小诉求引入常驻进程。
3. **硬链接合并释放（N24）** — 与 N4 相反方向：主动创建硬链接会让"删 A 坏 B"的不可见耦合进入用户数据，违背"100% 安全可逆"；检测（只读）做，合并不做。
4. **回收站逐项按大小清理（N18）** — 回收站逐项枚举需冷僻 shell API（M 成本），而主用法是整体清空；排序信息价值低。折中：清空前弹窗已有总大小，N2 落地后预测更完整。
5. **多语言（N21）** — 目标用户中文单语；全 UI 字符串资源化是 L 级重构，收益为零。
6. **AI 风险解释（N22）** — 规则级解释已被"说明…"（detailHint 三段式）+Win32ErrorText 覆盖；再包一层"AI 文案"是营销词不是功能。
7. **启动项/DLL 侧载扫描（N23）** — 定位漂移至安全域，Sysinternals Autoruns 免费且权威。
8. **近重复 pHash / $MFT 直读 / EV 签名 / TRIM / SMART / 执行前全量 RM 预检** — 维持前三轮结论，本轮无新证据推翻。

---

## 7. 性能护栏（新功能硬约束）

1. **零扫描期新增 I/O**：N2/N3/N5/N7 全部只消费内存 snapshot_/静态表；N8 档 1 聚合 entries 为 O(n) 内存遍历。
2. **按需小规模 I/O**：N4 仅对重复簇成员开句柄（数十次 GetFileInformationByHandle）；N8 档 2/N11 全量计数走 worker + 可取消（CancelScan_ 语义已有），76ms/百万条的实测基线为预算上限。
3. **导出即内存遍历**：N3 在 UI 线程遍历 ≤1000 行（虚拟列表时代行数上限即预算），写盘用 temp+rename；>50ms 则挪 worker。
4. **趋势采样限启动一次**：N9 每盘一次 QueryDiskSpace（<1ms），禁止运行期轮询。
5. **批量 UI 写走 map**：N5 复用 SetAllChecked 的"批量写 map + 单次重绘"模式，禁止逐行 ListView 消息（v2.15 前的 O(N²) 教训）。
6. **新持久化一律原子写 + 幂等**：N7/N9 的 settings/searchViews、space-trend.jsonl 沿用 Settings::Save 模式。
7. **UI 线程纪律不变**：一切索引/盘 I/O 在 SessionService worker；新增 DONE 消息必须带任务代数（taskGen_），旧任务消息丢弃。

---

## 8. 特别加分：现有零件拼装清单（复用密度）

| 已有零件（本轮新确认/继承） | 只差什么 | 拼出 |
|---|---|---|
| rules.json 规则管线（校验/降级/热加载已真通） | 纯数据条目 | N1 全部缓存规则 |
| ExecuteReport.quarantinedBytes/freedBytes + QuarantineUsageText | 执行前聚合一次 | N2 预测+对照 |
| snapshot_/cells_（虚拟列表后备） + 原子写模式 | 转义+剪贴板 | N3 导出 |
| 去重三阶段管线（groupKey/DUP- 前缀） | 簇内 file index 比对 | N4 硬链接感知 |
| checkStateByPath_ map + SetAllChecked 批量模式 | 谓词函数 4 个 | N5 批量选择 |
| **v2.19 并行 GetFileAttributesExW 大小池（新零件）** | 复用到任意路径集 | N8 档 2、隔离区条目大小显示 |
| kPresets 表 + ToggleQueryToken + Settings | 菜单两段渲染 + 3 字段 | N7 自定义预设 |
| QueryDiskSpace + info 条 + jsonl 追加 | 一次采样+一行渲染 | N9 趋势 |
| RootsDlg（修好的版本） | 内容换成 exclusions | N10、N7 管理 |
| HistoryPresenter rows_ + RunEmptyQuarantine | 单条化 + 右键 | N6 隔离区浏览器 |

结论：Top 10 中 8 项核心是"接线"——本轮与三轮前同构，但零件库更大（v2.19 大小池是新增的可复用资产）。

---

## 9. 未核实项

1. **N1 各缓存路径的社会化校验**（vcpkg/conda/pnpm/uv/Go/Docker/Unity/UE/Steam 在不同安装方式下的实际布局与版本差异）未逐一调研——实施时需在真机逐条验证，本文按公开常识路径评估（推断）。
2. **Unity `Library/` 的"Settings 资产"例外**：Library 内含可再生物与部分配置的混合，Cautious 级+不预勾是否足够安全，需 Unity 用户实测（推断：隔离策略下可逆，风险可控）。
3. **N6 单条 Purge 后记录重签与 HMAC 覆盖面**：OperationLog UpdateStatus 签名路径未逐行核对（上轮未核实 #3 延续）。
4. **数据面数字**（索引 3M 项/267MB、隔离区 50 项 2.13GB、历史 93 条）来自任务给定，未读用户数据复核。
5. **v2.19 大小池"全页几十毫秒"** 为代码注释自述（SessionService.cpp:539），未实测；N8 档 2/N11 的预算以此为据。
6. **asInvoker 下 Program Files 应用迁移、DISM 委派、隔离区写入系统盘根**的实际降级行为未实测（v2.17 改 manifest 的影响面无专项测试记录）——影响 N1 中 Docker/Steam 类规则在未提权会话的表现。
7. rules.json 28 条规则的 minAgeDays/detailHint 当前具体值未逐条读取（仅核对 id 清单与结构）。
