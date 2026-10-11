# MiniSys 布局机制评审(第五轮 · 纯布局)

> 日期:2026-10-11 · 对象:HEAD `be73902`(v2.15 build 52,正在运行的活体实例)
> 基线:docs/review-2026-10-11/07-ux.md(第四轮,X-26..X-31 已修)、docs/REVIEW_2026-10-11.md §5、docs/REVIEW_2026-10-06-3.md 附录 07
> 方法:14 张 v2.15 活体截图(.tmp-shots\shot-01..14.png,-tour 巡页,双视觉模型交叉判读)+ 代码核对(MainWindow.cpp / Presenters.cpp/h / Controls.cpp / Layout.cpp 全读相关段)。**仓库只读;未启动/结束 MiniSys.exe;未触碰 build.bat 与 tools\\*。**
> 范围:几何/裁剪/对齐/一致性机制;配色字体口味归并行美学同事,未越界。

---

## 0. 一页结论

本轮唯一的重大发现是一个 **P0 级渲染断裂**:v2.15 虚拟列表(`df283d1`)的提交说明声称行文本"通过 LVN_GETDISPINFO 路由到活动 presenter"、勾选"走 NM_CLICK state-icon 路径",但**全仓 grep `GETDISPINFO` 只命中 Presenters.h:49 的一条注释**——MainWindow::WndProc 的 WM_NOTIFY(MainWindow.cpp:238-350)没有任何 LVN_GETDISPINFO / NM_CLICK 分支,`SupplyDispInfo` / `OnCheckFlipped` 两个函数零调用点。发送端(cells_ 预渲染、LVM_SETITEMCOUNT、state map)全部写好,接收端忘了接线。

14 张活体截图与该结论完全自洽:**五个 ListView 页(垃圾/搜索/大文件/迁移/历史)行内容全部空白**,而计数与滚动条照常——历史页状态栏"隔离区: 50 项"但列表 0 行可见(shot-01,滚动条半高滑块贴顶=按 50 行计算);大文件页"执行(11 项 · 1.31 GB)"按钮有计数而列表 0 行(shot-03,计数来自 checkStateByPath_ map,不走控件);搜索页 0 匹配无滚动条(shot-02,与 N=0 一致);**TreeView(非虚拟)25 行正常渲染(shot-04)**,反证不是截图/重绘时机问题。执行/勾选计数、切页列头切换均正常,唯独行内容不出现。

第四轮报告 §8.2 已预警"虚拟列表的勾选路径未活体验证"——实际断裂比预警深:不止勾选,整条行渲染链都没接。上轮 07 报告 X-7 核对"虚拟列表未做"时未发现接线缺失,属本轮新证据推翻旧结论的"完成度"判断。

其余:上轮 X-26..X-31 六项修复**全部核实存活**(含 X-27b 面板项名已改为与提示逐字一致);X-18/23/25/35/37 及空态占位确认残留(均 P3);信息区自适应高度、状态栏、按钮行几何、树页布局**活体确认无问题**。

---

## 1. 问题清单(L-1 起,按严重度)

### P0

**L-1 虚拟列表"幻接线":LVN_GETDISPINFO 无接收端,五个 ListView 页行内容(文本+复选框)全部不渲染**
- 代码证据:[V] `MainWindow.cpp:238-350` WM_NOTIFY 完整分支表(TCN_SELCHANGE / LVN_ITEMCHANGED / LVN_ITEMACTIVATE / NM_CUSTOMDRAW / LVN_COLUMNCLICK / NM_RCLICK×2 / NM_DBLCLK)——**无 LVN_GETDISPINFO、无 NM_CLICK**;grep 全 src `GETDISPINFO` 仅 `Presenters.h:49` 注释;`SupplyDispInfo`(Presenters.cpp:182 实现、:849 历史实现)与 `OnCheckFlipped`(:205)零调用点。
- 提交说明与代码脱节:[V] `git show df283d1`:"cell text … served via LVN_GETDISPINFO **routed to the ACTIVE presenter**"、"toggles flow through the **NM_CLICK state-icon path**"——两者均不存在。
- 截图证据(双模型交叉一致):
  - [V] shot-01(历史页):状态栏"隔离区: 50 项 / 2.13 GB",列表区 495px 全空白;**垂直滚动条存在、滑块约占半高贴顶**——与 `LVM_SETITEMCOUNT(50)` 生效(滚动范围 50 行、视口约 24 行)而行文本为空完全吻合;
  - [V] shot-03(大文件/去重页):按钮"执行(11 项 · 1.31 GB)"有计数(计数读 `checkStateByPath_` map,不经过控件),列表 0 行可见 + 滚动条贴顶(11 行 < 视口,滑块满轨);
  - [V] shot-02(搜索页):"匹配: 0 项",列表空且**无**滚动条——N=0 时无滚动条,反证上面两条的滚动条来自 N>0 的计数;
  - [V] shot-05/07/08/10/12(遮挡残条):垃圾清理/应用迁移/历史各页列头("分类|风险…")之下全部空白;
  - [V] shot-04 / shot-11(文件夹分析页,TreeView **非虚拟**):根节点 C:\ + 24 子文件夹、行高一致 24px、滚动条正常——同为切页 4 秒时点,树有内容、列表没有,排除截图时机因素。
