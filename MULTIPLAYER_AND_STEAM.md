# MassRTS — 联机 + Steam 上架 技术方案

本文档覆盖三件事：
1. "卡住单位" bug 的真正原因与修复
2. 局域网/直连联机的架构与落地（推荐 **Lockstep**，附理由）
3. Steamworks SDK 集成与上架准备清单

---

## 1. 卡住单位 Bug —— 已定位并修复

### 真正原因
状态枚举：`Idle=0, Moving=1, Attacking=2, Dead=3, Retreating=4, Ragdoll=5`

两个 compute shader 顶部都有：
```glsl
if (u.state >= 3u) return;  // 本意：跳过 Dead/Ragdoll
```
但 `>= 3u` 把 **Retreating(4)** 也包含了。于是：
- 单位血量低于撤退阈值 → combat shader 把 `state` 设为 4
- 下一帧两个 shader 都在顶部 `return`，**撤退+回血逻辑（compute_combat.glsl 109-126 行）和 movement 位置积分都到不了**
- 单位被永久冻结在原地，alive 但永远不动

大战中大量残血单位变成这样，看起来就像"死了却没尸体、一直 idle 卡住"。
你的 DIAG 没抓到是因为它只统计 Moving/Idle 为 stuck（`st_ret` 单列、未计入 stuck）。

### 修复
两个 shader 改为只跳过 Dead 和 Ragdoll：
```glsl
if (u.state == 3u || u.state == 5u) return;
```
- `shaders/compute_movement.glsl` ✅ 已改好
- `shaders/compute_combat.glsl` ⚠️ 逻辑已改对，但文件末尾有一个多余的 `x` 字节（调试时误写入，文件被某进程独占写锁导致无法清除）。**请关闭正在占用该文件的程序（运行中的游戏 / 编辑器 / Live Server）后，删掉最后一行那个孤立的 `x`，或让我重跑清理脚本。** 否则该 shader 编译会失败。

### 顺带建议（非必须）
- DIAG 的 stuck 统计应把 `Retreating` 且 `velocity<0.3` 也算进去，便于将来发现同类冻结。
- 调试完后删掉 `[DIAG] printf` 和 `fix_*.py` 临时脚本。

---

## 2. 联机方案：局域网/直连 + Lockstep（确定性同步）

### 为什么选 Lockstep（而不是状态同步）
你的核心约束是 **几万单位**。
- **状态同步**：主机每帧把所有单位的 pos/vel/state 发给客户端。几万单位 × 每个~40字节 × 20Hz = **几十 MB/s**，局域网都吃力，公网不可能。直接否决。
- **Lockstep**：只同步**玩家指令**（每帧通常 0~几条），带宽近乎为零。所有客户端用相同输入、相同算法、各自跑完整模拟，结果必然一致。这正是 AOE/星际等"海量单位 RTS"的标准做法，也是你现有 protocol（TickSync / CommandBroadcast）本来就设计的方向。

### Lockstep 的硬性前提：**模拟必须完全确定性**
所有客户端每一 tick 必须算出**逐位相同**的结果。当前项目有几个确定性风险，必须先解决：

| 风险点 | 位置 | 处理 |
|---|---|---|
| 敌方 AI 购买用墙钟种子 | `main_gpu.cpp:391` `mt19937 erng((uint32_t)(now*100))` | **改为 tick 计数做种子**，如 `mt19937 erng(0xBEEF ^ game_tick)`。墙钟在不同机器上不同，必然 desync。 |
| 出兵随机摆放 | `main_gpu.cpp:222` `mt19937 rng(42+faction*7777)` | 已是固定种子 ✅，保持。 |
| 变长 dt（帧率相关） | 主循环用真实 dt | **联机时改用固定步长** `FIXED_DT = 1/30`，模拟与渲染解耦（见下）。 |
| **GPU 浮点确定性** | compute shaders | ⚠️ **最大风险**。不同 GPU/驱动对浮点（尤其 `atan/normalize/length` 等）结果可能有最后几位差异，累积后 desync。 |

