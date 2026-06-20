# SDFCraft 当前状态与开发路线图

**更新日期**: 2025-01-XX  
**基准文档**: `SDF_MINECRAFT_PORT_PLAN.md`  
**知识图谱**: G-CMakePJ-MassRTS (8343 nodes, 23186 edges)

---

## 一、当前存在的核心问题

> **更新（本次会话）**: 下方三个 🔴 紧急 bug 已全部修复并通过 `test_sdfcraft` +
> 离屏渲染验证。详见每条的「✅ 已修复」说明与「六、立即行动清单」。

### ✅ 紧急问题（已修复）

#### 1. **深层挖掘时多边形撕裂（Marching Cubes Artifact）** — ✅ 已修复
- **现象**: 挖到深层（y < 20）时出现诡异的三角形和撕裂
- **真正根因**（本次定位）:
  - `World::analytic_field_cached()` 用 `_hcache` 的中心差分求地表梯度。
  - `HeightCache::get()` 对越界坐标返回 `0.0`，被当成「高度=0」。
  - 在预热高度缓存区域的**边缘**，梯度 `gx=(0-h)/2` 直接爆炸，
    把假的等值面交叉推到 chunk 接缝上 → 接缝撕裂。
- **修复方案**:
  - `HeightCache::get()` 越界返回哨兵 `MISS=-1e30`。
  - `analytic_field_cached()` 在中心或任一梯度邻居 miss 时，
    回退到精确的 `analytic_field()`，保证跨 chunk 表面连续。
- **代码**: `src/sdfcraft/world.h::HeightCache` / `analytic_field_cached()`

#### 2. **相机抖动（行走时上下颠簸）** — ✅ 已修复
- **现象**: 走路时视角不停上下抖动
- **真正根因**（本次定位）:
  - `Player::move_axis()` 的台阶「落回」循环以 0.05m 的粗步进量化 `pos.y`。
  - 走在平滑但永不绝对水平的 FBM 地表上时，落点高度每帧在 5cm 网格上跳变，
    平滑目标本身在振荡 → 相机上下抖。
- **修复方案**:
  - 用 12 次二分搜索求精确接触高度，落点连续、逐帧稳定。
  - 配合把 `smooth_y_` rate 从 12 提到 16，更跟手。
- **代码**: `src/sdfcraft/player.h::move_axis()`

#### 3. **地形颜色不理想 / 不统一** — ✅ 已修复
- **现象**: 地面颜色错乱、不统一、像没打光
- **真正根因**（本次定位）:
  - `shaders/sdfcraft_chunk.frag` 的 `main()` 被一次未提交编辑改坏：
    `micro/meso` 重复声明（GLSL 编译错误）、引用了未定义的 `base`、
    且**从未调用 `palette()`**。shader 编译失败 → 地表渲染成乱色/无光照。
- **修复方案**:
  - 重写 `main()`: 先 `base = palette(...)` 解析地表色，
    再统一明亮日光打光（ambient 0.55 + 暖阳 + 冷天光反弹）。
- **代码**: `shaders/sdfcraft_chunk.frag::main()`

---

## 二、已完成功能（✅）

### 基础系统
- ✅ **SDF地形生成** - analytic_field + FBM噪声
- ✅ **Marching Cubes meshing** - 平滑地形mesh
- ✅ **方块系统** - GRASS/DIRT/STONE/SAND/SNOW/LOG/LEAVES/ORE
- ✅ **玩家物理** - 行走/跳跃/飞行/碰撞检测
- ✅ **视角控制** - 第一人称相机，raycast瞄准
- ✅ **Inventory系统** - 9格hotbar + 背包
- ✅ **挖掘/放置** - 左键挖掘（支持SDF smooth carve），右键放置

### 地形编辑工具（新增）
- ✅ **carve_sphere** - 球形挖掘/填充
- ✅ **raise_terrain** - 堆山（push地形向上）
- ✅ **smooth_terrain** - 平滑地形（3x3x3卷积核）
- ✅ **flatten_terrain** - 平整为指定高度
- ✅ **boxify_terrain** - 将圆形洞穴变成方形房间