- 影响面:核心 UI 不可用——行文本、复选框(state image 同由 DISPINFO 供给,Presenters.cpp:193-201)、风险 emoji 列、选中态/焦点视觉、LABELTIP 全部无从谈起;连锁失效:鼠标勾选(L-2)、NM_CUSTOMDRAW 风险染色与双击说明面板(L-3,ItemAtRow 取 lParam)。Space 键路径(`MainWindow.cpp:167-179` → ToggleCheckAt)写 map 后 ListView_RedrawItems 也无内容可画。
- 修法(约 15 行):WM_NOTIFY 增补
  `else if (nm->hwndFrom==h_.list && nm->code==LVN_GETDISPINFO){ auto* di=reinterpret_cast<LPNMLVDISPINFOW>(lp); if(auto* p=ActivePresenter()) p->SupplyDispInfo(di); }`
  修复后必须回归:行高一致性、复选框渲染、勾选(Ctrl+A/右键全选 SetAllChecked 的整表重绘)、选中态、LABELTIP、滚动中段行(快速滚动时 DISPINFO 高频回调)、10 万行压力。
- 备注:这是"布局机制"范畴内的渲染管线断裂,不是美学;上机第一件事即验证。

### P1

**L-2 鼠标勾选路径 NM_CLICK 同样未实现(提交说明声称存在的路径不存在)**
- [V] grep `NM_CLICK` 全 src 仅 `MainWindow.cpp:256` 注释("the authoritative map is written by the NM_CLICK / Space paths below"——"below" 里并没有 NM_CLICK)。
- 后果:即使补上 L-1,虚拟列表 + LVS_EX_CHECKBOXES 下点击复选框不会写 `checkStateByPath_`(comctl 对 owner-data 的 state 点击不自动改写,需宿主在 NM_CLICK 的 LVHT_ONITEMSTATEICON 中翻转);勾选是"扫描→勾选→执行"主链的第一步,鼠标用户整链不通。Space 键路径存在但键盘新人无从发现行(行都看不见)。
- 修法:WM_NOTIFY 增补 NM_CLICK 分支:ListView_SubItemHitTest,`flags & LVHT_ONITEMSTATEICON` → `lp->ToggleCheckAt(iItem)` + UpdateExecButton。S。

### P2

**L-3 ItemAtRow 依赖 lParam,owner-data 下失效 → NM_CUSTOMDRAW 风险染色、双击说明、IDM_LIST_INFO 取错行或全失效**
- [V] `Presenters.cpp:126-132`:`ListView_GetItem(LVIF_PARAM)` 取 `lvi.lParam` 当 snapshot 索引;v2.15 `RenderItems()`(:134-171)不再 per-row insert,**lParam 无人设置**(owner-data 下 LVM_GETITEM 的 lParam 返回 0/未定义)。
- 后果:①NM_CUSTOMDRAW(MainWindow.cpp:286-288)对每行 `ItemAtRow(dwItemSpec)` → 恒指向 snapshot_[0] 或 nullptr → 高对比度外的风险行染色(X-14 的正常态)全失效/错行;②`OnItemActivated(row)`、IDM_LIST_INFO(MainWindow.cpp:410-414)同病。
- 修法:虚拟模式下显示行号即快照索引(snapshot_ 已按当前排序排列),`ItemAtRow(row)` 直接 `row < snapshot_.size() ? &snapshot_[row] : nullptr`,删掉 ListView_GetItem。S(3 行)。

### P3

