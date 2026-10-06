# MiniSys v2.3 UI 布局与交互设计复审报告（07-ux-re）

> 评审人角色：交互设计师 + 可用性工程师 · 日期：2026-10-06 · 代码基线：master@5eae1d2 + 工作树（v2.3）
> 对象：上轮评审（docs/review-2026-10-06/07-ux.md，X-1..X-24）之后的两轮大改（v2.2 评审路线图全部实施 + v2.3 Everything 式文件搜索 Tab）
> 焦点：布局与交互设计（视觉层级、信息密度、控件编排、操作流、反馈与状态可见性、文案）。安全/后端/架构不在范围。
> 证据标记：[V] = 亲自读到的代码/数据；[I] = 框架语义推断，需活体复核。置信度：Confirmed / Likely / Speculative。
> 无 GUI 活体（MiniSys.exe 正在用户桌面提权运行，禁止启动/附着）；用户数据 %LOCALAPPDATA%\MiniSys 仅只读。

---

## 0. 证据基础

- UI 代码全读：`MiniSys\src\MainWindow.cpp`(1128 行)、`src\ui\Presenters.cpp`(606)、`Controls.cpp`(143)、`Layout.cpp`(104)、`Icons.cpp`(48)、`src\res\resource.h`、`core\JunkRules.cpp`、`JunkScanner.cpp`、`LargeFileScanner.cpp`、`AppScanner.cpp`、`SessionService.cpp`、`PlanBuilder.cpp`、`VolumeIndex.h/.cpp`(节选)、`main.cpp`、`MiniSys\rules.json`。
- 用户日志（只读，C:\Users\tjf\AppData\Local\MiniSys\logs\minisys.log）：
  - 14:45 会话：14:45:13 启动 → 14:45:48 `VolumeIndex: C: indexed 27 entries`（**建索引耗时 35 秒**）；垃圾扫描 22 项 30,324ms；tab=1 扫描 242 项 76,796ms。
  - 15:55 会话（v2.3，当前运行中）：15:55:07 启动 → 15:55:38 `indexed 27 entries`（31 秒）。
  - 【待解释观察】真实系统卷索引仅 27 项（正常 C 盘应为百万级）。根因不在本次范围；但布局上 `UpdateSearchStatus` 的"索引就绪：N 项 / 索引共 N 项"直接把 27 展示给用户（见 L-24）。
- 无 settings.json（LoadSettings 走默认值）。

---

## 1. 总体结论

v2.2/v2.3 把上轮"风险沟通缺失"的主干补上了：风险列+徽标+行染色、风险分组确认弹窗、默认取消、动态执行按钮、进度条归位、批量撤销、键盘可达、列表右键菜单——骨架方向正确且大部分实装到位。**但复审发现两类新问题：**

1. **一批"声称已修复"的项实际打折或未动**（详见 §3 核查表）：最重的三个是——历史页列头与数据完全错位（列 1"风险"84px 里塞 19 字符时间戳）、用户"取消勾选"在排序后被静默重新勾上（安全工具的输入被篡改）、X-16 的 advancedChk 溢出与列表压状态栏坐标一个都没改。
2. **修复之间互相打架**：info 标签固定 36px 高，而降级告知/dashboard/搜索计数全都往里塞——X-10 的"降级可见化"文本追加在尾部，超出 36px 的行大概率永远看不见；布局常量按 DPI 放大而窗口默认尺寸不变，150% 下右侧按钮反而溢出；进入搜索 Tab 的自动建索引把整套 UI 锁 31 秒（实测日志时长）。

一句话：**仪表盘接上了线，但有几根接错了插座。**

---

## 2. 问题清单（按严重度排序）

严重度：P0 阻断 / P1 严重 / P2 一般 / P3 打磨。行号以当前工作树为准。

### P0

**L-1 用户"取消勾选"在排序/切 Tab 后被静默撤销——recommended 项重新被勾上**
- 位置：[V] `src\ui\Presenters.cpp:53-89`（RenderItems）。
- 现象：v2.2 的勾选保持（X-6 修复）只快照了 `wasChecked`（勾选集合）；渲染时 `check = it.recommended`，命中 wasChecked 才强制 true。**没有 wasUnchecked 集合**：用户在垃圾清理 Tab 显式取消勾选一个默认勾选项（如 npm 缓存）→ 点"按大小排序"或切 Tab 回来 → 该项恢复勾选，且无任何提示。
- 为什么伤：这是安全工具。用户的勾选状态就是其确认意志的表达；排序（挑选大文件的必经步骤）静默恢复厂商默认勾选，用户若不再逐项核对就直接执行，会把刚被排除的项移入隔离区。需求.txt 第 3 条"所有的迁移和删除都需要我确认"——确认的对象被工具偷偷改了。
- 建议：RenderItems 快照双向集合：`wasChecked`（勾）与 `wasUnchecked`（用户取消过的 recommended 项，按 path），恢复优先级 userUnchecked > wasChecked > recommended。或干脆把快照从"勾选集合"改为"完整勾选位图"。
- 证据：Confirmed（代码机制确凿；触发路径不需要任何异常条件）。

