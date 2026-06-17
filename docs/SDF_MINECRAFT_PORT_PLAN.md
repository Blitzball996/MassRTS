# SDF-Minecraft 移植计划（SDF 体素生存建造模式）

> 目标：完全参考 `G:\CMakePJ\MinecraftConsoles`（Minecraft Legacy Console Edition / TU19 真实商业 C++ 源码），
> 把它的**全部生存建造玩法**移植进 MassRTS，但底层用 **MassRTS 的 SDF 体素**而非方块体素，
> 并支持**无尽世界**。复用 MinecraftConsoles 的角色模型/材质/动画。
>
> **关键约束：**
> 1. 单独出一个模式 `SDFCraft`，与原 RTS 模式**完全隔离**，互不冲突。
> 2. 从第一行代码起就考虑**网络同步复制**（服务器权威 + 客户端预测）。
> 3. 同时支持 **Listen Server（主机兼玩家）** 和 **Dedicated Server（专用服）**。
> 4. 先写完整计划（本文件，覆盖全部功能不漏），再按**最小可玩循环**开始，
>    每完成一个系统**自测通过**后在本文件打勾。

---

## 0. 进度总览（每个系统完成并自测后打勾）

- [x] 阶段 A：基础设施与模式骨架（独立 SDFCraft.exe + 第一人称 + 区块世界渲染）
- [~] 阶段 B：最小可玩循环（MVP）（单机挖/放/走已通；多人同步待 N 阶段网络层接入）
- [~] 阶段 C：物品 / 背包 / 合成（物品/工具/合成/熔炼逻辑已通；容器UI与掉落实体待 F/O）
- [ ] 阶段 D：实体 / 生物 / AI
- [ ] 阶段 E：战斗 / 生存数值
- [ ] 阶段 F：方块功能与方块实体（容器/熔炉/工作台）
- [ ] 阶段 G：红石系统
- [ ] 阶段 H：维度系统（魔影世界/末地/天空岛）
- [ ] 阶段 I：Boss（末影龙 / 凋灵）
- [ ] 阶段 J：世界生成（生物群系/结构/矿物/洞穴）
- [ ] 阶段 K：附魔 / 药水 / 状态效果
- [ ] 阶段 L：村民 / 交易 / 村庄
- [ ] 阶段 M：持久化存档
- [ ] 阶段 N：网络完整化（专用服 + 兴趣管理 + 反作弊）
- [ ] 阶段 O：UI / HUD / 音效 / 打磨

> 说明：阶段顺序按"最小循环优先"排列。A+B 跑通即可玩（挖/放/走/多人看见彼此）。
> 之后每阶段都是独立可测增量。

---

## 1. 技术基础（两模式共用）

### 1.1 体素体系：SDF + 方块混合渲染

MinecraftConsoles 底层是离散整数方块（`Tile` 类，16³ = Chunk），MassRTS 底层是连续 SDF（Marching Cubes，32³ chunk @ 3m/voxel）。移植策略：

| 层 | 表示 | 用途 |
|----|------|------|
| SDF 自然地形 | `SDFChunk::sdf[]`（float） | 山地/洞穴/水体，Marching Cubes 渲染（平滑） |
| 方块放置层 | 新增 `BlockLayer`（uint8 per voxel） | 玩家放置/挖掘的具体方块，立方体网格渲染 |
| 合成渲染 | 先 SDF mesh，再叠方块 mesh | 自然地形平滑 + 建造方块方正并存 |

**参考文件：**
- `Tile.h / Tile.cpp`（MinecraftConsoles）→ 移植方块 ID 枚举
- `src/render/sdf_terrain.h`（MassRTS）→ 扩展为混合层
- `SDFTerrain::CarveOp`（已有 Dig/Fill）→ 扩展为方块放置

### 1.2 无尽世界（Infinite World）

MinecraftConsoles 是固定大小地图；无尽世界需要**区块动态流式加载**。

| 要素 | 设计 |
|------|------|
| 坐标系 | 全局 int32 体素坐标，区块以 `(cx,cy,cz)` 表示 |
| 加载策略 | 玩家周围半径 8 区块（同 VoxelNetEngine::VIEW_DISTANCE）动态加载/卸载 |
| 生成 | 无尽 Perlin/simplex 种子驱动，per-chunk 惰性生成（首次加载时触发） |
| 持久化 | RegionFile 格式（.mca）—— 仅存储"修改层 Delta"，自然层每次从种子重建 |
| 参考 | `RandomLevelSource.cpp`（MC 世界生成）、`RegionFile.cpp`（MC 存档格式） |

### 1.3 网络架构（从第一天设计进去）

见 `docs/DUAL_MODE_NETWORK.md`（已有完整设计）。SDFCraft 模式用**服务器权威 + 客户端预测**：

```
listen server = 主机自己也是玩家（VoxelNetEngine 在本地线程起服务）
dedicated server = 无渲染，纯服务器进程（命令行参数 --server）
client = 连接服务器，本地预测 + 网络确认
```