**L-4 X-37 残留(活体确认):恢复的非空查询 + 空结果仍报"匹配 0 项",不区分"未搜/缓存空"**
- [V] `MainWindow.cpp:1329-1332` 非空查询统一输出"匹配 N 项(索引共…最多显示前 1,000)";[V] shot-02:会话恢复查询 `pdb folder\debug`、索引 3,030,729 项、显示"匹配: 0 项"——开发机上 pdb 不可能 0 命中,该状态实为"缓存结果缺失/被消毒且未自动重搜",提示误导(用户以为真的无结果)。改法照上轮:snapshot 空+查询非空 → "已恢复搜索词,按 Enter 重新搜索"。

**L-5 上轮 P3 池残留确认(截图/代码双重)**
- X-18 历史列头排序:仍未修——HistoryPresenter 未覆写 OnColumnClick(Presenters.h:204-219 无、cpp:822-930 无),基类空实现;[V] shot-01 历史页五个列头均无排序箭头,与代码一致(可点无响应)。
- X-23 进度条 marquee↔determinate 切换 SETPOS 0 归零跳变:[V] MainWindow.cpp:438-450 原样。
- X-25 按钮行 `+20/+40` 字面量未过 UiScale:[V] Layout.cpp:55-59 原样(96dpi 无碍,≥150% 时 exec/undo/emptyQ/targetBtn 相对间距偏窄)。
- X-35 历史时间列宽 150:[V] Presenters.cpp:829 仍 150;19 字符时间戳("2026-10-07 12:44:05")在 96dpi Segoe UI 下约 140px+列内边距,临界截断;本轮被 L-1 遮蔽无法拍板,列宽建议 170 维持。
- 空态占位(V-R9,六页全缺):[V] shot-01/02/03 三个空列表均无任何占位提示(纯白);L-1 修复后此问题将原形毕露(搜索 0 匹配、未扫描页)。
- X-16 信息项:标题栏仍 "v2.10 build 52"(shot-01..04 一致),次版本与提交叙事 v2.15 脱节,用户无法对应变更日志。

**L-6 信息区行高换算常数与实际字体行高不一致(轻微,方向安全)**
- [V] Layout.cpp:135-139:`lineH = Scale(15)` 硬编码,而 `rc.bottom` 来自 DT_CALCRECT 实际测量(100% DPI 下 Segoe UI 换行行高约 16-17px)→ `lines` 向上取整偏大:2 行文本分得 3 行高度。[V] shot-01(历史页 2 行说明 + 约 1 行留白)与之吻合。宁多勿裁,无功能损害;打磨项:用 `rc.bottom / 实际行高` 或给 DT_CALCRECT 结果直接加 padding。

---

## 2. 已修项核对表(上轮 X-26..X-31 + 关联项)

| # | 上轮项 | 本轮核对 | 证据(HEAD be73902 / build 52) |
|---|---|---|---|
| X-26 | RootsDlg `flush;` 漏括号(多行根丢失+清配置+假成功) | ✅ 已修 | [V] MainWindow.cpp:904/907/1038/1041 四处均 `flush()` |
| X-27a | 历史页未签名仍标"🛡 可还原" | ✅ 已修 | [V] Presenters.cpp:881-884 `HistoryRiskLabel` 对 `!VerifySignature(r)` 返回 **"未签名"**(纯文字,无 ⧗/🛡 前缀——诚实且规避 emoji 渲染风险;观感被 L-1 遮蔽,待活体) |
| X-27b | 拒绝提示与面板项名不一致(照抄过滤=0 结果) | ✅ 已修 | [V] 面板项名已改为 `L"信任并签名旧记录…"`(MainWindow.cpp:816),提示引文"信任并签名旧记录"(Presenters.cpp:918)为其前缀,子串过滤可命中 |
| X-27c | Ctrl+K 全 UI 不可发现 | ✅ 已修 | [V] About 使用说明含"Ctrl+K 命令面板:所有操作零层级直达"(MainWindow.cpp:1816) |
| X-28 | info 高度自适应不随文本重排 | ✅ 已修且活体确认 | [V] OnTabChanged:1217 / OnScanDone 尾:1414(`// X-28`) / ShowHint:1551 均调 OnSize;截图 2/3/4 行内容全部完整可见无裁剪(shot-01/03/04/05) |
| X-29 | 空闲倒计时刷 info + 取消无反馈 | ✅ 已修 | [V] 倒计时全程走 pane2 `SetProgressText`(MainWindow.cpp:541-547/1106);取消分支输出"已取消空闲刷新"(:533);info 不再被逐秒覆盖 |
| X-30 | 树 WS_TABSTOP / WM_CONTEXTMENU | ◐ 半修 | [V] 树已加 WS_TABSTOP(Controls.cpp:139);**WM_CONTEXTMENU 仍 0 命中**,Shift+F10/Menu 键仍无菜单 |
| X-31 | 最小宽 980 → Apps 页重叠 | ✅ 已修(代码层) | [V] MainWindow.cpp:234 `UiScale(hwnd_, 1080)`;96dpi 数学复核:targetBtn 右缘 8+4×148+40+180=820 < advancedChk 左缘 1080-8-240=832,余 12px。截图无最小宽 Apps 帧(shot-07/14 被 RGBBox 遮挡且非最小窗),活体待补;12px 余量偏紧,改 DPI/字重后建议再验 |
| X-33 | Apps 页修改时间列全"—" | ✅ 已修 | [V] AppScanner.cpp:153 `it.createTime = it.lastWriteFiletime`(观感待 L-1 修复后验证) |
| — | v2.15 虚拟列表本体 | ❌ 接线缺失 | 见 L-1/L-2/L-3;渲染提速的发送端(cells_/SETITEMCOUNT/map)实现完整 |