**L-2 历史页列头与数据完全错位：时间戳塞 84px"风险"列、长路径塞 100px"大小"列**
- 位置：[V] `src\ui\Controls.cpp:31-35`（共享列头：分类 180 / 风险 84 / 项目 330 / 大小 100 / 详情 320）+ `src\ui\Presenters.cpp:519-541`（HistoryPresenter::Refresh 的列填充）。
- 现象：历史页 5 列实际内容为：col0=类型·状态（"已隔离 · 成功"）；col1=**时间戳** `"2026-10-06 15:55:07"`（19 字符 ≈133px，塞进 84px 的"风险"列 → 截断成 "2026-10-06 15:"）；col2=**大小数值**（"1.46 GB" 塞 330px"项目"列）；col3=**source → target | note**（长路径塞 100px"大小"列 → 只能看到前几个字符）；col4=风险标签（"🛡 可还原" 塞 320px"详情"列）。
- 为什么伤：历史页是"隔离区可逆"这一产品核心卖点的唯一操作面（撤销/清空都在这）。列头说"风险"下面是日期、说"大小"下面是路径——用户找不到要撤销哪条记录，也不信任看到的数字。v2.2 声称"历史语义列"修复（列 0 内容人话化 ✓），但列头与列宽是全 Tab 共享的，内容改了容器没改。
- 建议：切 Tab 时用 `LVM_SETCOLUMN` 动态改列头文本与宽度（History: 类型 140 / 时间 150 / 大小 90 / 来源→去向 自适应 / 状态 90）；或历史页专用 ListView。
- 证据：Confirmed（两处代码交叉确凿；截断表现按 9pt Segoe UI 字宽推算为 Likely，误差不影响结论）。

### P1

**L-3 info 标签固定 36px 高：降级告知/dashboard 追加后 4-6 行被裁——X-10"降级可见化"实际大概率不可见**
- 位置：[V] `src\ui\Layout.cpp:89`（`SetWindowPos(ui.info, …, Scale(36, dpi))` 高度恒定）+ `MainWindow.cpp:594-633`（OnScanDone：degrade 追加在原文**尾部**、dashboard 前置在头部）。
- 现象：垃圾扫描完成后 info 内容 = dashboard（1 行）+ 原 Tab 说明（2 行）+ 降级告知（最多 2 行："外置规则不可用…" + "快速索引未启用…"）≈ 5 行 × ~15px = 75px，而控件高 36px（约容 2-2.5 行）。dashboard 占掉第 1 行后，**追加在尾部的降级告知整段落不可见**。
- 为什么伤：X-10 的修复（规则/索引降级可见化）在这条路径上等于没修——用户永远跑在回退路径上也永远不知道。同时该标签还要承载搜索计数（UpdateSearchStatus）、执行结果（ShowHint）、排除提示——5 种用途互相覆盖且都受 36px 裁剪。
- 建议：info 高度随内容自适应（`GetTextExtentPoint32` 算行数 × 行高，OnSize 重排），或拆两个区域：第 1 行固定为"动态反馈行"（结果/计数），下方静态说明区；dashboard 与降级告知改走状态栏/通知图标。最低成本：把 degrade 从"追加尾部"改为"插入 dashboard 之后、说明之前"，并把 info 高度提到 52px（3 行）。
- 证据：Confirmed（代码：追加顺序+固定高度）；视觉裁剪 Likely（需活体确认 9pt 行高）。

**L-4 X-16 残留：Apps Tab 高级复选框仍溢出默认窗宽；列表底部仍被状态栏压住**
- 位置：[V] `src\ui\Layout.cpp:46`（advancedChk：x = pad + 5×(btnW+pad) + 80 = 8 + 740 + 80 = **828**，宽 360 → 右缘 **1188** > 默认客户区 ~1084）；`Layout.cpp:92`（`contentH = H - contentY - pad`，未减 statusH → 列表底边 = H-8，与状态栏 [H-23, H] 重叠 ~15px，状态栏后创建、z-order 在上）。
- 现象：v2.2 只改了进度条坐标；这两处坐标公式原样未动。1100px 默认窗口下，"(默认 Junction)"（~110px）几乎整段被裁；列表最后一行底部约 15px 被状态栏盖住。
- 为什么伤：上轮点名、复查指令点名，却原样残留——用户今日首次看 v2.3 就会撞上（切到应用迁移 Tab 即见）。
- 建议：advancedChk 改右锚定：`x = W - pad - 360`（并在窄窗口时与 targetBtn 最小间距约束）；列表高度 `contentH = H - contentY - statusH - pad`。
- 证据：Confirmed（坐标数学）；视觉表现 Likely。

**L-5 进入"文件搜索"Tab 的副作用：自动建索引把整套 UI 锁死 ~31 秒；主动取消后文案误导**
- 位置：[V] `MainWindow.cpp:448-453`（OnTabChanged(Search)：`BuildIndexAsync()` 成功即 `SetTaskBusy(TaskMode::Scanning)`）+ `SetTaskBusy`（646-652：tab、list、tree、exec、undo、排序按钮全部 EnableWindow(FALSE)，仅 scan 可点且文字变"取消"）+ `SessionService.cpp:118-120`（完成后 `IsValid()` 为 false 时统一显示"索引不可用（此磁盘不支持或被策略限制）…"）。
- 现象：用户第一次点开搜索 Tab（本意可能只是看看）→ marquee 出现、扫描按钮自动变成"取消"、**无法切到任何其他 Tab**，实测（日志 15:55:07→15:55:38）持续 31 秒。若用户点"取消"，BuildFull 中断 → valid_=false → 完成后状态文字显示"此磁盘不支持或被策略限制"——但真正原因是用户自己取消了，文案张冠李戴。
- 为什么伤：切 Tab 是浏览行为，不应触发全局模态式锁定；"取消"得到的解释还是错的（"磁盘不支持"）。Everything 的预期是"打开即搜"，这里变成"打开即锁"。
- 建议：① 索引构建不锁 Tab（SetTaskBusy 只禁依赖索引的动作：搜索框可保留输入、Tab 可切换、仅 exec/排序按现有矩阵禁用）；② 取消与失败区分文案：`CancelScan` 触发的中断显示"ℹ 已取消索引构建，下次进入将继续"；③ 至少在锁定期给出倒计时感（marquee + "正在构建文件索引，约半分钟"）。
- 证据：Confirmed（代码路径 + 日志时长）；体验严重度 Likely（无人观察过活体）。