**已有骨架：** `src/net/voxel/voxel_net_engine.h/.cpp`（已有，未完成）。
本计划从第一个可玩系统（B 阶段）起就挂上这个网络层。

---

## 阶段 A：基础设施与模式骨架

> 目标：建立独立的 SDFCraft 模式入口，与 RTS 完全隔离；搭好状态机、第一人称相机、
> 渲染管线、世界对象。不含玩法，但能进入空世界自由飞行。

### A1. 模式隔离与入口
- [x] 新增 `src/sdfcraft/` 目录，所有 SDFCraft 代码隔离在此命名空间 `sdfcraft::`
- [~] 新增主菜单选项 "SDF Survival"（暂用独立可执行 `SDFCraft.exe`，菜单接入留待 O 阶段）
- [x] 独立 `main_sdfcraft.cpp` 入口（独立 GLFW 窗口 + GL 上下文，与 RTS 零耦合）
- [~] 启动参数：当前支持 `SDFCraft.exe <seed>`；server/host 参数留待 N 阶段

### A2. 第一人称相机与角色控制器
- [x] `sdfcraft::Player` 控制器：WASD 移动、鼠标视角、跳跃、重力、碰撞（`player.h`）
- [~] 参考 `MovePlayerPacket` / `PlayerInputPacket`（输入结构已抽象为 `FrameInput`，网络化待 N）
- [x] 体素碰撞检测（AABB vs 方块层，逐轴 swept + 二分贴合）
- [x] 飞行/创造模式切换（F 键）

### A3. 角色模型复用
- [ ] 从 MinecraftConsoles 提取 Steve/Alex 模型几何 + 皮肤材质
  - MC 角色是程序化立方体模型（`HumanoidModel`），非 .mesh 格式
  - 方案：用 MassRTS 的 `SkinnedModel`（.mesh/.anim）格式重建一个人形，或直接程序化立方体人形
  - 皮肤纹理：复用 MC 的 `char.png` / DummyTexturePack
- [ ] 第三人称/第一人称手臂渲染

### A4. 混合体素渲染
- [x] 区块方块层（每 voxel 一个 uint8 方块 id，`world.h` `Chunk::blocks`）
- [x] 方块网格生成器（隐藏面剔除 + 跨区块面剔除，`mesher.h`）
- [~] 方块纹理图集（当前用调色板顶点色 + 方向明暗；terrain.png 图集留待 O 阶段）
- [x] 渲染顺序：不透明方块 → 透明方块（水/玻璃/树叶，`chunk_renderer.h`）

**A 阶段自测：** 2025-xx，`SDFCraft.exe` 编译通过并启动（6s 烟测无崩溃/无 GL 报错），
进入无尽程序化世界，第一人称行走/飞行，看到地形、树、水、矿物分层。✅

---

## 阶段 B：最小可玩循环（MVP）⭐ 先做这个

> 目标：**挖 / 放 / 走 / 多人看见彼此**。这是"最小可玩循环"，做完就能多人联机挖矿建造。

### B1. 挖掘（破坏方块）
- [x] 射线检测选中体素（体素 DDA raycast，`player.h` `Player::raycast`）
- [~] 挖掘进度（当前为固定冷却 0.18s；按硬度+工具的进度条留待 D/E 阶段）
- [x] 挖掉 → 方块层置 AIR → 物品进背包（掉落实体留待 C 阶段）
- [ ] **网络：** 客户端预测 + `VoxelPlayerActionPacket(BREAK)` → 服务器验证广播（待 N 阶段）

### B2. 放置（建造方块）
- [x] 选中面 + 放置方块（按命中面法线 set 方块，避免压住玩家自身）
- [x] 放置消耗手持物品（`Inventory::consume_held`）
- [ ] **网络：** `VoxelPlayerActionPacket(PLACE)`，服务器扣背包 + 广播（待 N 阶段）

### B3. 玩家移动同步
- [ ] 4 字节增量移动包（移植 `MoveEntityPacketSmall` 位打包），见 DUAL_MODE_NETWORK 1.3
- [ ] 其他玩家实体渲染（用 A3 的角色模型）
- [ ] 快照插值（平滑其他玩家移动）

### B4. 区块流式同步
- [ ] 兴趣管理：服务器按玩家位置推送区块（`VoxelChunkVisibility` + `VoxelChunkData`）
- [ ] 客户端按需请求 + 卸载远区块
- [ ] 已有骨架 `VoxelNetEngine::serverUpdatePlayerView`

### B5. Listen / Dedicated server
- [ ] Listen：主机进程内起 VoxelNetEngine 服务 + 本地客户端
- [ ] Dedicated：`--sdfcraft-server`，无渲染主循环，纯逻辑 tick（20Hz，参考 MC `MAX_TICKS_WITHOUT_INPUT`）

**B 阶段自测：** 两个客户端连同一服务器，A 挖的坑/放的方块 B 实时看到，互相看到角色移动。

---

## 阶段 C：物品 / 背包 / 合成