顺手核实:`kScanCols` 第 6 列"修改时间"由 SetListColumns 动态补插(v2.13c 修复仍在,Presenters.cpp:238-241 注释),列宽 110 @96dpi 容 "MM-dd HH:mm" 充足、右对齐(:251-253)✓;列数变化时宽度不保持属上轮已知可接受行为,未变。

## 3. 已验证无问题 / 被推翻

| 项 | 结论 | 证据 |
|---|---|---|
| 文件夹分析页(TreeView,非虚拟)整体布局 | ✅ 无问题:25 行正常渲染、行高一致(~24px)、无截断、滚动条滑块位置正常、单列树右缘留白属正常 | [V] shot-04(完整帧)、shot-11(残条 24 行同样正常) |
| 信息区自适应高度机制 | ✅ 工作正常:2 行(shot-01)/3 行(shot-03/04)/4 行(shot-05)均完整显示、无垂直裁剪;留白方向宁多勿裁(常数偏差见 L-6) | [V] Layout.cpp:122-141 + 四帧截图 |
| 状态栏 | ✅ 四格结构完整(SB_SETPARTS 4,MainWindow.cpp:1230-1231;第 4 格为进度条保留空格):磁盘/隔离区/"就绪"文字全部完整无截断;X-20 的 grip 让位逻辑未变 | [V] shot-01/02/03;注:视觉模型只见 3 格,因第 4 格恒空文本,属设计 |
| 按钮行几何(96dpi 数学) | ✅ 无重叠:scan/exec/undo/emptyQ/open/targetBtn/btnSortSize/btnSortTime 左锚排布 + advancedChk 右锚;历史页 undo/emptyQ 起点x=324/492 与 shot-01 实测吻合 | [V] Layout.cpp:53-67 + shot-01/03 |
| 列头右侧空白区 | ✅ 非缺陷:6 列宽和 1010(96dpi)< 列表宽 ~1048,尾列后 ~40-110px 空白是报表列表列宽和小于视口的正常表现(shot-01/02/03 观察到的"空白列区") | [V] 代码+截图 |
| 上轮 4.3 假设 4(截图可当当前证据) | 本轮延续成立:14 张摄于同一 build 52 会话,shot-05 起 RGBBox 常驻遮挡系用户并行开启的窗口,非 MiniSys 缺陷 | [V] |
| 被推翻:第四轮 §8.1c"滚动/勾选/排序应明显跟手(虚拟模式)"的活体验证预期 | 实际:行渲染整链未接线(L-1),该验证若执行必然失败;第四轮 §0"渲染 P0 已除"的结论在**用户可见层面**不成立(70ms 的测量是 LVM_SETITEMCOUNT 成本,不含可见文本) | [V] 本轮 L-1 证据链 |

## 4. 截图台账(.tmp-shots\,14 张全查)