### 关于 GPU 浮点确定性（关键决策）
跨不同显卡做逐位一致的 GPU 浮点模拟**非常难保证**。三个现实选项：

**A. 局域网"同机型/同驱动"约定（最省事，推荐先做）**
   - 仅声明支持局域网、相同或相近 GPU。配合"desync 检测 + 自动用主机状态纠正"兜底。
   - 实现：每 N tick 各客户端算一个 **状态校验和（hash 全部单位 pos/hp/state）**，发给主机比对。不一致就让主机广播一次全量快照纠正（用你已有的 `GameState` 包）。
   - 这样即使偶尔浮点漂移，也能自愈，体验可接受。**建议作为第一版。**

**B. CPU 确定性模拟分支**
   - 联机时不用 GPU 模拟，改用 `combat_system.h`/`movement_system.h` 的 CPU 路径（已存在 fallback），用整数/定点或严格 IEEE 浮点。CPU 跨平台浮点更可控。
   - 代价：几万单位 CPU 模拟性能远低于 GPU，可能要降低联机单位上限。

**C. 主机权威 + 增量状态同步（混合）**
   - 只有主机跑 GPU 模拟，客户端只接收"变化的单位"增量。介于状态同步和 lockstep 之间，但几万单位增量仍可能很大。

> **推荐路线**：先做 **A**（Lockstep + 校验和 + 主机快照纠正兜底），局域网场景下体验好、工作量小。后续若要公网/跨显卡，再评估 B。

### 与现有代码的对接（要补的洞）
现有 `client.h/server.h` 是 relay 雏形，缺这些 lockstep 必需件：

1. **指令延迟执行（input delay）**：本地点击的指令不立即执行，而是打包到 `tick = current_tick + INPUT_DELAY`（如 +3 tick ≈ 150ms），等所有人都收到该 tick 的全部指令后**同时执行**。这是 lockstep 不卡顿的核心。
2. **Tick 屏障**：每个客户端只有在收到"该 tick 所有玩家的指令（或空指令确认）"后才推进模拟。需要 `TickSync` 包做确认。
3. **指令缓冲区**：`std::map<tick, vector<Command>>`，按 tick 累积、到点执行。
4. **校验和上报与 desync 处理**（方案 A）。
5. **CommandPacket 扩展**：当前只有"移动选中范围"。需支持 attack-move / stop / 购买 / 核弹 等所有会改变模拟的玩家操作——**任何改变世界状态的输入都必须走网络**，否则 desync。
6. **指令里的单位标识**：现在用 `unit_start..unit_end` 范围，多人下每个玩家拥有的单位不连续。改为"按 faction + 选择集"或显式 ID 列表更稳。

### 模拟/渲染解耦（联机与单机都受益）
```
accumulator += real_dt;
while (accumulator >= FIXED_DT && tick_ready(sim_tick)) {
    apply_commands(sim_tick);     // 执行该 tick 所有玩家指令
    step_simulation(FIXED_DT);    // GPU dispatch + readback + CPU attack
    sim_tick++;
    accumulator -= FIXED_DT;
}
render(interpolate);              // 渲染用最新状态（可插值）
```

### 落地步骤（建议顺序）
1. 修掉确定性风险（erng 种子、固定步长）。
2. 抽出 `NetSession`（封装 client/server，main_gpu 只调用 `session.poll()/queue_command()/advance(tick)`）。
3. 把"右键移动 / 购买 / 核弹"等输入改为：本地不直接改 world，而是 `session.queue_command(...)`；命令到点后由 `apply_commands` 统一改 world。
4. 加 tick 屏障 + input delay。
5. 加校验和上报 + 主机快照纠正。
6. 菜单加"创建主机 / 输入IP加入"。

---

## 3. Steamworks SDK 集成 + 上架准备