### 生存元素（部分）
- ✅ **树木系统** - 2x2粗壮树干，带树枝
- ✅ **树木物理倒塌** - 砍断底部整棵倒塌，收集掉落物
- ✅ **工具系统** - 镐/斧/铲，tier分级，挖掘速度差异
- ✅ **合成系统** - 基础crafting recipes

### 渲染
- ✅ **Chunk meshing** - 分块渲染，dirty标记
- ✅ **光照系统** - 环境光+太阳光+天空反射
- ✅ **材质着色** - triplanar mapping，procedural纹理
- ✅ **行星背景** - 无缝地平线，地球曲率

---

## 三、部分完成/需修复功能（⚠️）

### 地形编辑工具绑定
- ⚠️ **按键绑定不完整**:
  - `FrameInput`已添加`key_1-5, key_r_down/up, key_t_down/up`
  - `mode.h`已添加`SculptMode`枚举和切换逻辑
  - ❌ `main_sdfcraft.cpp`绑定未完成：
    - Ctrl+1-5切换模式
    - [ / ]调整半径
    - - / =调整强度
  - ❌ 缺少UI提示（当前模式/半径/强度显示）

### 网络同步
- ⚠️ **NetWorldOps**已实现，但terrain sculpting工具未同步
  - `carveSphere`已有网络支持
  - `raise/smooth/flatten/boxify`没有网络接口

### 保存/加载
- ⚠️ Chunk保存只支持blocks，SDF field未持久化
  - `dirty_save`标记存在，但save路径未实现

---

## 四、未实现功能（按SDF_MINECRAFT_PORT_PLAN.md）

### Phase 1: 核心生存玩法（0-2周）
| 功能 | 状态 | 优先级 | 工作量 |
|------|------|--------|--------|
| **饥饿系统** | ❌ 未开始 | P0 | 2天 |
| **食物种类** | ❌ 未开始 | P0 | 3天 |
| **生命值UI** | ❌ 未开始 | P0 | 1天 |
| **怪物AI** | ❌ 未开始 | P0 | 5天 |
| **昼夜循环** | ❌ 未开始 | P1 | 2天 |
| **床与重生** | ❌ 未开始 | P1 | 2天 |
| **合成台GUI** | ⚠️ 基础实现 | P0 | 3天 |
| **熔炉** | ❌ 未开始 | P1 | 3天 |
| **箱子** | ❌ 未开始 | P1 | 2天 |

### Phase 2: 建造与装饰（2-4周）
| 功能 | 状态 | 优先级 | 工作量 |
|------|------|--------|--------|
| **门/栅栏/楼梯** | ❌ 未开始 | P1 | 4天 |
| **玻璃/窗户** | ❌ 未开始 | P2 | 2天 |
| **火把/照明** | ❌ 未开始 | P1 | 3天 |
| **红石系统** | ❌ 未开始 | P3 | 10天 |
| **活塞/机关** | ❌ 未开始 | P3 | 5天 |

### Phase 3: 世界生成（4-6周）
| 功能 | 状态 | 优先级 | 工作量 |
|------|------|--------|--------|
| **Biome系统** | ⚠️ 简单实现 | P1 | 5天 |
| **洞穴生成** | ❌ 未开始 | P1 | 4天 |
| **矿脉分布** | ⚠️ 基础ore | P1 | 3天 |
| **村庄生成** | ❌ 未开始 | P2 | 7天 |
| **地牢/要塞** | ❌ 未开始 | P3 | 10天 |

### Phase 4: 高级内容（6-8周）
| 功能 | 状态 | 优先级 | 工作量 |
|------|------|--------|--------|
| **末地/下界** | ❌ 未开始 | P3 | 15天 |
| **Boss战** | ❌ 未开始 | P3 | 7天 |
| **附魔系统** | ❌ 未开始 | P3 | 5天 |
| **药水酿造** | ❌ 未开始 | P3 | 4天 |

### Phase 5: 性能与规模（8-10周）
| 功能 | 状态 | 优先级 | 工作量 |
|------|------|--------|--------|
| **八叉树LOD** | ❌ 未开始 | P0 | 10天 |
| **Chunk流式加载** | ⚠️ 基础实现 | P0 | 5天 |
| **视锥剔除** | ❌ 未开始 | P1 | 3天 |
| **并行meshing** | ❌ 未开始 | P1 | 5天 |
| **地球规模测试** | ❌ 未开始 | P2 | 7天 |