> 参考 MC：`Inventory.cpp`, `Item.cpp`, `ItemInstance.cpp`, `Container.cpp`,
> `Recipes.cpp`, `ShapedRecipy.cpp`, `ShapelessRecipy.cpp`, `CraftingMenu.cpp`,
> `WorkbenchTile.cpp`, `InventoryMenu.cpp`, `Slot.cpp`

### C1. 物品系统
- [x] `Item` / `ItemStack`（id + count + 每物品 max_stack），`items.h` / `inventory.h`
- [x] 物品注册表（方块物品 id 映射 + 工具 Pickaxe/Axe/Shovel/Sword × 木/石/铁/钻 + 食物/材料）
- [~] 物品掉落实体 `ItemEntity`（当前挖掘直接进背包；地面掉落实体留待 D 阶段）

### C2. 背包与容器
- [x] `Inventory`（36 格：9 hotbar + 27 主仓），按物品堆叠上限合并（`inventory.h`）
- [x] 快捷栏 hotbar + 选中槽（数字键 1-9 + 滚轮切换）
- [~] 容器基类 `Container` / `Slot`（合成在 mode 内实现；通用容器抽象待 F 阶段）
- [ ] **网络：** `ContainerSetContentPacket` 等（服务器权威库存，待 N 阶段）

### C3. 合成
- [x] 配方系统 `RecipeBook`（shaped 偏移容错 + shapeless 多重集匹配），`crafting.h`
- [x] 2x2 玩家合成 + 3x3 工作台（`try_craft(grid,gw,gh)` 支持两种网格）
- [x] 熔炼配方 `smelt()` + 燃料 `is_fuel()`（矿石→锭、沙→玻璃、圆石→石、生猪排→熟）
- [x] 全套工具配方 + 木板/木棍/工作台/熔炉/火把配方

**C 阶段自测：** 2025-xx，`test_sdfcraft.exe` 全部通过（合成原木→木板、木板→木棍、
2x2工作台、3x3木镐、熔炼铁锭/玻璃、燃料判定、背包堆叠上限、世界种子确定性、方块编辑往返）。✅
工具挖掘加速 + 矿石按工具等级掉落已接入 `mode.h`。

---

## 阶段 D：实体 / 生物 / AI

> 参考 MC 实体与 AI 体系（大量文件）：`Entity.cpp`, `LivingEntity.cpp`, `Mob.cpp`,
> `PathfinderMob.cpp`, `GoalSelector.cpp`, `Goal.cpp` + 所有 `*Goal.cpp`,
> `PathFinder.cpp`, `PathNavigation.cpp`, `MobSpawner.cpp`

### D1. 实体基础
- [ ] `Entity` / `LivingEntity` / `Mob` 层级，移植核心更新循环
- [ ] 实体网络同步：`AddEntityPacket` / `AddMobPacket` / `RemoveEntitiesPacket` /
      `SetEntityDataPacket`（脏标记，移植 `SynchedEntityData`）
- [ ] 实体物理（重力/碰撞/击退），移植 `Entity::move`

### D2. AI 系统（Goal-based）
- [ ] `GoalSelector` + `Goal` 优先级调度，移植 `GoalSelector.cpp`
- [ ] 移植全部 Goal：`MeleeAttackGoal`, `RandomStrollGoal`, `LookAtPlayerGoal`,
      `PanicGoal`, `FloatGoal`, `BreedGoal`, `TemptGoal`, `AvoidPlayerGoal`,
      `NearestAttackableTargetGoal`, `HurtByTargetGoal`, `FleeSunGoal`, ... （全表见 D5）
- [ ] 寻路 A*（`PathFinder.cpp` / `Node.cpp` / `BinaryHeap.cpp` / `Path.cpp`）
- [ ] 导航 `PathNavigation` / 移动控制 `MoveControl` / `LookControl` / `JumpControl`

### D3. 被动生物（Animal）
- [ ] `Pig, Cow, MushroomCow, Sheep, Chicken, Ozelot/Ocelot, Wolf, Horse, Squid, Bat`
- [ ] 繁殖 `BreedGoal` / `MakeLoveGoal`、驯服 `TamableAnimal`、骑乘 `EntityHorse`

### D4. 敌对生物（Monster）
- [ ] `Zombie, Skeleton, Creeper, Spider, CaveSpider, EnderMan, Silverfish,
      Slime, LavaSlime(MagmaCube), Blaze, Ghast, PigZombie, Witch, Giant, Snowman, VillagerGolem`
- [ ] 攻击/特殊行为（爬行者爆炸 `SwellGoal` + `Explosion.cpp`，骷髅射箭 `ArrowAttackGoal`，
      末影人传送 + 搬方块，蜘蛛爬墙，史莱姆分裂）

### D5. 刷怪机制
- [ ] `MobSpawner`（自然刷怪，按光照/生物群系/难度），移植 `MobSpawner.cpp`
- [ ] 刷怪笼 `BaseMobSpawner` / `MobSpawnerTile`
- [ ] 难度系统 `Difficulty.h`

**D 阶段自测：** 世界刷出动物和怪物，夜晚僵尸追玩家，爬行者靠近爆炸，狼可驯服。

