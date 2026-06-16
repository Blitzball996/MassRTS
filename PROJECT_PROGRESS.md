# MassRTS 项目进度与规划文档

> 最后更新：本次会话整理。本文档记录当前进度、已知问题、Editor 架构、流体/网络/玩法/强化学习 AI 的设计与调研。

---

## 0. 项目核心目标

基于自研 OpenGL 引擎的大规模即时战略（RTS）+ 大军团战斗模拟。核心诉求：

- 大规模单位（目标十万到百万级），GPU compute 驱动模拟
- SDF / 体素地形，可被炮火破坏、形变
- 自然的 LOD 过渡（远处不突兀）
- 可换模型、音效、特效、数值的数据驱动配置
- 网络同步（多人）
- 玩法：攒资源造兵、推进清场、就地建立新基地
- 长期：强化学习 AI

> 注：UE5 分支用于场景/人物/声音/UI，SDF 地形保留；但本文档聚焦自研 MassRTS 引擎线。

---

## 1. 当前进度（已完成）

### 1.1 GPU 模拟管线
- `shaders/compute_units.glsl`：单位整合 + LOD 分桶
- `shaders/compute_spatial_hash.glsl`：空间哈希加速邻居查询
- `shaders/compute_movement.glsl`：移动积分 + 承伤结算 + 死亡标记
- `shaders/compute_combat.glsl`：交战，原子累加伤害（InterlockedAdd 思路）

### 1.2 渲染
- `src/render/renderer.h`：实例化渲染，mesh / billboard 双路
- LOD crossfade 过渡带已实现（mesh ↔ billboard 渐隐渐显，消除"亮柱/硬切换"）
  - 利用 InstanceData 的 `_pad` 字段承载 fade 权重（结构体大小不变）
  - 4 处实例绑定 + unit/billboard 的 vert/frag 均接入 fade（location 10）

### 1.3 战斗
- 伤害结算移植到 GPU：射程内 + cooldown 就绪即原子累加伤害到目标
- movement pass 扣血、血 <= 0 标记 Dead 并清零速度

### 1.4 其他既有模块
- ECS：`src/ecs/`、空间网格 `src/ai/spatial_grid.h`
- 音频：`src/audio/`（miniaudio）
- 网络：`src/net/`（client/server/session/socket/protocol）
- Steam：`src/platform/steam_integration.h`
- 地形：`src/render/terrain.h`（SDF/体素）
- **SDF 体素地形（本次实装）**：`src/render/sdf_terrain.h` + `sdf_terrain_tables.cpp`
  - 32³ 分块 SDF 体积场，开局由 heightmap 初始化（生成/biome/河流全保留）
  - CPU Marching Cubes，仅重网格被波及的脏 chunk；顶点格式与 terrain shader 一致，零 shader 改动
  - carve() 球形 CSG 挖/填 → 真 3D 坑洞；挖后写回 heightmap，单位/相机/战斗经 get_height_at 自动跟随塌陷地表
  - 渲染为 heightmap 之上的叠加层（polygon offset 防 z-fight），仅渲染被挖过的 chunk
  - 炮弹/核弹命中经 apply_explosion → World::carve_requests 队列 → 主循环 drain → renderer.carve_terrain（每帧上限 8，heightmap GPU 重传每帧合批一次）
  - 联网（lockstep 安全）：protocol.h 新增 TerrainCarve(17) + TerrainCarvePacket{center,radius,op}；carve 为确定性事件，只需广播即可全客户端一致。当前本地直调，多人时改走广播

---

## 2. 已知问题（待修复）