---

## 五、架构问题与技术债

### 1. **SDF vs Blocks 混合架构**
- **现状**: 地形用SDF，树木/方块用blocks
- **问题**: 
  - 两套系统，复杂度高
  - blocks不能嵌入SDF地形（已修复：do_place支持）
  - meshing有两套（MCMesher + Mesher）
- **长期方案**: 统一为SDF？或明确分层（地形=SDF，建筑=blocks）

### 2. **Chunk管理不完善**
- **问题**:
  - 无chunk卸载机制（内存泄漏风险）
  - 无预加载/异步生成
  - dirty标记过于粗暴（整个chunk重mesh）
- **需要**: 
  - LRU chunk cache
  - 后台线程生成/mesh
  - 增量mesh更新

### 3. **网络同步架构不完整**
- **现状**: `NetWorldOps`存在但功能有限
- **问题**:
  - 只同步block set/carve，不同步其他sculpt操作
  - 无客户端预测/服务器校正
  - 无带宽优化（每次carve都发送完整参数）

### 4. **UI系统缺失**
- **现状**: 只有debug overlay
- **需要**:
  - Inventory GUI（9x4网格）
  - Crafting GUI（3x3）
  - 生命值/饥饿条
  - 快捷栏高亮
  - 准星/瞄准提示

---

## 六、立即行动清单（本周）

### ✅ 已完成（本次会话）

#### 1. **修复相机抖动** — ✅ 完成
- `Player::move_axis()` 台阶落回改为 12 次二分搜索（连续接触高度）
- `smooth_y_` rate 12 → 16
- 已通过离屏行走渲染验证

#### 2. **修复多边形撕裂** — ✅ 完成
- `HeightCache::get()` 越界返回 `MISS`；`analytic_field_cached()` miss 时回退精确场
- `test_sdfcraft` 的 `DIG MESH: long_edges=0` 通过

#### 3. **修复地形颜色** — ✅ 完成
- 重写 `sdfcraft_chunk.frag::main()`，恢复 `palette()` 调用 + 统一明亮日光
- 离屏渲染确认地表统一受光、无乱色

### 🔥 下一步：HUD / UI 系统（最高优先）

> 当前游戏**完全没有任何屏幕 UI**：看不到血量、饥饿、准星、hotbar。
> 这是修完渲染 bug 后体验上的最大缺口，优先做。

#### HUD（建议 1-2 天）
- [ ] 十字准星（屏幕中心，简单两段线/纹理 quad）
- [ ] 生命值条（红心 x10，读 `Player::health`）
- [ ] 饥饿条（鸡腿 x10，读 `Player::hunger`）
- [ ] 氧气条（潜水时显示，读 `Player::air`）
- [ ] hotbar（9 格 + 选中高亮，读 `Inventory`）
- [ ] 方案选择：ImGui 集成 vs 自绘 2D 正交 quad（无新依赖）
- **入口**: `src/sdfcraft/mode.h` 渲染收尾处 + `chunk_renderer.h` 旁新建 `hud_renderer.h`

#### 紧随其后：接入生存+怪物循环
- [ ] 把 `Player::survival_tick()` 接进 `Mode::update`
- [ ] 把 `EntityManager`/`MobAI`（`entity.h`/`ai.h` 已写好）接进主循环
- [ ] 夜间在黑暗处生成怪物（注意 `is_night` 当前恒为 false，需昼夜系统）

#### 地形工具按键绑定（已大部分完成，待核对）
- [x] Ctrl+1-5 / `[` `]` / `-` `=` 绑定已在 `main_sdfcraft.cpp`
- [ ] 待 HUD 完成后加 debug overlay 显示当前模式/半径/强度

---


---

## 七、中期路线图（1-2个月）

### 月度1: 核心玩法完整（Week 1-4）
- Week 1: 饥饿/生命/怪物基础 ✅
- Week 2: 合成台/熔炉/箱子GUI
- Week 3: 昼夜/床/重生/火把
- Week 4: 基础建筑blocks（门/楼梯/栅栏）