### 3.1 前置（Steamworks 后台）
1. 注册 Steamworks 合作伙伴账号，缴纳 $100 上架费/每款游戏。
2. 在后台创建 App，拿到 **AppID**（开发期可用测试 AppID `480` SpaceWar 跑通流程）。
3. 配置：商店页、定价、年龄分级、支持平台、Depots（构建上传容器）。

### 3.2 SDK 集成（代码层）
1. 下载 Steamworks SDK，把 `sdk/public/steam` 头文件和 `sdk/redistributable_bin/` 的 lib/dll 放进项目。
2. 项目根放 `steam_appid.txt`（内容就是 AppID），开发期用；正式版由 Steam 启动器注入。
3. CMake 链接 `steam_api64.lib`，把 `steam_api64.dll` 拷到可执行目录。
4. 启动时 `SteamAPI_Init()`，退出 `SteamAPI_Shutdown()`，主循环每帧 `SteamAPI_RunCallbacks()`。
5. （可选但推荐）`SteamAPI_RestartAppIfNecessary(appid)`：非 Steam 启动时自动通过 Steam 拉起。

封装见随附的 `src/platform/steam_integration.h`（已生成骨架，未启用，需放入 SDK 后开宏 `MASSRTS_USE_STEAM`）。

### 3.3 推荐接入的 Steam 功能
- **成就/统计**：`ISteamUserStats`（如"消灭1万单位""赢得首胜"）。
- **Steam 联机**（强烈建议替代裸 UDP）：`ISteamNetworkingSockets` / `ISteamNetworkingMessages`。
  - 好处：自带 NAT 穿透 + Steam 中继（SDR），玩家不用手输 IP、不用开放端口，公网可玩。
  - 你现有的 UDP `socket.h` 适合局域网直连；Steam 网络适合好友/大厅联机。**两者可并存**：局域网走 UDP，Steam 走 NetworkingSockets，上层 `NetSession` 抽象统一。
- **大厅/邀请**：`ISteamMatchmaking` 建房、好友邀请。
- **创意工坊**（地图/MOD）：`ISteamUGC`，你已有 `CUSTOMIZATION.md`，契合度高。
- **云存档**：`ISteamRemoteStorage`（存档/设置）。
- **Overlay**：自动可用，注意游戏要用标准窗口/输入以兼容。

### 3.4 上架硬性 Checklist
- [ ] 商店页：名称、简介、5+截图、预告片、标签、系统需求
- [ ] 美术资产：Logo、胶囊图（多尺寸）、Header
- [ ] AppID + Depot 配置，用 SteamPipe（`steamcmd` + `app_build.vdf`）上传构建
- [ ] `steam_api64.dll` 随包发布；删除开发期 `steam_appid.txt`
- [ ] 通过 Steamworks "上架前审核"（Review）—— 会检查能否启动、Overlay、退款政策符合等
- [ ] 年龄分级问卷、支持的语言、隐私政策
- [ ] 定价与发售日设置
- [ ] 构建在"Default" branch 设为可玩，提交审核
- [ ] EULA / 第三方依赖授权（GLFW/GLM/glad/miniaudio 的 license 已在 build/_deps，需在游戏内或文档列出）

### 3.5 与联机的关系（建议）
- **第一版（验证）**：局域网直连 UDP（你现有方案）+ Lockstep + 校验和。
- **上架版**：把 `NetSession` 的传输层替换/增加 `ISteamNetworkingSockets`，复用同一套 Lockstep 上层逻辑。这样从局域网平滑升级到 Steam 好友/大厅联机，无需重写同步逻辑。

---

## 待办优先级
1. ✅ 修 shader 卡住 bug（仅剩清掉 `compute_combat.glsl` 末尾多余 `x`）
2. 修确定性风险（erng 墙钟种子 → tick 种子；固定步长）
3. 抽 `NetSession` + lockstep tick 屏障 + input delay
4. 校验和 desync 检测 + 主机快照纠正
5. 集成 Steamworks（先成就/Overlay，再 NetworkingSockets）
6. 走 SteamPipe 上传 + 商店页 + 审核
