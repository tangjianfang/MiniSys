# 02/03/05 号评审子代理补发说明(限额 2026-10-08 00:33 重置后执行)

> 背景:第三轮评审(docs/REVIEW_2026-10-06-3.md)派发 5 路,04/07 完整交付,
> 02/03/05 因 API 限额(429)中途夭折。本文件供重置后的会话重新拼装派发。
> 派发方法:按 C:\Users\tjf\.claude\skills\multi-role-app-review\ 的 SKILL.md
> Step 1(通用约束块 + roles.md 角色模板 + 线索块),三路放同一条消息并行派发。

## 线索块公共事实(相对上轮的变化)

- HEAD:v2.12(开发者缓存根设置 UI + GuardRails 对抗语料 11 项 + 3 处安全修复)
- **索引 27 条截断已修**(HighUsn=INT64_MAX,VolumeIndex.cpp)——待用户活体
  重建确认条目数;若确认,03 号的"根因分析"任务改为"验证修复 + 评估多卷性能"
- **缓存投毒提权已修**(双层:RunPlan 白名单复验 + TryLoadCache 消毒),
  05 号的 T-B1 假设已 Confirmed 并修复——不要再报,转向其余面
- 新增已落地:多卷索引(v2.11)、空闲维护(IdleMaintenanceAsync)、会话恢复、
  命令面板、任务代数(R-4)、对抗语料测试
  (MiniSysTests/tests/GuardRailsAdversarialTests.cpp,92/92 全过)
- 用户数据:%LOCALAPPDATA%\MiniSys 只读;应用未运行时才能链接 build\Release
- 分报告写到 C:\Users\tjf\AppData\Local\Temp\minisys-review-<当日>\

## 各角色任务裁剪(其余按模板)

- **02 UI 延迟**:重跑其留下的微基准(probe 源码在旧临时目录,可重建);
  重点补:完整延迟预算表、UI stall watchdog 方案、RenderItems 视图状态
  保持(v2.10 已加)后的残余卡顿点
- **03 后端**:根因已修——评估多卷索引的首建/增量/内存(80B/文件),
  遍历回退盘的删除同步缺口,DirSizeCache 并发,单 worker 队头阻塞
- **05 安全**:T-B1 已修;对抗面转向:8.3/尾点/junction TOCTOU 的**实测**
  (对照 GuardRailsAdversarialTests.cpp 已覆盖项勿重复)、rules.json 绕过、
  index-cache.bin 投毒影响面、ShellExecute /select 注入、JSONL 破坏样本

## 完成后

逐条复核(主审)→ 更新 docs/REVIEW_2026-10-06-3.md(补 02/03/05 章节)
→ 分报告归档到 docs/review-2026-10-06-3/。