**里程碑**: 可以"生存一晚上，建一个庇护所，烧制工具"

### 月度2: 世界内容扩展（Week 5-8）
- Week 5: Biome系统完善（沙漠/雪原/森林/海洋）
- Week 6: 洞穴生成（3D perlin worms）
- Week 7: 矿脉分布（钻石/铁/煤炭深度规则）
- Week 8: 村庄生成（简单房屋结构）

**里程碑**: 有探索价值的世界

---

## 八、长期目标（3-6个月）

### 1. **性能优化（支持地球规模）**
- 八叉树LOD (按SDF_MINECRAFT_PORT_PLAN.md的adaptive octree方案)
- 视锥剔除 + 遮挡剔除
- GPU-driven rendering
- 并行chunk生成/meshing

### 2. **高级内容**
- 下界/末地（异世界传送门）
- Boss战（末影龙/凋灵）
- 附魔/药水系统
- 红石电路

### 3. **多人联机**
- 完善NetWorldOps
- 客户端预测/插值
- 服务器权威架构
- 反外挂机制

---

## 九、风险与阻塞因素

### 技术风险
1. **Marching Cubes质量** - 可能需要切换到Dual Contouring或Transvoxel
2. **SDF性能** - 大规模世界下内存/计算开销
3. **Chunk streaming** - 无限世界的加载/卸载逻辑复杂

### 资源风险
1. **UI美术** - 当前只有程序UI，缺少美术资源
2. **音效/音乐** - 完全缺失
3. **模型/动画** - 玩家/怪物模型简陋

### 设计风险
1. **SDF vs Blocks** - 架构决策尚未最终确定
2. **玩法平衡** - 生存难度、资源分布需要大量测试
3. **性能目标** - 地球规模是否真的可行？

---

## 十、参考资料与知识图谱

### 关键代码位置（知识图谱verified）
```
核心系统:
- src/sdfcraft/world.h::World - 地形生成/SDF管理
- src/sdfcraft/player.h::Player - 玩家物理/控制
- src/sdfcraft/mode.h::Mode - 游戏主循环
- src/sdfcraft/inventory.h - 物品/背包系统
- src/sdfcraft/mc_mesher.h::MCMesher - Marching Cubes实现

地形编辑:
- World::carve_sphere (line 293-330)
- World::raise_terrain (line 419-447)
- World::smooth_terrain (line 449-496)
- World::flatten_terrain (line 498-527)
- World::boxify_terrain (line 529-560)

渲染:
- src/sdfcraft/renderer.h::Renderer - 渲染管线
- shaders/sdfcraft_chunk.frag - 地形着色器
- src/sdfcraft/mesher.h::Mesher - 方块mesher

网络:
- src/sdfcraft/net_ops.h::NetWorldOps - 网络同步
- src/sdfcraft/world_ops.h::WorldOps - 操作抽象层
```

### 外部参考
- MinecraftConsoles源码: `G:\CMakePJ\MinecraftConsoles`
- MassRTS的SDF地形: `src/render/sdf_terrain.h`
- SDF_MINECRAFT_PORT_PLAN.md - 主计划文档

---

## 附录：快速诊断命令

### 编译测试
```bash
cd G:/CMakePJ/MassRTS
cmake --build build --config Release --target SDFCraft
```

### 运行验证器
```bash
./build/Release/validate_sdfcraft.exe
```

### 检查知识图谱
```cypher
// 查找所有地形编辑相关函数
MATCH (f:Function|Method) 
WHERE f.qualified_name CONTAINS 'terrain' 
  OR f.qualified_name CONTAINS 'carve' 
  OR f.qualified_name CONTAINS 'sculpt'
RETURN f.qualified_name, f.file_path, f.start_line
ORDER BY f.file_path
```

### 当前统计（知识图谱）
- 总节点: 8343
- 总边: 23186
- SDFCraft相关函数: ~150个
- 已实现的MC功能: ~15%
- 完成度: **Phase 0.5 / 5.0**

---

**最后更新**: 请在每次重大改动后更新此文档
**负责人**: CloseCrab AI + 人类协作
**状态**: 🟡 原型阶段，核心bug待修复
