# 02/03/05 号评审子代理补发说明(2026-10-11 刷新版)

> 背景:第三轮评审(docs/REVIEW_2026-10-06-3.md)派发 5 路,04/07 完整交付,
> 02/03/05 因 API 限额中途夭折;核心问题当时已由主审接手修复。
> 本文件 2026-10-11 已刷新到当前 HEAD。派发:按 multi-role-app-review
> 技能(通用约束块 + roles.md 模板 + 下方线索),三路同一条消息并行。

## 线索块公共事实(2026-10-11,HEAD 76f6514 = v2.13c,build 35)

**已修勿重报**(此前评审遗留项全部关闭):
- 索引截断:HighUsn=INT64_MAX 未治好用户卷的 USN 截断 → 现以**并行遍历回退**
  为正路(6 线程,实测建成 **2,831,802 项**);walk 索引**落盘**(缓存格式
  v2 带 walk 标志)+ 空闲自动重遍历保鲜;取消不再产生空壳索引
- 缓存投毒提权:双层修复(RunPlan 执行点白名单复验 + TryLoadCache 消毒),
  附对抗语料 MiniSysTests/tests/GuardRailsAdversarialTests.cpp(11 项)
- R-4 任务代数(DONE 消息盖代数戳);R-1/2/3/5;07 号 X-1..X-16 主干
- ≤5s 预算达标:热扫描 41ms / 搜索 76ms(PerfBenchTests 固化;
  目录级子树作用域增量发现是关键算法)
- 视觉评审轮跑通(-tour 巡页 + tools/screenshot.ps1;V-R1 列插入缺失/
  V-R5 测试污染用户缓存/V-R6 状态栏跨页残留 已修)
- 测试 94/94;身份=磁盘瘦身助手(全部固定磁盘)

**已知未竟**(本轮各角色的任务重心):
- 02:完整延迟预算表、UI stall watchdog、1000 行渲染 0.8-1.6s(实测)的
  虚拟列表评估
- 03:单 worker 队头阻塞(长任务独占)、多卷索引内存(~80B/文件,283 万项
  实测多少 MB?从进程工作集反推)、walk 索引无 USN 增量(空闲重遍历补偿,
  量级评估)、DirSizeCache 并发正确性
- 05:对抗面扩展——rules.json 路径/通配符/环境注入语料实测、index-cache.bin
  投毒影响面(伪造 names/dirPaths 对 HasDir 短路和搜索显示的欺骗范围)、
  ShellExecuteW "/select" 参数注入、history.jsonl 破坏样本对
  UpdateStatusBulk 重写的影响、GuardRailsAdversarialTests 未覆盖的变体

用户数据 %LOCALAPPDATA%\MiniSys 只读;分报告写
C:\Users\tjf\AppData\Local\Temp\minisys-review-20261011\。
应用可能正在运行(勿动进程;标准输出目录被锁时构建到其他 OutDir)。
rotate.ps1 excluded from review blacklist (it launches the app by user directive 2026-10-12)