### 2.1 RTS 摄像机被地面下沉卡住（你本次提出）
**现象**：炮兵把地面炸塌下去后，摄像机最低仍只能看到水平面（Z=0），无法俯视到被炸下去的坑里。
**根因**：摄像机的高度/俯角限制是按固定水平面（Z=0）写死的，没有跟随真实地形高度（SDF 采样）。
**修复方向**：
- 摄像机 pitch/最低高度约束改为基于地形 SDF 采样的实际地表高度，而不是常量 0
- 允许摄像机焦点跟随地形下沉，下限取该处地表高度而非 0
- 相关文件：`src/input/camera.h`，需要读 `src/render/terrain.h` 的高度查询接口

### 2.2 爆炸特效贴在水平面，而非被炸位置
**现象**：地面被炸塌后，爆炸特效仍生成在 Z=0 水平面，不在真实被炸的坑底。
**根因**：粒子/特效生成点的 Z 用了水平面常量，没采样 SDF 地形真实高度。
**修复方向**：
- 特效 spawn 时用 SDF 采样得到该 XY 处的真实地表 Z
- 相关文件：`src/render/particles.h`、`src/render/terrain.h`

### 2.4 LOD crossfade 待实跑验证
- 过渡带可能出现半透明穿插闪烁（alpha blend 深度排序问题）
- 若出现：改用 alpha-to-coverage，或收窄过渡带宽度

---

## 3. Editor 架构设计

### 3.1 目标
做一个轻量内置 Editor，目的明确：加场景、换模型、换音效、换特效、调数值，全部数据驱动，无需改代码重编译。

### 3.2 技术选型
- **GUI**：Dear ImGui（轻量、即时模式、与 OpenGL 无缝集成，适合工具面板）
  - docking 分支，支持停靠面板布局
- **资产导入**：标准格式中转，不直读 .uasset
  - 模型：glTF / FBX（新增 gltf 加载器，复用现有 `obj_loader.h` / `skinned_model.h` 思路）
  - 贴图：PNG / JPG（stb_image）
  - 音频：WAV / OGG（miniaudio 已支持）
- **序列化**：JSON（已有 `src/core/json.h`），数据资产以 .json 描述

### 3.3 关于 UE .uasset（重要结论）
**直接读 .uasset 二进制：现实中做不到，给 UE 源码也帮不了。**
原因：.uasset 不是单纯文件格式，而是 UE 整个 UObject 反射 + 序列化系统的内存快照。反序列化它等于在本项目里重建半个 UE（反射系统、类布局、属性系统、依赖解析）。

**可行替代路径（达成"用上 UE 级素材"的真实目标）**：
1. **UE 当导出器**：在 UE 里把 StaticMesh/SkeletalMesh 导出 fbx/gltf，贴图导出 png，本引擎直接吃
2. **直接用源格式素材**：Mixamo（免费骨骼动画角色，fbx/gltf）、Sketchfab、Quixel
3. **UE Python 批量导出脚本**：半自动把项目 mesh/贴图导成 gltf

结论：.uasset 直读堵死，但通过 gltf/fbx 中转，UE 素材（导出后）+ Mixamo + 商城源文件全可用。Editor 第一个核心功能就做 **gltf 导入**。

### 3.4 Editor 面板规划
- **场景面板**：摆放/移动/删除实体，保存为场景 json
- **资产浏览器**：导入并预览 模型/贴图/音效/特效
- **兵种配置面板**：每个兵种的 模型、动画集、音效、特效、数值（血/攻/射程/速度/cooldown）
- **队伍配置面板**：每队的兵种组成、颜色、阵营
- **地形/环境面板**：SDF 地形编辑、河流/水体
- **预览/运行**：所见即所得

### 3.5 数据资产结构（草案）
```
assets/data/
  units/        每个兵种一个 .json（模型路径、动画、音效、特效、数值）
  teams/        每队 .json（兵种组成、颜色、阵营）
  scenes/       场景布局 .json
  effects/      特效定义
```

---

## 4. 流体（河水）设计

### 4.1 目标
- 河水做成流体：被炸起水花、炸开后水会跟着地形流动
- 与破坏性 SDF 地形联动（坑/堤坝改变水流）