**L-6 UpdateExecButton O(N) per LVN_ITEMCHANGED：1000 行搜索结果上 Ctrl+A ≈ O(N²) 秒级冻结**
- 位置：[V] `MainWindow.cpp:151-156`（任何 LVIF_STATE 变化——**含选中态变化**——都调 UpdateExecButton）+ `MainWindow.cpp:669-700`（UpdateExecButton → CollectChecked 全列表遍历，ListView_GetCheckState/GetItem 逐行跨消息）+ `MainWindow.cpp:1011-1013`（Ctrl+A 逐行 SetCheckState）。
- 现象：搜索 Tab 上限 1000 行。全选：1000 次 SetCheckState × 每次发 2-3 条 LVN_ITEMCHANGED × 每条触发一次 O(1000) 的 CollectChecked ≈ 2-3×10⁶ 次跨控件消息；每次都无条件 `SetWindowTextW`（文本不变也重发 WM_SETTEXT）。用户点击/方向键移动选中（不勾选）同样触发全量遍历。
- 为什么伤：Everything 式搜索的主用法就是"输入→全选→批量处理"；这里第一次 Ctrl+A 就把 UI 冻住（推算 1-3 秒，视机器）。勾选单行也有可感知延迟（1000 项时每次勾选 ≈ 2×O(N) 消息）。
- 建议：① 维护增量计数：LVN_ITEMCHANGED 里只读 `uNewState & LVIS_STATEIMAGEMASK` 判断该行勾选翻转，O(1) 增减计数与字节数（字节从 ItemAtRow 的 lParam 索引取）；② 文本变化才 SetWindowText（先 GetWindowText 比对或缓存上次串）；③ 排除选中态变化（`uChanged==LVIF_STATE` 且新旧 stateImage 相同即返回）。
- 证据：Confirmed（机制）；冻结时长 Likely（推算）。

**L-7 搜索 Tab 与确认弹窗/风险体系的语义错配：任意文件被套上"缓存谨慎"话术**
- 位置：[V] `Presenters.cpp:287`（SearchPresenter 构造项：`riskLevel = RiskLevel::Cautious; // unclassified`，ruleId 为空 → 徽标"—"）+ `MainWindow.cpp:766-806`（确认分组：非 Safe 非 Advanced 全归 `gCautious`，话术固定"ℹ 谨慎 N 项（X）— 清理后系统/程序需重新下载或重建"）。
- 现象：用户在搜索 Tab 搜"报告.docx"→ 勾选 → 执行 → 确认弹窗说"谨慎 1 项（2 MB）— 清理后系统/程序需重新下载或重建"——对个人文档完全不成立。风险列显示"—"、行染色不触发（Cautious 且 ruleId 空被排除），用户得不到任何针对性后果提示。
- 为什么伤：确认框话术是 v2.2 风险沟通的核心资产；在搜索 Tab 上它变成了错误信息。文件安全工具的确认文案与实际操作对象脱节，比没有文案更伤信任。
- 建议：按 Tab 分支话术：Search Tab 确认行改为"ℹ 普通文件/文件夹 N 项（X）— 将整体移入隔离区（原路径消失，程序快捷方式可能失效），可随时还原"；风险列对搜索项显示"ℹ 未评估"而非"—"，与"可执行性"区分。
- 证据：Confirmed。

**L-8 术语与语言混杂大面积残留（X-13 声称"术语中文化"，实际只中文化了一半）**
- 位置与实例（全部 [V]）：
  - **分类列（列 0）全英文**：Junk 内置表与 rules.json 的 category 为 "System Temp" / "Explorer" / "Browser Cache" / "Dev Cache" / "Windows Update" / "Windows Logs" / "Chat Files" / "System Component" / "System Reserved"（JunkRules.cpp:38-115、rules.json:4-33）、"Recycle Bin"（JunkScanner.cpp:372）、LargeFiles 的 "Image/Video/Audio/Archive/Installer/Document/VirtualDisk/Other"（LargeFileScanner.cpp:23-41）与 "Duplicate (Video)"（:256）、Apps 的 "App"（AppScanner.cpp:137）。列头是中文"分类"，列内容全英文。
  - **FolderTree 右键菜单仍是"删除文件夹…"**（Presenters.cpp:471 `AppendMenuW(hMenu, MF_STRING, IDM_CTX_DELETE, L"删除文件夹…")`）——而其确认弹窗标题已是"移入隔离区"（:481）。实施状态表声称右键菜单已改"移入隔离区…"，未改。菜单说"删除"、确认说"移入隔离区"，用户在最后一刻看到两个动词。
  - 已中化的部分确认到位：[保留]/[待删]（LargeFileScanner.cpp:260）、"清空回收站（所有磁盘）"（JunkScanner.cpp:373）、"安装位置:/卸载命令:"（AppScanner.cpp:141-143）、MoveJunctionOp 错误串（"创建 Junction 失败…"等）、LargeFileScanner 进度（"预哈希…"）。
- 为什么伤：分类列是列表第一列（180px 最宽列之一），是用户扫视分组的主要依据；英文 category + 中文列头 + 中文标题的混排直接拉低"严谨工具"的观感。"删除文件夹…"是上轮 X-13 点名的第一例，残留最刺眼。
- 建议：① JunkRules 内置表与 rules.json 的 category 加中文映射（显示层转换即可，存储不动："System Temp"→"系统临时"等 10 条 + LargeFiles 8 条 + "Duplicate (x)"→"重复文件·视频"）；② FolderTree 菜单文字改"移入隔离区…"。
- 证据：Confirmed。