---

## 阶段 E：战斗 / 生存数值

> 参考 MC：`DamageSource.cpp`, `CombatTracker.cpp`, `Abilities.cpp`, `FoodData.cpp`,
> `Attribute.cpp`, `SharedMonsterAttributes.cpp`, `MobEffect.cpp`

### E1. 战斗
- [ ] 近战攻击（伤害 + 击退 + 暴击 + 冷却），移植 `Player::attack` / `LivingEntity::hurt`
- [ ] 伤害来源 `DamageSource`（摔落/火/岩浆/溺水/窒息/爆炸/虚空/实体）
- [ ] 远程：弓箭 `Arrow` / `BowItem`、雪球、鸡蛋、末影珍珠、三叉戟（如有）
- [ ] 护甲减伤 `ArmorItem` + 装备槽
- [ ] 网络：`AnimatePacket` / `EntityEventPacket` / `SetHealthPacket`

### E2. 生存数值
- [ ] 生命值 + 自然回血、属性系统 `Attribute` / `AttributeInstance`
- [ ] 饥饿度 `FoodData`（饱食/饱和度/疲劳），吃食物 `FoodItem`
- [ ] 经验 `ExperienceOrb` / `SetExperiencePacket`、等级
- [ ] 窒息/溺水/氧气、摔落伤害
- [ ] 死亡/重生 `RespawnPacket`、床设置重生点 `BedTile`

---

## 阶段 F：方块功能与方块实体

> 参考 MC：所有 `*Tile.cpp` + `*TileEntity.cpp`，`BlockSource`, `LevelChunk`

### F1. 方块大全（移植全部 Tile 类型）
- [ ] 基础：石/土/草/沙/砾/原木/木板/玻璃/羊毛/砖/黑曜石/萤石/冰...
- [ ] 功能方块：箱子/熔炉/工作台/附魔台/酿造台/信标/漏斗/发射器/投掷器/活塞...
- [ ] 自然方块：矿石/树叶/仙人掌/甘蔗/蘑菇/花/草丛/睡莲...
- [ ] 农业：耕地 `FarmTile`、作物 `CropTile/CarrotTile/PotatoTile`、南瓜/西瓜茎 `StemTile`
- [ ] 特殊：门/活板门/栅栏/栅栏门/楼梯/台阶/床/告示牌/梯子/火把/TNT/南瓜灯

### F2. 方块实体（TileEntity）
- [ ] 容器类：`ChestTileEntity, FurnaceTileEntity, DispenserTileEntity, HopperTileEntity,
      BrewingStandTileEntity, BeaconTileEntity, EnderChestTileEntity`
- [ ] 功能类：`SignTileEntity, MobSpawnerTileEntity, NoteBlock, CommandBlockEntity,
      EnchantmentTableEntity, SkullTileEntity, ComparatorTileEntity, DaylightDetectorTileEntity`
- [ ] 网络：`TileEntityDataPacket` / `ContainerOpenPacket` / `ContainerSetContentPacket`
- [ ] 漏斗物品传输、发射器/投掷器行为 `DispenseItemBehavior`

### F3. 物理方块
- [ ] 流体 `LiquidTile`（水/岩浆，流动算法），移植 `LiquidTileDynamic.cpp`
- [ ] 重力方块 `FallingTile`（沙/砾）
- [ ] 火焰蔓延 `FireTile`、爆炸 `Explosion.cpp`（破坏 + 掉落 + 击退）
- [ ] 活塞 `PistonBaseTile` / `PistonMovingPiece`（推/拉方块）

**F 阶段自测：** 箱子存取物品、熔炉烧矿、农田种地收获、活塞推方块、水流扩散、TNT 爆炸。

---

## 阶段 G：红石系统 ⭐ 用户特别要求

> 参考 MC：`Redstone.cpp`, `RedStoneDustTile.cpp`, `DiodeTile.cpp`, `RepeaterTile.cpp`,
> `ComparatorTile.cpp`, `NotGateTile.cpp`(红石火把), `LeverTile.cpp`, `ButtonTile.cpp`,
> `PressurePlateTile.cpp`, `WeightedPressurePlateTile.cpp`, `DetectorRailTile.cpp`,
> `TripWireTile.cpp`, `DaylightDetectorTile.cpp`, `PistonBaseTile.cpp`,
> `DispenserTile.cpp`, `NoteBlockTile.cpp`, `RailTile.cpp`, `PoweredRailTile.cpp`

### G1. 红石信号传播
- [ ] 红石粉 `RedStoneDustTile`（信号强度 0-15 衰减传播）
- [ ] 信号更新调度（`Level::updateNeighbors` / `tile updates`），移植 `Redstone.cpp`
- [ ] 强充能 / 弱充能区分

### G2. 红石元件
- [ ] 电源：红石火把 `NotGateTile`、拉杆 `LeverTile`、按钮 `ButtonTile`、
      压力板 `PressurePlateTile/WeightedPressurePlateTile`、绊线 `TripWireTile`、
      阳光传感器 `DaylightDetectorTile`、红石块