### 4.2 技术方案（按工程量从轻到重）
**方案 A：高度场浅水方程（推荐起步）**
- Shallow Water Equations (SWE)，2D 高度场，GPU compute 求解
- 优点：性能好、易与地形高度场耦合、适合大面积河流/湖泊
- 炸击 = 在高度场注入脉冲（水花），地形改变 = 改变底床高度 → 水自然重新分布流动
- 缺点：不擅长飞溅、翻卷等强 3D 行为

**方案 B：SPH / FLIP 粒子流体（局部强效果）**
- 仅在爆炸点局部用粒子流体做飞溅水花，远处仍用高度场
- A + B 混合：高度场做主体流动，粒子做爆点飞溅

### 4.3 与地形耦合
- 地形 SDF/高度图作为流体底床（bed elevation）
- 炮击改地形 → 流体底床更新 → 下一帧水流重算
- 相关文件：`src/render/terrain.h`（地形高度）、新增 `fluid_system`

### 4.4 渲染
- 水面：屏幕空间反射/折射 + 法线扰动
- 水花：粒子系统（复用 `src/render/particles.h`）

### 4.5 网络同步注意
- 流体全量同步代价高。建议：流体由确定性模拟（相同输入→相同结果）在各端本地跑，只同步触发事件（爆炸点、地形改动），而非每帧水体状态

---

## 5. 网络同步（多人）

### 5.1 现状
- 已有 `src/net/`：client/server/session/socket/protocol 基础框架
- 已有 Steam 集成 `src/platform/steam_integration.h`

### 5.2 RTS 网络模型选型
**Lockstep（确定性同步）— RTS 标准做法，推荐**
- 各端只同步玩家指令（建造/移动/攻击命令），不同步单位状态
- 所有端用相同逻辑确定性地推进模拟，结果一致
- 优点：百万单位也只传少量指令，带宽极低
- 关键难点：**确定性**——浮点必须各端一致（GPU 浮点跨硬件不保证一致！）

### 5.3 GPU 模拟 + Lockstep 的核心矛盾
- GPU 浮点跨显卡/驱动结果可能有微小差异 → lockstep 必然漂移崩溃
- 解决路径（三选一或组合）：
  1. **定点数**模拟关键逻辑（位置/血量用整数定点），渲染再转浮点
  2. 模拟逻辑放 CPU 用确定性定点，GPU 只做渲染（牺牲规模）
  3. **状态同步**（server 权威，定期广播状态快照 + 客户端插值）——放弃 lockstep，规模受带宽限制
- 当前 combat 已用定点整数累加伤害（DamageAccum），方向正确，可朝定点确定性推进

### 5.4 同步事件清单
- 玩家指令（造兵、移动、攻击、建基地）
- 爆炸/地形破坏事件（用于流体和地形各端本地重算）
- 随机数种子（确定性随机）

---

## 6. 玩法规划

### 6.1 核心循环（你的设想）
1. 攒资源和钱
2. 用资源生产兵
3. 兵推进，清除一个区域的敌人
4. 清场后可在该区域自行建立新基地
5. 以新基地为前哨继续推进

### 6.2 需要的系统
- **资源系统**：资源点、采集、存储、消耗
- **生产系统**：建筑、生产队列、单位生成
- **建造系统**：基地/建筑放置（合法性检查：地形、是否清场）
- **区域控制**：判定一个区域是否"已清场"（无敌方单位），解锁建造
- **基地系统**：基地血量、被摧毁、提供生产/补给半径
- **战争迷雾**（可选）：视野系统

### 6.3 实现顺序建议
资源 → 生产 → 建造 → 区域控制 → 基地扩张。先单机跑通，再叠加网络同步。

---

## 7. 参考项目调研（RTS / 建造）