### P2

**L-9 两套确认框并存：MessageBox（平文本）与 TaskDialog（command links）视觉/交互不一致**
- 位置：[V] `MainWindow.cpp:812-847`（Apps=TaskDialog：主指令大字+command link 按钮+验证勾选；其他 Tab=MessageBox：同一段 confirm 文本平铺，无主指令层级）。
- 现象：同为"确认操作"，迁移是现代 TaskDialog，垃圾清理/搜索/文件夹树是经典 MessageBox；同屏信息结构不同（TaskDialog 有 MainInstruction 分层，MessageBox 全是小字）。
- 建议：统一为 TaskDialog（代码库已有完整模板，OnExecute 的 Apps 分支直接推广到 else 分支；MainInstruction="将把 N 项移入隔离区，共 X"，Content=风险分组）。工作量 S。
- 证据：Confirmed。

**L-10 列头点击映射仍错位；"按时间排序"在 Apps Tab 与去重行仍是 no-op（X-14 部分残留）**
- 位置：[V] `Presenters.cpp:132-139`（OnColumnClick：col==3(大小)→按大小，**其余全按时间**——点"分类"列头按时间排序）+ `AppScanner.cpp:136-154`（ScanItem 未赋 createTime）+ `LargeFileScanner.cpp:255-266`（去重行无 createTime）。无排序方向箭头。
- 建议：col0→按 category、col1→按 riskLevel（Safe<Cautious<Advanced<InfoOnly）、col2→按 title；补 AppScanner/去重行 createTime=lastWrite；列头加 HDF_SORTUP/DOWN。
- 证据：Confirmed。

**L-11 详情列仍是"路径+\n+detailHint"塞单行单元格，且无 tooltip（X-11 部分残留）**
- 位置：[V] `JunkScanner.cpp:322-323、356-359`（detail = path + "\n" + detailHint + "\n(仅列出 N 天…)"）、`AppScanner.cpp:141-143`（"\n卸载命令: …"）、`Controls.cpp:21`（未设 LVS_EX_LABELTIP）、MainWindow.cpp 无 LVN_GETINFOTIP 处理。
- 现象：v2.2 加了双击说明面板（好），但详情列文本本身没改——320px 单行，\n 后的关键风险指引（"仅可通过 DISM 组件清理，不可直接删除"）依然不可读，悬停也无完整内容。
- 建议：详情列只放路径；hint 移入说明面板（已有）并加 LVN_GETINFOTIP 悬停显示完整 detail。
- 证据：Confirmed（文本构造）；渲染截断 [I]。

**L-12 DPI 半修复 + 新副作用：布局常量 ×1.5 而窗口默认 1100 物理像素不变 → 150% 下右侧按钮溢出；列宽/图标/状态栏分栏未缩放**
- 位置：[V] `Layout.cpp:12`（Scale）+ `MainWindow.cpp:89`（CreateWindow 1100,720 固定）+ `Controls.cpp:31-35`（列宽 180/84/330/100/320 常量）+ `Icons.cpp:21`（ImageList 16×16）+ `MainWindow.cpp:493`（SB_SETPARTS edge 340/660）+ WndProc 无 WM_DPICHANGED 分支。
- 现象（96dpi 基准推算到 144dpi，pad=12/btnW=210）：targetBtn x=932 宽 250 → 右缘 **1182**、btnSortSize 右缘 **1142**、btnSortTime x=1152 → 全部超出 1100px 窗口的客户区 ~1084。最大化时（宽 ~1904）不溢出。96dpi 下正常。列宽与 16px 图标在 150% 下相对变小（文字放大 1.5 倍、列宽不变 → 截断加剧，"⚠ 系统组件"必截）。
- 建议：CreateWindow 尺寸也过 Scale（1100→1650×1080）；列宽按 DPI 缩放一次（初始化时按 GetDpiForWindow 换算 AddCol 参数）；图标用 SHGSI_LARGEICON/SYSICONINDEX 按 DPI 取。
- 证据：Confirmed（数学）；视觉 Likely。

**L-13 扫描期全程 marquee：尺寸阶段已有 done/total 却不用**
- 位置：[V] `JunkScanner.cpp:291-293`（progress(d, dirs.size(), name)——总数已知）→ `SessionService.cpp:130`（回调签名丢弃前两个参数 `(unsigned long long, unsigned long long, const std::wstring& msg)` 只取 msg 拼状态栏）。
- 现象：30 秒垃圾扫描期间进度条 marquee + 状态栏滚动目录名；明明最重的"计算目录大小"阶段有 d/total 可驱动 determinate 百分比。
- 建议：RunScan 进度回调透传 done/total → Post WM_APP_SCAN_PROGRESS 带 wParam 百分比 → 扫描期也 determinate（大文件扫描 visited 无总数，可维持 marquee）。
- 证据：Confirmed。