- [ ] 逻辑：中继器 `RepeaterTile`（延迟/锁存）、比较器 `ComparatorTile`（信号比较/容器读数）
- [ ] 执行：活塞/粘性活塞、发射器/投掷器、TNT 点燃、门/活板门、音符盒 `NoteBlockTile`、
      漏斗启用/禁用、铁轨 `PoweredRailTile/DetectorRailTile`、红石灯
- [ ] **网络：** 红石状态变更走方块更新 `TileUpdatePacket` / `BlockRegionUpdatePacket`，
      服务器权威计算，广播结果（红石逻辑只在服务器跑）

**G 阶段自测：** 拉杆→红石粉→活塞门、按钮→发射器、中继器延时、比较器读箱子、多人看到同步。

---

## 阶段 H：维度系统（魔影世界 / 末地 / 天空岛）⭐ 用户特别要求

> "魔影世界" = 下界/地狱（Nether）。参考 MC：`Dimension.cpp`, `HellDimension.cpp`,
> `NormalDimension.h`, `TheEndDimension.cpp`, `SkyIslandDimension.cpp`,
> `PortalTile.cpp`, `PortalForcer.cpp`, `HellPortalFeature.cpp`

### H1. 维度框架
- [ ] `Dimension` 抽象（独立世界 + 独立区块加载 + 独立生成器）
- [ ] 维度切换（实体跨维度传送），移植 `PortalForcer.cpp`
- [ ] 每维度独立的 SDF/方块世界实例 + 独立网络区块流
- [ ] **网络：** `RespawnPacket`(切换维度) + 客户端重建世界

### H2. 下界（魔影世界 / Nether）
- [ ] `HellDimension` + `HellRandomLevelSource`（地狱地形生成）
- [ ] 地狱方块：地狱岩 `NetherrackTile`、灵魂沙 `SoulSandTile`、地狱砖、萤石 `LightGemTile`、
      地狱石英矿、岩浆海
- [ ] 下界传送门 `PortalTile`（黑曜石框 + 打火石点燃 → 紫色传送门）
- [ ] 下界生物：恶魂 `Ghast`、僵尸猪人 `PigZombie`、烈焰人 `Blaze`、岩浆怪 `LavaSlime`
- [ ] 下界要塞 `NetherBridgeFeature` + 刷怪笼、下界疣 `NetherWartTile`

### H3. 末地（The End）
- [ ] `TheEndDimension` + `TheEndLevelRandomLevelSource`
- [ ] 末地石、黑曜石柱 `SpikeFeature` + 末影水晶 `EnderCrystal`
- [ ] 末地传送门 `TheEndPortal` / `TheEndPortalFrameTile`（末影之眼激活）
- [ ] 返回传送门 `EndPodiumFeature`

### H4. 天空岛（Sky Island，主机版特有）
- [ ] `SkyIslandDimension`（如适用）

**H 阶段自测：** 建黑曜石门点燃进下界，地形/怪物正确，多人同步；末地传送进末地。

---

## 阶段 I：Boss（末影龙 / 凋灵）⭐ 用户特别要求

> 参考 MC：`EnderDragon.cpp`, `BossMob.cpp`, `BossMobPart.cpp`, `MultiEntityMob`,
> `WitherBoss.cpp`, `DragonFireball.cpp`, `WitherSkull.cpp`, `EnderCrystal.cpp`

### I1. 末影龙
- [ ] `EnderDragon` 多部位实体（`BossMobPart` 身体段碰撞）
- [ ] 飞行路径节点 AI、攻击模式、被末影水晶治疗
- [ ] 击杀 → 经验喷泉 + 返回传送门 + 龙蛋
- [ ] Boss 血条 UI（`UpdateProgressPacket` / Boss bar 网络同步）

### I2. 凋灵
- [ ] `WitherBoss` 三头多部位、`WitherSkull` 弹射物、破坏方块、护盾阶段
- [ ] 召唤（灵魂沙 T 形 + 3 凋零骷髅头）

### I3. Boss 网络同步
- [ ] 多部位实体同步、Boss 血条广播、Boss 战范围内所有玩家可见

**I 阶段自测：** 末地召出末影龙并击杀拿龙蛋；下界/主世界召凋灵击杀，多人同屏 Boss 血条。

---

## 阶段 J：世界生成（生物群系 / 结构 / 矿物 / 洞穴）

> 参考 MC：`RandomLevelSource.cpp`, `Biome.cpp` + 所有 `*Biome.cpp`, `BiomeSource.cpp`,
> 所有 `*Layer.cpp`(生物群系层), `*Feature.cpp`(结构), `OreFeature.cpp`,
> `CaveFeature.cpp`, `CanyonFeature.cpp`, `MineShaftFeature.cpp`, `VillageFeature.cpp`,
> `StrongholdFeature.cpp`, `DungeonFeature.cpp`, `PerlinNoise.cpp`, `SimplexNoise.cpp`