### 7.1 开源 RTS 引擎/游戏（可学架构）
- **Spring RTS / Beyond All Reason (BAR)**：开源大规模 RTS 引擎，支持上千单位、地形破坏、流体水面。BAR 是其现代化分支，画面和工程质量高。值得参考其单位/经济/建造系统设计与 Lua 数据驱动配置。
- **OpenRA**：红警/命令与征服开源重制，C#。经济、建造、生产、迷雾系统都很完整清晰，适合学玩法循环和数据驱动。
- **0 A.D.**：开源历史 RTS，C++ + JS。完整的资源采集、建造、科技树，工程成熟。
- **Sanctuary / Annihilation 类**：超大军团方向参考其 LOD 与实例化渲染。

### 7.2 超大军团渲染/模拟参考
- **Ultimate Epic Battle Simulator (UEBS)**：百万单位，GPU 驱动 + VAT 顶点动画纹理。证明你这条 GPU compute + 实例化 + VAT 路线可行。
- **Total War 系列**：大军团 LOD 分级（mesh → impostor/billboard），与你的 crossfade 思路一致。

### 7.3 数据驱动配置参考
- BAR / Spring 的 Lua 单位定义、OpenRA 的 yaml 规则文件，都是"数值/兵种全配置化"的好范例，可借鉴文件组织方式。

### 7.4 借鉴重点
- 经济+建造+生产循环：看 OpenRA / 0 A.D.
- 大规模渲染/LOD：看 UEBS / Total War
- 地形破坏+流体：看 BAR/Spring
- 数据驱动：看 Spring Lua / OpenRA yaml

---

## 8. 强化学习 AI（长期目标 + 打基础）

### 8.1 目标
让 RTS AI 用强化学习（RL）训练，而不是写死的行为树/脚本。

### 8.2 参考先例
- **AlphaStar（DeepMind, StarCraft II）**：RL + 模仿学习打到大师级。证明 RTS 可被 RL 攻克，但成本极高。
- **OpenAI Five（Dota 2）**：大规模自我对弈 PPO。
- **PySC2 / SC2LE**：暴雪+DeepMind 的 StarCraft II 学习环境，RTS RL 的标准参考接口设计。

### 8.3 现实判断
完整 RTS（建造+经济+战斗）端到端 RL 训练成本巨大（AlphaStar 用了海量 TPU）。务实路线：分层、从小做起。

### 8.4 打基础该做什么（关键，现在就能铺）
RL 的前提是环境能被程序高速驱动。现在就该把这些基础打好：

1. **环境接口标准化（最重要）**
   - 把游戏封装成 Gym 风格接口：`reset()` / `step(action)` / `observation` / `reward` / `done`
   - 即使现在不训练，先让游戏能"无 GUI、纯逻辑、可程序控制"地跑一局

2. **Headless 无渲染模式**
   - 训练要跑成千上万局，必须能关掉渲染纯逻辑跑（你已有 `main.cpp` 与 `main_gpu.cpp` 分离，可加 headless 入口）

3. **确定性 + 可加速模拟**
   - 训练需要"快进"（比实时快几百倍）和可复现（固定种子）。这与 §5.3 的确定性需求一致，一举两得。

4. **状态/动作空间定义**
   - 观察：单位/资源/地图状态的张量化表示
   - 动作：离散指令集（造兵、移动、攻击、建造）

5. **进程间通信**
   - C++ 游戏 ↔ Python 训练（PyTorch/RLlib）。常用方案：共享内存、gRPC、或 socket（你已有 `src/net/socket.h` 可复用）

6. **奖励设计**
   - 从简单稠密奖励起步（造成伤害、占领、经济增长），逐步过渡到稀疏的"胜负"奖励

### 8.5 工具栈建议
- 训练框架：**Ray RLlib** 或 **Stable-Baselines3**（PyTorch）
- 算法起步：**PPO**（RTS/连续训练的稳健默认选择）
- 接口规范：**Gymnasium**（Gym 后继）

### 8.6 落地顺序
先做 headless + Gym 接口 + 确定性 → 用小地图小规模（几十单位）跑通 PPO 自对弈 → 再逐步放大。不要一上来就全规模端到端。