**L-14 风险徽标的宽度与字体渲染风险**
- 位置：[V] `Controls.cpp:32`（风险列 84px）+ `Presenters.cpp:95-105`（徽标文案）。
- 现象：96dpi 下"⚠ 系统组件"（⚠~15px+空格+4 汉字）≈ 70px 勉强放下；150% dpi 列宽不变需 ~105px → 截断成"⚠ 系统…"。🛡（U+1F6E1）不在 Segoe UI 基本面内，依赖系统字体回退（Segoe UI Symbol/Emoji）——GDI 文本绘制下可能显示为黑白轮廓或方框 [I]；ℹ（U+2139）/⚠（U+26A0）/⊘（U+2298）在 Segoe UI 内。"—"（未分类）与"ℹ 谨慎"的区分靠内容长度，可接受但弱。
- 建议：风险列宽度 96→100 并随 DPI 缩放；🛡 可换 U+26E8 或直接用文字（"安全/谨慎/系统/提示"四字，颜色+文字双通道已满足高对比度要求）。
- 证据：宽度数学 Confirmed；emoji 渲染 [I]。

**L-15 右键菜单语境错位与虚假快捷键提示**
- 位置：[V] `MainWindow.cpp:1084-1104`（OnListContextMenu：固定菜单，所有非历史/树 Tab 相同）。
- 现象：① "打开所在位置\tEnter"——加速表（MainWindow.cpp:103-107）只有 F5/Ctrl+A；Enter 在列表上实际触发 LVN_ITEMACTIVATE：搜索 Tab=定位（恰好一致），**其他 Tab=说明面板**（与菜单提示不符）。② 搜索 Tab 下"永不清理此文件夹"——对象多为文件，文案错位；且 OnExcludeSelected（1061-1064）多选时只处理第一个选中行、无提示。
- 建议：菜单项按 Tab 生成：Enter 提示只在搜索 Tab 显示（或真正绑 Enter=打开位置）；"永不清理此文件夹"→"永不清理此项目"；多选排除时循环处理或提示"仅处理首个选中项"。
- 证据：Confirmed。

**L-16 搜索框无占位提示（cue banner）、无 Esc 清空**
- 位置：[V] `Controls.cpp:99-101`（editSearch 创建，无 EM_SETCUEBANNER；全代码无 WM_KEYDOWN/Esc 处理搜索框）。
- 建议：`SendMessage(h_.editSearch, EM_SETCUEBANNER, TRUE, (LPARAM)L"输入文件名关键词，多词为并且，支持 * ?")`——同时把 info 里那句语法说明解放出来；Esc 清空+重新聚焦。
- 证据：Confirmed。

**L-17 双击语义跨 Tab 不一致（定位 vs 说明），仅靠 info 一句话交代**
- 位置：[V] `MainWindow.cpp:157-165`（LVN_ITEMACTIVATE 按 Tab 分支）。
- 现象：同一列表控件，搜索 Tab 双击=Explorer 定位，其余 Tab=说明面板。用户跨 Tab 使用时需要重新校准肌肉记忆；说明面板本身是好功能，但两种"打开"语义混在一个手势上。
- 建议：统一双击=说明面板（信息需求优先），"打开位置"留给 Enter/右键/工具按钮；或搜索 Tab 双击定位但行内加"双击定位"提示列/提示条。至少在搜索 Tab 的空态文字里写明（当前 info 已有"双击定位文件"，可接受，列为打磨项）。
- 证据：Confirmed。

**L-18 About 落后一个版本：标题"v2.2"、使用说明没有"文件搜索"**
- 位置：[V] `MainWindow.cpp:936-949`（pszMainInstruction "…v2.2"；使用说明四条无文件搜索 Tab）。
- 建议：版本号从资源/常量读取；补"· 文件搜索：输入即搜全盘索引，双击定位，勾选可移入隔离区。"
- 证据：Confirmed。

### P3

**L-19 状态栏分栏固定 340/660 不随 DPI/窗口宽缩放**（[V] MainWindow.cpp:493-496；窄窗口退化 W/3 的逻辑保留了）；进度文本长标题（"执行 3/19: 组件存储 (WinSxS)"）在第三栏截断。建议 edge 按 W 比例（0.32/0.62）。

**L-20 键盘可达缺口**（[V] Controls.cpp）：advancedChk（:55-57）、editFileType（:85）、editDrives（:91）无 WS_TABSTOP——高级模式与两个过滤框键盘不可达；Tab 序按创建顺序在当前显示集内基本合理（GetNextDlgTabItem 跳过隐藏项）。

**L-21 小文案**（[V]）：① dangerousNames 硬截断 80 字符（MainWindow.cpp:770）无"等 N 项"后缀；② "撤销选中(历史)"半角括号（Controls.cpp:43）vs 全站全角"（N 项 · X）"；③ TaskDialog 取消按钮文案"取消（默认）"（MainWindow.cpp:827）——"（默认）"是给评审看的不是给用户看的；④ 状态栏三格文案前导空格手拼（" 系统盘…"），建议统一。

**L-22 扫描中的视觉状态**：① 扫描按钮变"取消"但仍配 SIID_FIND 放大镜图标（[V] MainWindow.cpp:366——图标配"取消"语义错位，可换 SIID_STOP 或去图标）；② 扫描中 EnableWindow(list,FALSE) 整表灰化（[V] :648）视觉突兀，可改为仅覆盖一层"扫描中"横幅 [I]；③ 进度条显示时悬浮在列表右下角（W-224 区域）盖住详情列尾行（[V] Layout.cpp:99-101 坐标；遮挡属 [I]）。

**L-23 FormatSize 1024 进制标 KB/MB/GB 与资源管理器十进制不一致（X-21 残留）**（[V] StringUtils.cpp:26-36）。用户与 Explorer 核对释放空间时数字对不上。建议改十进制或标 KiB/MiB。