### J1. 噪声与地形生成（适配 SDF + 无尽）
- [ ] 移植 MC 噪声 `PerlinNoise/ImprovedNoise/SimplexNoise/PerlinSimplexNoise`
- [ ] 把 MC 的方块高度生成转为 **SDF 密度场**（自然地形）+ 方块矿物层
- [ ] per-chunk 惰性生成（无尽世界核心）

### J2. 生物群系
- [ ] 移植生物群系层链 `*Layer.cpp`（GenLayer 体系）
- [ ] 全部群系：平原/森林/沙漠/丛林/雪原/沼泽/海洋/河流/极地山地/蘑菇岛/针叶林/雨林/海滩...
- [ ] 群系装饰 `BiomeDecorator`（树/花/草/矿/湖）

### J3. 结构与特征
- [ ] 矿物 `OreFeature`、洞穴 `CaveFeature`、峡谷 `CanyonFeature`
- [ ] 树 `TreeFeature` 全变种、湖 `LakeFeature`、地牢 `DungeonFeature`
- [ ] 废弃矿井 `MineShaftFeature`、要塞 `StrongholdFeature`、村庄 `VillageFeature`、
      沙漠神殿/丛林神庙 `ScatteredFeature`、女巫小屋

**J 阶段自测：** 无尽世界探索，群系过渡自然，挖到分层矿物，发现洞穴/矿井/村庄。

## 阶段 K：附魔 / 药水 / 状态效果

> 参考 MC：`Enchantment.cpp` + 所有 `*Enchantment.cpp`, `EnchantmentHelper.cpp`,
> `EnchantmentMenu.cpp` / `EnchantmentTableTile.cpp` / `EnchantmentTableEntity.cpp`,
> `EnchantedBookItem.cpp`, `MobEffect.cpp` + 所有 `*MobEffect.cpp`, `MobEffectInstance.cpp`,
> `PotionItem.cpp`, `PotionContents`, `BrewingStandTile.cpp` / `BrewingStandTileEntity.cpp`,
> `BrewingStandMenu.cpp`, `SplashPotionItem`, `AnvilMenu.cpp`

### K1. 附魔系统
- [ ] 附魔注册表 `Enchantment` + 全部附魔：
      `DamageEnchantment`(锋利/亢奋/制裁), `KnockbackEnchantment`(击退),
      `FireAspectEnchantment`(火焰附加), `LootBonusEnchantment`(抢夺/时运),
      `DigDurabilityEnchantment`(耐久), `DiggingEnchantment`(效率),
      `ArrowDamageEnchantment`(力量), `ArrowFireEnchantment`(火矢),
      `ArrowKnockbackEnchantment`(冲击), `ArrowInfiniteEnchantment`(无限),
      `ProtectionEnchantment`(保护/摔落/火焰/爆炸/弹射物), `WaterWorkerEnchantment`(水下挖掘),
      `WaterWalkerEnchantment`(深海探索者), `UntouchingEnchantment`(精准采集),
      `AquaAffinityEnchantment`, `ThornsEnchantment`(荆棘), `BindingCurse`/`VanishingCurse`(如有)
- [ ] 附魔台 `EnchantmentTableTile` + `EnchantmentMenu`（经验等级 + 青金石 + 书架加成）
- [ ] `EnchantmentHelper`（随机附魔、附魔等级计算、附魔书 `EnchantedBookItem`）
- [ ] 铁砧 `AnvilMenu`（合并附魔、改名、修复耐久、经验消耗）
- [ ] **网络：** 附魔结果走容器同步 `ContainerSetContentPacket`，经验走 `SetExperiencePacket`

### K2. 药水与酿造
- [ ] 状态效果注册表 `MobEffect` + 全部效果：
      速度/缓慢/急迫/挖掘疲劳/力量/瞬间伤害/瞬间治疗/跳跃提升/反胃/再生/抗性提升/
      防火/水下呼吸/隐身/失明/夜视/饥饿/虚弱/中毒/凋零/生命提升 `HealthBoostMobEffect`/
      伤害吸收 `AbsoptionMobEffect`/饱和
- [ ] `MobEffectInstance`（等级 + 时长 + 粒子），瞬时效果 `InstantenousMobEffect`
- [ ] 药水物品 `PotionItem` + 药水内容（基础/二级/延长），喷溅药水 `SplashPotionItem`
- [ ] 酿造台 `BrewingStandTile` / `BrewingStandTileEntity` / `BrewingStandMenu`
      （地狱疣基底 + 各类材料 + 烈焰粉燃料）
- [ ] **网络：** 效果走 `MobEffectPacket` / `RemoveMobEffectPacket`，酿造走容器同步

### K3. 信标 / 末影宝箱（关联系统）
- [ ] 信标 `BeaconTile` / `BeaconMenu`（金字塔结构 + 区域增益效果）
- [ ] 末影宝箱 `EnderChestTile`（玩家专属共享存储 `PlayerEnderChestContainer`）

**K 阶段自测：** 附魔台附魔工具、铁砧合书、酿造速度药水、喝下后状态栏显示并生效、多人同步。

---