---

## 9. 下一步行动清单（建议优先级）

1. 编译验证 LOD crossfade 效果（风险最高的已改项）
2. 修 RTS 摄像机随地形下沉（§2.1）+ 爆炸特效采样真实地表高度（§2.2）
3. 复核自研引擎线的战斗致死链路（§2.3）
4. 搭 ImGui Editor 骨架 + glTF 导入（§3）
5. 玩法：资源 → 生产 → 建造 → 区域控制（§6）
6. 流体高度场原型（§4）
7. 网络确定性改造（§5）
8. RL 基础：headless + Gym 接口（§8.4）

> 建议：1-3 是当前卡点，先做。4 是体验质变点。5-8 按你节奏推进。

---

## 修复记录（本次会话）

- **§2.1 摄像机随地形下沉**：`camera.h` 新增地形高度采样回调 `ground_height`；`update()` 末尾让 look-at 焦点 Y 平滑跟随真实地表高度；`ray_to_ground()` 改为对地形高度场光线步进求交（无采样器时回落到 Z=0 平面）。在 `main_gpu.cpp` 渲染器初始化后接入 `s_terrain_height`。
- **§2.2 爆炸特效贴水平面**：`particles.h` 新增 `ground_height` 采样回调；`update()` 中粒子落地判定改为按真实地表高度（坑底）而非常量 0。同样在 `main_gpu.cpp` 接入采样器。
- 编译验证：`MassRTS_GPU` Release 构建通过（仅剩既有的 APIENTRY/编码 warning）。
- **§2.3 战斗致死链补漏**：`combat_system.h` 中弓箭手 `perform_attack` 发箭、Bomber 自爆 原本用 y=0（高地上会生成在地下），现改为采样 `height_fn` 的真实地表高度，与箭塔/火炮一致。
- CPU 与 GPU 两个 target 均 Release 构建通过。

---

## 新功能：AI 自主安营扎寨（Phase 1）

定位：介于《文明》与 RTS 之间。AI 指挥官自主推进→选址扎寨→建设施→出兵。

- 新模块 `src/game/settlement_system.h`（混合 1C 架构）：`SettlementSystem` 管理据点/建筑的战略状态；每个建筑同时是一个 ECS `is_structure` 实体（HQ→Turret 型自卫，兵营→Wall 型高血），复用现有渲染/战斗管线，零渲染改动。
- 指挥官 FSM：March（行军）→ Settle（植 HQ 圈地）→ Develop（建兵营）→ Produce（兵营周期出兵，集结点朝敌方）。两阵营各一个指挥官。
- 玩家指令优先级最高：Settlement 从不覆盖带玩家 move 指令的单位。
- 接入：`main_gpu.cpp` 在 `start_battle` 中 `g_settlements.init(...)`，主循环 `sim_active` 段调用 `g_settlements.update(world, dt)`。GPU target Release 构建通过。
- 待办（Phase 2+）：扩张与驻防、经济单位（工人/矿工/商人）、反滑雪球游击队/间谍、玩家选据点下达指令。

---

## 摄像机改造：主动下沉 + 可仰视

- 取消"被动随地形下沉"：摄像机焦点 `target.y` 不再自动黏地形，正常飞行保持高度，无论下方地形多低都不会沉。
- 主动升降：新增 `R`=上升 / `F`=下降，速度随缩放距离自适应。
- 可仰视：中键拖拽的 pitch 限位从 `[10°, 85°]` 放宽到 `[-85°, 85°]`，下沉后能把镜头仰起来从坑底/谷底向上看。
- `ground_height` 采样回调保留，仅用于点击拾取地面（`ray_to_ground`），不再驱动高度。
- 控制台帮助新增一行相机操作说明。GPU target Release 构建通过。

---

## 总进度看板