**L-24 索引量级异常无护栏文案**：`UpdateSearchStatus`（[V] MainWindow.cpp:549-568）把 EntryCount 直接展示："索引就绪：27 项。输入关键词即可开始搜索。"——本机实测索引仅 27 项（日志 15:55:38），正常 C 盘为百万级；用户搜"windows"得 0 匹配时显示"匹配 0 项（索引共 27 项…）"，第一反应必然是"搜索坏了"。根因（索引为何只有 27 项）不在本次范围，但 UI 应防御：EntryCount < 阈值（如 10000）时提示"⚠ 索引条目异常少（N 项），搜索结果可能不完整，建议重建索引"。
- 证据：展示机制 Confirmed；27 项为日志事实（异常观察，非结论）。

**L-25 搜索 Tab 的"扫描"按钮语义漂移**：索引已就绪时点"扫描"/F5 → BuildScanner 返回 nullptr → BuildIndexAsync → EnsureBuilt 立即返回（cheap）→ 走完一圈什么都没变（[V] MainWindow.cpp:534-541、SessionService.cpp:104-124）。按钮叫"扫描"实际是"重建索引"，无提示。建议该 Tab 下按钮文字改"重建索引"。

---

## 3. 上轮 X 项修复核查表（X 编号 | 声称修复 | 实际状态）

| X | 上轮问题 | 声称（实施状态表） | 实际状态（本次逐条读码） |
|---|---|---|---|
| X-1 | 确认框无清单/详情无 tooltip | 风险分组摘要 | **部分修复**：分组摘要+危险项点名（≤80 字符）有了；确认框仍无完整路径清单；列表仍无 LVN_GETINFOTIP/LABELTIP；详情列 \n 依旧（L-11）。双击说明面板补足了"单项目审计"路径 |
| X-2 | 风险四级零呈现 | 风险列+徽标+染色 | **已修复**（Controls.cpp:32、Presenters.cpp:95-105、MainWindow.cpp:166-189），无回归；84px 宽度与 🛡 渲染存疑（L-14 [I]） |
| X-3 | InfoOnly 可勾选确认后被拒 | 自动排除 | **部分修复**：OnExecute 前置排除+确认框"⊘ 仅提示 N 项 — 已自动排除"+按钮计数排除（MainWindow.cpp:687/738-746/794）；复选框本身仍可勾、勾选时无行内即时反馈（反馈延迟到点执行） |
| X-4 | 错误码天书 | 人话+RM 诊断+重试 | **已修复**（QuarantineOp.cpp:43-55 "文件正被 X 占用，关闭这些程序后重试"、Win32ErrorText） |
| X-5 | 确认默认 OK | 默认取消 | **已修复**（MB_DEFBUTTON2 ×2 处；TaskDialog nDefaultButton=IDCANCEL）Confirmed |
| X-6 | 勾选丢失 | 按 path 恢复 | **部分修复/新缺陷**：显式勾选保留；**显式取消勾选不保留**（L-1，P0） |
| X-7 | 确认文案张冠李戴 | 按操作分支 | **已修复**（Apps 分支"迁移后原位置以 Junction…"，MainWindow.cpp:804-806） |
| X-8 | 进度条不可见/无取消 | 坐标+取消+determinate | **已修复**（y=H-statusH-prgH-4；扫描按钮变取消；执行期 WM_APP_OP_PROGRESS 驱动百分比）；残留：扫描期全程 marquee（L-13） |
| X-9 | 撤销仅单条 | 批量异步 | **已修复**（UndoSelected 多选 + UndoRecordsAsync 后台 + 预检分类提示，Presenters.cpp:570-604） |
| X-10 | 静默降级 | 信息栏告知 | **打折**：degrade 文本确实生成并追加（MainWindow.cpp:594-610），但追加在 info 尾部而 info 固定 36px 高 → 大概率不可见（L-3，P1） |
| X-11 | detailHint 不可读 | 说明面板 | **部分修复**：双击说明面板含完整信息；详情列文本构造未改、无 tooltip（L-11） |
| X-12 | 无键盘可达 | Tab/F5/Ctrl+A | **基本修复**（加速表+GetNextDlgTabItem+WS_TABSTOP）；缺口：advancedChk/editFileType/editDrives 无 tabstop（L-20）、Enter 提示虚假（L-15） |
| X-13 | 术语混杂 | 术语中文化 | **大部分未修**：分类列全英文、FolderTree 菜单仍"删除文件夹…"（L-8，P1）；已修：exec 去掉 SIID_DELETE、[保留]/[待删]、回收站 title、AppScanner detail、MoveJunctionOp 错误串、扫描进度 |
| X-14 | 时间排序无效/列头映射 | 补 createTime | **部分修复**：Junk/Search 已赋值；Apps Tab 与去重行仍无 createTime（排序 no-op）；列头映射仍"非大小列全按时间"；无方向箭头（L-10） |
| X-15 | 先确认后校验目标盘 | 前置校验 | **已修复**（MainWindow.cpp:750-756 确认前校验） |
| X-16 | advancedChk 溢出/列表压状态栏 | —（未列入声称） | **未修复**：两处坐标公式原样（L-4，P1）——Layout.cpp:46 与 :92 |
| X-17 | 输入容错/死按钮 | 禁用矩阵 | **已修复**（SetTaskBusy 统一矩阵）；残留：磁盘过滤"CD"静默取首字符（Presenters.cpp:213-226） |
| X-18 | DPI 固定像素 | 布局常量缩放 | **部分修复+新副作用**：Layout 常量已 Scale；列宽/图标/状态栏分栏未缩放；150% 默认窗口右侧按钮溢出（L-12）；无 WM_DPICHANGED |
| X-19 | 空态引导 | — | **未变**：空列表区仍无引导（说明文字已有两行） |
| X-20 | 历史页语义列 | 列 0 人话 | **部分修复**：列 0 内容人话化 ✓；但列头/列宽共享导致整页错位（L-2，P0） |
| X-21 | 容量单位 | — | **未修**（1024 进制标 KB/MB，L-23） |
| X-22 | 状态栏分栏挤占 | 隔离区常驻 | **已修复**（迁移目标仅 Apps Tab 显示，MainWindow.cpp:509-513）；edge 340/660 固定像素残留（L-19） |
| X-23 | 符号双通道 | 徽标+文字 | **已修复**（"🛡 安全"等符号+文字双通道）；emoji 渲染 [I]（L-14） |
| X-24 | 列表无右键菜单 | 已加 | **已修复**（OnListContextMenu 7 项）；语境适配残留（L-15） |