| 张 | 页签 | 状态 | 关键观察 |
|---|---|---|---|
| 01 | 操作历史(一轮) | ✅ 可用 | 列表 0 行+半高滑块滚动条 vs 状态栏"隔离区 50 项"→L-1;列头 5 列无箭头(X-18);info 2 行留白 1 行(L-6);undo/emptyQ 位置与 Layout 数学吻合 |
| 02 | 文件搜索(一轮) | ✅ 可用 | "匹配 0 项(索引共 3,030,729)"恢复态误导(L-4);空列表无滚动条(与 N=0 自洽,反证 01/03);第 6 列"修改时间"列头在;参数行/按钮行对齐正常 |
| 03 | 大文件/去重(一轮) | ✅ 可用 | "执行(11 项 · 1.31 GB)"计数在而列表 0 行→L-1(map 计数不经控件);info 3 行完整;滚动条贴顶 |
| 04 | 文件夹分析(一轮) | ✅ 可用 | TreeView 25 行正常(反证 L-1 非截图时机);行高一致;⚠ 单色琥珀字形渲染正常(非 tofu) |
| 05-14 | 垃圾清理/搜索×3/应用迁移×2/历史/大文件/文件夹 | ❌ 遮挡弃用(RGBBox 前景窗覆盖 ~75-80%,左缘 x≈222 恒定) | 残条旁证:垃圾页 info 4 行满(L-6 正常)、应用迁移页 info 3 行、历史页列表空(08)、树正常(11);两轮六页覆盖达成,但有效完整帧仅 4 张 |

emoji+VS16 专项:列表风险列被 L-1 遮蔽,**无法判定彩色/方框**;唯一旁证是 info 区 ⚠(不带 VS16)为单色琥珀字形、正常渲染(shot-01/04/05/07)。RiskBadge 文本(🛡️/⚠️/ℹ️ 带 U+FE0F,Presenters.cpp:358-373)的实际渲染列为 L-1 修复后第一优先验证项。

## 5. 10 分钟人工复核清单(修复 L-1 后执行)

1. **接线回归(2 min)**:任意扫描页出结果 → 行文本/复选框可见;鼠标点复选框与 Space 各勾一行 → "执行(N 项)"计数变化;Ctrl+A 全勾;快速滚动中段无空白行(滚动时 DISPINFO 按需拉取)。
2. **历史页(2 min)**:50 条旧记录第 5 列应全为"未签名"(X-27a 活体);"时间"列 19 字符时间戳是否截断(X-35 定论);列头点击有无排序(X-18)。
3. **风险 emoji(1 min)**:垃圾页风险列 🛡️/⚠️/ℹ️ 是彩色 emoji、单色字形还是方框(本轮唯一无法拍板的专项)。
4. **最小窗 Apps(1 min)**:窗口缩到最小(1080)→ 应用迁移页看 targetBtn 与"高级模式(Symlink)"间隙(预期 12px,顺带 125%/150% DPI 复测 X-25/X-31)。
5. **搜索空态(1 min)**:清空搜索框再输入无结果词 → 观察空列表占位(现无,L-5)与"匹配 0 项"提示(L-4)。
6. **染色与双击(1 min)**:高对比度关/开各看一次风险行颜色(X-14→L-3);双击一行看说明面板是否取对行(L-3)。
7. **补采截图(2 min)**:关闭 RGBBox 后重跑 -tour,补齐垃圾清理/应用迁移两页完整帧(本轮被遮挡),六页两轮齐全归档。

## 6. 与并行美学同事的边界声明

本报告仅覆盖几何/裁剪/对齐/一致性/渲染管线机制:未评价页签选中态弱(shot-01/02 视觉模型提及,属样式)、配色、字体、emoji 观感偏好(仅记录渲染形态事实)、按钮行左锚不右对齐的设计取舍。L-1 虽表现为"大片空白",但根因是机制断裂而非视觉设计,已按范围归入本轮。

## 附:证据清单

- 代码(HEAD be73902):MainWindow.cpp(WM_NOTIFY 238-350 / 加速键 144-192 / MINMAXINFO 229-237 / 倒计时 513-547 / UpdateStatusBar 1220-1280 / UpdateSearchStatus 1304-1334 / 面板 816)、ui/Presenters.cpp(RenderItems 134-171 / SupplyDispInfo 182-203 / ItemAtRow 126-132 / RiskBadge 358-373 / 历史 822-930)、ui/Presenters.h(48-51/204-219)、ui/Controls.cpp(23-47/139)、ui/Layout.cpp(35-152)、core/AppScanner.cpp:153
- 截图:C:\tjf\github\MiniSys\.tmp-shots\shot-01..04.png(可用)、shot-05..14.png(RGBBox 遮挡弃用,残条旁证已注明)
- 遵守约束:仓库只读;未启动/结束正在运行的 MiniSys.exe;未触碰 build.bat、tools\*;报告写入 .tmp-review\minisys-review-r5\07-layout.md。