### 已完成
- [x] §2.1 摄像机随地形下沉 bug（旧的被动下沉，已修并被新方案取代）
- [x] §2.2 爆炸特效贴 Z=0 平面 → 落到真实坑底
- [x] §2.3 弓箭手/Bomber 攻击高度 y=0 bug → 采样真实地表
- [x] 新功能 Phase 1：AI 自主安营扎寨（March→Settle→Develop→Produce）
- [x] 摄像机主动下沉 + 可仰视（R/F 升降，pitch 放宽到 ±85°）

### 进行中 / 待办
- [ ] Phase 2：据点扩张 + 驻防兵力维持（扩张越多管理越难）
- [ ] Phase 3：经济单位（平民/工人/矿工/商人）+ 资源驱动建造与出兵
- [ ] Phase 4：反滚雪球——国力评估 + 弱势方派游击队/间谍骚扰强势方城市（3-3 分散包围）
- [ ] Phase 5：玩家选中据点下达建造/出兵/集结指令（玩家指令永远最高优先级）
- [ ] §2.4 LOD crossfade 闪烁（运行时观察项，待实跑确认）

---

## 画面 / 大世界改造（进行中）

> 路线：用公开的工业级算法实现并改编进现有 OpenGL 管线（不参考任何泄露的专有源码）。

- [x] 单位远处"圆柱体"问题 → 根因是几何坍缩：俯视 RTS 角度下竖直卡片几乎侧对镜头被压成竖条。改为**球形（view-space）billboard**——卡片在视空间构建，永远完全朝向相机，绝不坍缩；同时把 LOD 切换距离从 200-800 拉远到 900-2600，让单位在正常视距下保持真实 3D 网格，只在很远处才换成 imposter。文件 `shaders/billboard.vert` + `renderer.h`。
- [x] 天空美化 → 程序化大气散射（Rayleigh + Mie 单次散射，16 步主采样）+ 柔和太阳盘/辉光 + raymarching 体积云 + ACES 色调映射。全屏三角形 pass（`shaders/sky.vert` / `sky.frag`），LEQUAL 深度、不写深度最先绘制；太阳方向与场景光照 `(0.4,0.85,0.3)` 对齐。
- [x] 水面 / 河流美化 → 水面顶点改用 **4 波叠加 Gerstner 波**并解析重算法线（修折角根因）；frag 加 **Schlick 菲涅尔反射** + 天空反射色 + 双层滚动法线波纹 + 太阳镜面高光 + 柔和岸边泡沫（`terrain.vert` / `terrain.frag`，新增 `u_cam_pos`）。河道生成改为多倍频正弦叠加（伪 fbm）蜿蜒曲线 + 宽度起伏，不再笔直（`terrain.h::carve_river`）。
- [x] 地形美化（雾）→ 大气雾从"离世界原点距离"改为**相机相对距离**指数雾，并向天空地平线色过渡，远景自然融进程序化天空，消除地形与天空硬边（`terrain.frag`）。陆地生物群系混合（草/森林/沙/山/雪/苔藓）原本已较丰富，保留。
- [x] 矿厂 + 农田 → `BuildingKind` 扩展 `Mine`/`Farm`，接入 Settlement 系统；指挥官 Develop 阶段在 HQ 后方对称建矿场+农田，建成后按 **矿场 6 金/秒、农田 3.5 金/秒** 自动给对应阵营产钱（帧率无关的累加器）。矿场=矿石灰、农田=作物绿，各有专属配色与血量。
- [ ] SDF 体素地形（Marching Cubes + 可雕刻，替换 heightmap）— 引擎架构级重写，下一步分阶段做
- [ ] 地图 ×10 + World Partition（分块流式加载 + 视距分级 LOD）— 引擎架构级，下一步分阶段做

---

## 修复（本轮）