### 已验证已修复（做得好的，v2.3 无回归）
X-2 / X-4 / X-5 / X-7 / X-8(主体) / X-9 / X-15 / X-17 / X-22(主体) / X-23 / X-24 —— 均逐行读到实现，机制确凿。

### 被推翻的假设 / 打折的"已修复"声称（复核纪律要求单列）
1. **"降级可见化已修复"**——文本确实生成，但被 36px info 标签裁掉，实际大概率不可见（L-3）。
2. **"术语中文化已修复"**——分类列（列表第一列）全英文 + FolderTree 菜单"删除文件夹…"未改（L-8）。
3. **"勾选保持已修复"**——只保持"勾"，不保持"取消勾"；后者会静默恢复 recommended（L-1）。
4. **"DPI 已缩放"**——只缩放了 Layout 常量；与固定窗口尺寸组合在 150% 下产生按钮溢出这一新问题（L-12）。
5. 我方自查推翻两例：① 曾疑"搜索 Tab 可把系统文件移入隔离区是安全洞"——读 PlanBuilder.cpp/RunPlan 确认搜索项走 QuarantineOp + GuardRails FileOp，系统路径被拦，问题仅在文案层（L-7），非安全洞；② 曾疑"扫描中 editSearch 可输入会与 worker 竞争 results_"——BuildIndexAsync 不写 results_[Search]，MutableResults 仅 UI 线程调用，无此竞态。

---

## 4. 十个最值得先改的布局/交互改进（小改大效）

| # | 改动 | 位置 | 工作量 | 对应 |
|---|---|---|---|---|
| 1 | RenderItems 双向勾选快照（wasChecked + wasUnchecked） | Presenters.cpp:53-89 | S（1h） | L-1/P0 |
| 2 | 切 Tab 时 LVM_SETCOLUMN 动态改历史页列头/列宽 | Presenters/Controls | S-M（半天） | L-2/P0 |
| 3 | info 高度自适应（或提到 52px 且降级告知前置） | Layout.cpp:89 + MainWindow.cpp:594 | S | L-3 |
| 4 | advancedChk 右锚定 + 列表高减 statusH | Layout.cpp:46/92 | S（30min） | L-4（X-16） |
| 5 | UpdateExecButton 增量计数 + 文本去重 | MainWindow.cpp:151/669 | S-M | L-6 |
| 6 | 索引构建不锁 Tab + 取消文案区分 | MainWindow.cpp:448 + SessionService.cpp:118 | M | L-5 |
| 7 | category 中文映射表（显示层） | Presenters.cpp RenderItems | S-M | L-8 |
| 8 | FolderTree 菜单"移入隔离区…" + 搜索 Tab 确认话术分支 | Presenters.cpp:471 + MainWindow.cpp:787 | S | L-8/L-7 |
| 9 | EM_SETCUEBANNER 搜索占位 + 统一确认框为 TaskDialog | Controls.cpp + MainWindow.cpp:839 | S-M | L-16/L-9 |
| 10 | CreateWindow 尺寸与列宽过 Scale；扫描期 determinate | MainWindow.cpp:89 / Controls.cpp / SessionService.cpp:130 | M | L-12/L-13 |

（1-5 合计约一个工作日，直接消掉两个 P0 与三个 P1 主症。）

---

## 5. 理想布局蓝图

### 5-A 六 Tab 信息架构（含"搜索 vs 大文件"论证）

现状顺序：垃圾清理 → **文件搜索** → 大文件/去重 → 应用迁移 → 文件夹分析 → 操作历史。

- **短期（不动代码顺序）**：顺序合理，保留。"垃圾清理"是主打心智放第 1；"文件搜索"作为高频横向工具放第 2 符合 Everything 心智；与大文件相邻（2、3）天然形成"查找区"（搜索=按名字找，大文件=按大小找），应用迁移/文件夹分析="搬家区"，操作历史=账本。需要改的是**命名与区分度**：两个 Tab 的工具行长得几乎一样（搜索框行 vs 三控件过滤行），建议大文件 Tab 的设置行前加一节标签"过滤条件"，搜索 Tab 保留极简（框+复选）。
- **中期（值得论证的合并）**：大文件 Tab 的"最小大小/类型/磁盘"三个过滤器本质是结构化搜索。一旦索引带上尺寸（当前 ADR-004 不带，需 FastWalk 补），大文件/去重可降级为搜索 Tab 的两个预设（"大于 100MB"“重复文件"过滤 chips），Tab 数 6→5，认知负担与代码双降。**在索引含尺寸之前不建议合并**——大文件 Tab 依赖 FastWalk 全量遍历，与 USN 索引搜索是两条数据通路，强行合并会把 77 秒的后台扫描塞进"即时搜索"的预期里，违背 Everything 心智。
- 操作历史建议更名"历史与隔离区"——该页承载撤销+清空+隔离区状态三件事，现状名称只覆盖三分之一。