## 阶段 L：村民 / 交易 / 村庄

> 参考 MC：`Villager.cpp`, `Merchant.cpp` / `MerchantMenu.cpp` / `MerchantRecipe.cpp` /
> `MerchantRecipeList.cpp` / `TradeItemPacket.cpp`, `Village.cpp` / `Villages.cpp` /
> `VillageSiege.cpp`, `VillagerGolem.cpp`(铁傀儡), `VillageFeature.cpp` / `VillagePieces.cpp`,
> `TradeWithPlayerGoal.cpp`, `MoveThroughVillageGoal.cpp`, `DefendVillageTargetGoal.cpp`

### L1. 村民实体
- [ ] `Villager`（职业：农民/图书管理员/牧师/铁匠/屠夫 等），AgableMob 繁殖
- [ ] 交易系统 `Merchant` / `MerchantRecipe` / `MerchantRecipeList`（绿宝石经济，等级解锁）
- [ ] 交易 UI `MerchantMenu` + `MerchantResultSlot`，**网络** `TradeItemPacket`
- [ ] 村民 AI：`TradeWithPlayerGoal`, `MoveThroughVillageGoal`, 工作/作息

### L2. 村庄结构与机制
- [ ] 村庄结构生成 `VillageFeature` / `VillagePieces`（房屋/农田/路/水井/铁匠铺）
- [ ] 村庄数据 `Village` / `Villages`（边界/声望/门统计）
- [ ] 铁傀儡 `VillagerGolem` 自然生成 + 守护，`DefendVillageTargetGoal`
- [ ] 僵尸围城 `VillageSiege`、僵尸感染村民 / 治疗（金苹果 + 虚弱）

**L 阶段自测：** 世界生成村庄、与村民交易绿宝石、铁傀儡防御、夜晚僵尸袭村、多人同步。

---

## 阶段 M：持久化存档

> 参考 MC：`LevelStorage.cpp` / `LevelStorageSource.cpp`, `RegionFile.cpp` /
> `RegionFileCache.cpp` / `McRegionChunkStorage.cpp`, `ChunkStorage.cpp`,
> `CompoundTag.cpp` / `NbtIo.cpp`(NBT 序列化), `LevelData.cpp`, `SavedData.cpp` /
> `SavedDataStorage.cpp`, `DirectoryLevelStorage.cpp`
> MassRTS 已有：`src/net/persistence/region_file.h`, `world_storage.h`

### M1. 区块存档（无尽世界核心）
- [ ] 复用/对接 `src/net/persistence/region_file.h`（已有 .mca 风格 region 文件）
- [ ] **只存修改 Delta**：自然层从种子重建，仅持久化方块放置/挖掘差异 + 方块实体
- [ ] 区块加载/卸载时的脏区块落盘（`McRegionChunkStorage` 思路）
- [ ] 每维度独立存档目录（主世界 / 下界 / 末地）

### M2. 世界元数据与实体存档
- [ ] 世界级数据 `LevelData`（种子/时间/出生点/游戏规则/天气/难度）
- [ ] 实体与方块实体序列化（NBT 风格 `CompoundTag` / `NbtIo` 或自定义二进制）
- [ ] 玩家数据存档（背包/位置/血量/经验/效果/重生点），dedicated 下按 UUID
- [ ] `SavedData`（村庄/要塞/地图/刷怪进度等全局数据）

### M3. 存档兼容与安全
- [ ] 存档版本号 + 向后兼容迁移
- [ ] 自动保存（定时 + 退出时）、崩溃安全（写临时文件再原子替换）

**M 阶段自测：** 建造后退出再进入，世界/背包/实体/维度状态完整保留；dedicated 重启后世界一致。

---

## 阶段 N：网络完整化（专用服 + 兴趣管理 + 反作弊）

> 参考 MC：`net.minecraft.server` 体系、`PlayerList`, `ServerPlayer`, `ServerLevel`,
> `Connection`/`PacketListener`, 以及 MassRTS `src/net/voxel/voxel_net_engine.*`,
> `src/net/core/*`（connection / compression / file_transfer / nat_traversal）
> 总体策略见 `docs/DUAL_MODE_NETWORK.md`（按玩家数选同步策略）。

### N1. 服务器架构完善
- [ ] Dedicated server 主循环（20Hz tick，无渲染），配置文件（端口/最大玩家/视距/难度/白名单）
- [ ] Listen server（主机内嵌服务 + 本地玩家零延迟路径）
- [ ] 玩家会话管理 `PlayerList`（加入/退出/超时 `MAX_TICKS_WITHOUT_INPUT`/踢出/封禁）
- [ ] 控制台命令（参考 `Minecraft.Server/Console/commands`：op/kick/ban/whitelist/save/stop/tp/give/time/weather/difficulty/gamemode）