- [x] **安营扎寨"没反应"的 bug** → 根因：开局每方生成 100000 单位，红+蓝=200000 正好等于 `MAX_LIVE_UNITS` 上限，世界开局即满，`create_entity()` 永远返回 `INVALID_ENTITY`，HQ/兵营/矿场/农田一个都建不出来。修法：开局兵力降到每方 90000（共 180k），留 20k 余量给 AI 建筑与持续增援；仍是十万级大军。`main_gpu.cpp` 共改 4 处。另外把行军速度从 60 提到 120 u/s 加快扎营，并在 `place_building` 加控制台日志便于验证。
- [x] **河流/沼泽锯齿折角** → 根因是生成时的**硬边界**：河岸 `dist<river_width` 和沼泽 `height<8 && slope<0.1` 都是按网格单元的硬阈值，边界沿网格阶梯化。修法：用多倍频 `fbm` 噪声扰动河岸位置（±14u 大摆动 + ±5u 细摆动）和沼泽阈值，使水陆边界有机蜿蜒而非网格对齐。（地形法线本就是中心差分平滑，非折角来源。）`terrain.h`。


---

## SDF 体素地形（Plan B：整图全 SDF，进行中）

> 决策记录：heightmap 渲染与 SDF 曾经**同时画同一块地**，polygon offset 压不住两层穿插 → z-fighting 表现为"小三角锥 + 锯齿"。任何参数（smin / 法线 / 深度阈值）都治不了，因为同一像素有两个面。最终选 **Plan B：整图完全由 SDF + Marching Cubes 渲染，heightmap 不再绘制**（数据保留，仅供 `get_height_at` 给单位/相机/战斗查询）。每像素单面，根除 z-fight。

### 架构
- 新模块 `src/render/sdf_terrain.h` + 标准 Marching Cubes 表 `src/render/sdf_terrain_tables.cpp`（命名空间常量默认内部链接，必须加 `extern` 才能跨 TU 链接）。
- SDF 场按 **32³ 体素分块（chunk）**，体素 3 单位；**稀疏分配**——只有"地表穿过 / 被挖过 / 含底面"的 chunk 才分配 float 数组，其余只存均匀值标记。否则 6000 地图按满额分配会吃约 1.7GB 内存。
- 顶点格式与现有 terrain shader 完全一致（pos+normal+uv+biome+height_norm），零 shader 改动即可复用。
- init 时一次性网格化整图；稀疏化后 6000 地图仍可秒开。

### 已完成
- [x] 整图全 SDF 渲染，根除 z-fighting（小三角锥/锯齿）。
- [x] **真体素体地图**：地表以下到 `SDF_FLOOR_Y` 全实心，挖才有"肉"；底面网格化使侧视有厚度（slab）。
- [x] `carve()` 球形 CSG，**smooth-min/max（IQ 多项式软混合）**圆滑坑壁成碗状。
- [x] **carve 范围外扩 1 圈** + 邻居 chunk 标 dirty → 修"挖坑接不上"（共享边界体素两侧写一致值）。
- [x] **挖穿限制**：dig 不能低于 `SDF_FLOOR_Y`、raise 不能高于 ceiling，clamp 防穿透。
- [x] **4 个笔刷全走 SDF**（按 B 进入，数字键）：1=升高土壤(Fill) / 2=挖坑(浅碗) / 3=平滑(Laplacian) / 4=挖洞(深井)。
- [x] **深度分层着色**：顶点带距地表 depth，<1.5m 保留地表 biome，1.5–14m=泥土(7)，>14m=岩石(8，带层理+矿脉)；深处按 depth 渐变变暗。
- [x] **炮弹/核弹命中自动炸坑**：`World::apply_explosion` 把 carve 请求入队，主循环每帧 drain 调 `renderer.carve_terrain`（半径≥12 才炸坑）。
- [x] **联网 carve 协议（lockstep 安全）**：`protocol.h` 加 `TerrainCarve=17` + `TerrainCarvePacket{center,radius,op}`；carve 是确定性事件，只广播参数，各客户端重放得一致 SDF。
- [x] **地图 ×2**：`Terrain::WORLD_SIZE` 3000 → 6000。
- [x] 性能：每帧最多 8 个 carve、GPU heightmap 重传合批（每帧最多一次）。