### 5-B 统一反馈层级规范（现状偏差 → 目标）

| 通道 | 应承载 | 现状偏差 |
|---|---|---|
| info 标签 | 单行动态反馈（结果/计数/引导）+ ≤2 行静态说明 | 5 种用途互相覆盖 + 36px 裁剪（L-3） |
| 状态栏 | 持续状态（磁盘/隔离区）+ 当前任务一行 | 基本达标；分栏像素固定（L-19） |
| 进度条 | 任务"活着"的证据；有总数必 determinate | 扫描期全程 marquee（L-13） |
| 模态弹窗 | 仅破坏性确认（含分组+默认取消）与失败决策 | 方向对；两套确认框并存（L-9） |

规范一句话：**info 一件事一句话、状态栏一状态一格、进度条有数必百分比、弹窗只问"确定吗"。**

---

## 6. 高频任务路径成本表（v2.3 现状）

| 任务 | 步骤 | 成本 | 残留痛点 |
|---|---|---|---|
| 垃圾清理（默认项） | 扫描（1 击+30s，可取消✓）→ 勾选(0，默认)→ 执行(1)→ 确认(1，默认取消✓)→ 完成（info 一行） | 3 击 | 确认框无完整清单；dashboard/降级文字可能被裁（L-3） |
| 垃圾清理（排除某默认项） | 上述 + 取消勾选(1) + **排序前必须重新核对全部勾选** | 3 击 + 核对成本 | 取消勾选在排序/切 Tab 后丢失（L-1，P0） |
| 挑选大文件 | 切 Tab(1)→ 过滤(0-3 控件)→ 扫描(1+77s)→ 排序(1，勾选保持✓)→ 勾 N→ 执行(1)→ 确认(1) | 6+N 击 | 去重行时间排序无效（L-10）；Ctrl+A 大列表可能卡（L-6） |
| 首次搜索→定位 | 切 Tab(1)→ **等索引 31s（全 UI 锁定）**→ 输入→ 双击(1) | 2 击+输入+31s | L-5；索引 27 项导致多数查询 0 结果（L-24） |
| 再次搜索→定位 | 切 Tab(1)→ 输入（200ms 防抖即时）→ 双击(1) | 2 击+输入 | 达标（Everything 预期） |
| 搜索→隔离 | 输入→ 勾 N→ 执行(1)→ 确认(1) | 2+N 击 | 确认话术"缓存…重新下载"语义错配（L-7）；风险列"—"无参考 |
| 撤销一次隔离 | 历史 Tab(1)→ 找记录（**列错位，时间戳/路径截断**）→ 选中(1)→ 撤销(1) | 3 击 | L-2（P0）；批量多选撤销已可用✓ |
| 释放空间（真清理） | 隔离流程 + 历史 Tab(1)→ 清空(1)→ 确认(1) | 跨 2 Tab 共 6 击 | 状态栏隔离区占用常驻✓ |

---

## 7. 建议人工复核清单（[I] 项，约 5 分钟；在现有运行实例上操作，勿新启进程）

1. **(30s)** 默认 1100px 窗口切"应用迁移"：高级复选框"(默认 Junction)"是否被右缘裁剪（L-4）。
2. **(30s)** 垃圾清理扫描完成：info 第 3 行起（"ℹ 外置规则…/快速索引未启用…"）是否可见（L-3，预期不可见）。
3. **(20s)** 垃圾清理 Tab 取消勾选任一默认项→点"按大小排序"→该项是否被重新勾上（L-1，预期会）。
4. **(45s)** 首次进入"文件搜索"Tab：Tab/按钮锁定时长；期间点"取消"后状态栏文字是否为"此磁盘不支持…"（L-5）。
5. **(30s)** 搜索任意词→Ctrl+A 全选：UI 是否卡顿 1 秒以上（L-6）。
6. **(20s)** 列表风险徽标：🛡/⚠/ℹ/⊘ 是否显示为方框或黑白轮廓（L-14）。
7. **(30s)** 操作历史页：第 2 列时间戳、第 4 列路径是否截断，列头是否与内容对不上（L-2）。
8. **(30s)** 150% 显示缩放重启（或改 DPI）：右侧按钮（排序/目标盘）是否超出窗口（L-12）。
9. **(15s)** 任一确认弹窗直接按 Enter：是否落在"取消"（X-5 活体确认）。
10. **(20s)** 列表滚动到底：最后一行是否被状态栏盖住下缘（L-4b）。

---

## 8. 未验证 / 局限

1. **无 GUI 活体**：全程以代码+文案+坐标数学为证据。所有视觉表现类结论（裁剪、截断、遮挡、emoji 渲染、卡顿时长、锁定期体验）标 Likely/[I]，待 §7 清单复核。MiniSys.exe 正在用户桌面运行，本评审未触碰任何进程。
2. 用户数据仅只读读取了 minisys.log；history.jsonl 未再核对（上轮已核，本次无新操作）。
3. 索引仅 27 项的根因（USN 枚举为何只得到 27 条）按指令未查——本报告只确认了 UI 会把这个数字直接展示（L-24），并给出防御性文案建议。
4. 14:45 会话日志中 "Scan tab=1 done: 242 items"（tab=1 在 v2.3 是文件搜索，但 SearchPresenter 不产生 Scanner 扫描）——该会话可能运行的是 v2.2 二进制（tab=1=大文件），未深究，不影响 UI 结论。
5. CCleaner/Everything 对标基于产品常识（Speculative），未在本机实测。
6. 未测试屏幕阅读器；高对比度模式下徽标"符号+文字"双通道理论上达标但未活体验证。