### N2. 服务器权威与同步策略
- [ ] 所有玩法逻辑（方块编辑/红石/实体/战斗/库存/合成）**只在服务器跑**，客户端预测 + 校正
- [ ] 兴趣管理（AOI）：按玩家位置只推送可见区块/实体（`VoxelChunkVisibility`，完善 `serverUpdatePlayerView`）
- [ ] 实体同步：脏标记 `SynchedEntityData` + 增量移动包（`MoveEntityPacketSmall` 位打包）+ 快照插值
- [ ] 方块编辑批量广播 `VoxelEditBatch` / `BlockRegionUpdatePacket`，区块压缩传输（已有 zlib + file_transfer）
- [ ] 按玩家数选策略（见 DUAL_MODE_NETWORK）：少人即时全量 / 多人 AOI 分级

### N3. 反作弊与健壮性
- [ ] 服务器校验：动作距离 / 速度 / 挖掘时间 / 库存合法性 / 放置权限，拒绝则回滚客户端
- [ ] 丢包/乱序处理（已有 `Connection` 可靠层），重连
- [ ] NAT 穿透 / 直连 / 局域网发现（已有 `nat_traversal.h`）
- [ ] 带宽预算与限流，防泛洪

**N 阶段自测：** 多客户端连专用服长时间游玩稳定；高延迟下预测平滑；伪造动作被服务器拒绝并回滚。

---

## 阶段 O：UI / HUD / 音效 / 打磨

> 参考 MC：`Gui.cpp`, `InventoryScreen`, 各 `*Screen`/`*Menu` 客户端 UI，
> `SoundEngine` / `SoundEvent`, `ParticleEngine`, MassRTS `src/ui/hud.h` / `menu.h` /
> `src/audio/audio_system.h` / `src/render/particles.h`

### O1. HUD
- [ ] 血条 / 护甲条 / 饥饿条 / 经验条 / 氧气条 / Boss 血条 / 物品栏 hotbar + 选中槽
- [ ] 准星 + 选中方块高亮框、手持物品/手臂渲染、状态效果图标、屏幕红屏/受伤反馈

### O2. 菜单与界面
- [ ] 物品栏界面（拖拽/合成格/装备）、各容器界面（箱子/熔炉/工作台/附魔/酿造/铁砧/信标/交易）
- [ ] 暂停菜单、设置（视距/音量/键位/视角）、死亡界面、世界选择/创建/多人列表
- [ ] 创造模式物品栏（分类标签 + 搜索）

### O3. 音效与粒子
- [ ] 方块破坏/放置/脚步（按材质）、实体音效、环境音、UI 音效（接入 `audio_system.h`）
- [ ] 粒子：破坏方块碎屑、爆炸、火焰/烟、药水、附魔台、传送门、下雨/下雪、暴击（接入 `particles.h`）

### O4. 环境与打磨
- [ ] 昼夜循环 + 太阳/月亮/星空、动态光照（方块光 + 天空光传播）、天气（雨/雪/雷暴）
- [ ] 雾 / 水下视觉 / 下界与末地天空盒、流畅度与性能优化（区块网格缓存/视锥剔除/LOD）

**O 阶段自测：** 完整 HUD 与所有界面可用，音效粒子到位，昼夜天气光照正常，整体手感接近原版。

---

## 附录：移植映射速查（MinecraftConsoles → SDFCraft）

| MC 概念 | MC 文件 | SDFCraft 落点 |
|---------|---------|---------------|
| 方块 `Tile` | `Tile.*` / `*Tile.*` | `sdfcraft::Block` + `BlockLayer`（uint8 per voxel） |
| 方块实体 | `*TileEntity.*` | `sdfcraft::BlockEntity`（坐标索引存储 + 网络同步） |
| 物品 | `Item.*` / `ItemInstance.*` | `sdfcraft::Item` / `ItemStack` |
| 实体/生物 | `Entity/LivingEntity/Mob.*` | `sdfcraft::Entity` 层级（独立于 RTS ECS） |
| AI | `Goal/GoalSelector/PathFinder.*` | `sdfcraft::ai`（Goal 调度 + A* 寻路） |
| 世界生成 | `RandomLevelSource/*Layer/*Feature.*` | `sdfcraft::worldgen`（输出 SDF 密度场 + 方块层） |
| 维度 | `Dimension/HellDimension/TheEnd*.*` | `sdfcraft::Dimension`（独立世界实例） |
| 存档 | `RegionFile/ChunkStorage/CompoundTag.*` | 复用 `src/net/persistence/*` + Delta 存储 |
| 网络 | `*Packet.*` / server 体系 | `src/net/voxel/voxel_net_engine.*` + 新增包 |
| 角色模型 | `HumanoidModel` / `char.png` | 程序化立方体人形 或 `SkinnedModel` 重建 + MC 皮肤 |

## 测试与打勾规范
- 每个子项 `[ ]` 完成且**自测通过**后改为 `[x]`，并在该阶段末尾补一行测试结论（日期 + 结果）。
- 阶段总览（第 0 节）在该阶段全部子项打勾后再勾。
- 自测优先用最小可复现实验（单机起 listen server + 第二客户端 loopback 连入）验证同步。

> 计划完毕。下一步：从 **阶段 A → 阶段 B（最小可玩循环）** 开始实现，逐项打勾。