### 进行中 / 待办（本轮反馈）
- [x] **#1 颜色**：深处改为**以灰色为基础渐变加深成深灰**（当前偏蓝，与水难分）。← 已在 frag 调整，待验证  [DONE: frag grey-scale depth gradient verified]
- [x] **#2 strength 控制**：B 模式下玩家用滚轮/+- 调挖掘力度，**默认调低**，HUD 显示当前值。  [DONE: scroll wheel adjusts strength 0.1-1.0 default 0.35; scales stamp depth; HUD bar+value]
- [x] **#3 笔刷光标**：B 模式下鼠标位置画圈指示笔刷范围。  [DONE: world-space ground ring under cursor, color per brush; shaders/brush.* + render_brush_ring]
- [x] **#4 挖洞（隧道）**：沿光标方向**持续渐进**掘进，而非一次性球。  [DONE: carve_tunnel stamps overlapping spheres along drag; cave brush bores continuous shaft]
- [x] **#5 挖穿限制**：极限深度更低、升高极限远高于下挖极限（不对称）。← 已加 clamp，待调参  [DONE: clamp to SDF_FLOOR_Y / ceiling, asymmetric] [TUNED: DIG_FLOOR -200, RAISE_CEIL +400 (build ~2x deeper than dig)]
- [ ] **#6 挖洞联网**：隧道 carve 走 `TerrainCarvePacket` 广播。
- [x] **#7 远处雪山消失**：snow 误用 depth(≈0) 触发，改用真实世界 Y。← 已改 frag，待验证  [DONE: snow keyed off world Y]
- [x] **#8 上坡变慢**：兵种爬坡降速（坡度影响 move speed）才有战术意义。  [DONE: GPU path now uploads per-unit get_speed_mult to p2 (biome*slope)]
- [x] **#9 初始兵力**：降到 35k vs 35k（共 70k）。← 已改，待验证  [DONE: 35k vs 35k verified]
- [x] **#10 沼泽碎裂**：低平区 SDF 网格在 3 单位体素下抖动/biome 边界碎裂，待修。  [DONE: gradient-normalized SDF (divide by sqrt(1+|grad h|^2)) + 5x heightmap smooth kills MC spikes; verified via --shots]
- [x] **#11 侧视厚度**：地图做成有厚度的体（底面/侧壁可见）。← 已加 slab 底面，待验证  [DONE: solid slab bottom, side thickness visible]
- [ ] **#12 无限延展地图**：World Partition / 学 Minecraft 做无限分块流式（量级大，单独阶段做）。

### 本次会话新增准备（groundwork）
- [x] **流体系统脚手架**：新增 `src/render/fluid_system.h`（Plan A 浅水方程 SWE）。已就位：256^2 模拟网格、water_height/bed_height 主机镜像、refresh_bed()（地形编辑后重采底床）、add_water_pool()/add_splash()（注水与爆炸脉冲）、ping-pong SSBO 字段与 update(dt) 调度桩。SWE compute 内核与水面渲染待后续填充；gpu_ready=false 不影响现有构建。
- [ ] **无限地图准备**：现有 SDF 已是分块（SDFChunk 32^3 体素 + 稀疏 unordered_map 索引），天然适配流式。下一步：把 chunk 坐标改为以相机为中心的滑动窗口懒加载/卸载，地形高度查询改为按需 noise 生成。属引擎架构级，单列阶段做。

### 已知约束
- 文件写入受杀软/索引锁影响（truncate 失败），编辑改用 Python `r+b` 原地覆盖 + 不足补空格，文件尾可能残留无害空白。
- 改代码前需先关闭运行中的游戏（占用 `build/Release` 目录锁）。
