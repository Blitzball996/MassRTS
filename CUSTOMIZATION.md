# MassRTS 自定义指南

## 自定义音频

音频系统使用 [miniaudio](https://miniaud.io/) 引擎。音频文件放在项目目录下即可。

### 文件位置
```
MassRTS/
  assets/
    audio/
      bgm_menu.wav          - 主菜单背景音乐
      bgm_battle_calm.wav   - 战斗冷静期BGM
      bgm_battle_intense.wav- 激烈战斗BGM
      bgm_victory.wav       - 胜利音乐
      bgm_defeat.wav        - 失败音乐
      sfx_sword_hit.wav     - 近战攻击音效
      sfx_arrow_fire.wav    - 弓箭发射
      sfx_arrow_hit.wav     - 弓箭命中
      sfx_explosion.wav     - 爆炸(投弹手/火炮)
      sfx_nuke.wav          - 核弹音效
      sfx_cavalry_charge.wav- 骑兵冲锋
      sfx_march.wav         - 行军脚步
      sfx_horn_attack.wav   - 进攻号角(移动指令)
      sfx_horn_retreat.wav  - 撤退号角
      sfx_capture.wav       - 据点占领
      sfx_buy.wav           - 购买兵种
      sfx_coin.wav          - 金币获得
      amb_wind.wav          - 环境风声
      amb_birds.wav         - 鸟鸣
      amb_river.wav         - 河流声
```

### 音频格式要求
- 格式: **WAV** (推荐) 或 **MP3** / **FLAC**
- 采样率: 44100Hz 或 48000Hz
- BGM: 建议循环点用元数据标注(loop point)
- SFX: 尽量短(<2秒)，无头部静音

### 如何添加/替换音频

1. 将音频文件放入 `assets/audio/` 目录
2. 在 `src/audio/audio_system.h` 中找到对应的加载代码：

```cpp
// 示例：替换背景音乐
audio.load_bgm("assets/audio/bgm_battle_calm.wav");    // 平静
audio.load_bgm("assets/audio/bgm_battle_intense.wav"); // 激烈(>50%兵力交战)

// 示例：添加新音效
audio.load_sfx("my_custom_sound", "assets/audio/my_sound.wav");
// 在代码中播放:
audio.play_sfx("my_custom_sound", position, volume);
```

### 动态音乐切换逻辑
```cpp
// 在 main.cpp 游戏循环中:
float combat_ratio = (fighting_units) / total_units;
if (combat_ratio > 0.4f) 
    audio.crossfade_to("bgm_battle_intense", 2.0f); // 2秒渐变
else 
    audio.crossfade_to("bgm_battle_calm", 3.0f);
```

### 阵营语音
```
assets/audio/
  voice_red/
    move_1.wav    - "收到！"
    move_2.wav    - "移动中！" 
    attack_1.wav  - "进攻！"
    attack_2.wav  - "杀！"
    die_1.wav     - "啊！"
  voice_blue/
    move_1.wav    - 敌方移动声
    attack_1.wav  - 敌方攻击声
```

在代码中触发：
```cpp
// 当玩家下达移动命令时
audio.play_voice(Faction::Red, "move", position);
```

---

## 自定义模型

### 当前模型系统

模型在 `src/render/mesh_gen.h` 中用代码生成（Minecraft风格方块体）。每种兵种是一组 `AnimVertex` 组成的长方体拼接。

### 模型结构
```cpp
struct AnimVertex {
    float x, y, z;      // 顶点位置
    float nx, ny, nz;   // 法线
    float part_id;      // 部位ID (0=body, 1=head, 2=left_arm, 3=right_arm, 4=left_leg, 5=right_leg)
    float pivot_y;      // 动画旋转轴Y高度
};
```

### 如何修改模型

**方法1: 修改方块尺寸**（最简单）

编辑 `src/render/mesh_gen.h` 中对应兵种函数：
```cpp
static Mesh create_infantry() {
    // 参数: center_x, center_y, center_z, half_w, half_h, half_d, part_id, pivot_y
    add_box(verts, idx, 0, 14, 0,  2.5, 5, 1.5, 0, 14);  // 身体
    add_box(verts, idx, 0, 22, 0,  2, 2, 2, 1, 20);       // 头
    add_box(verts, idx, -4, 14, 0, 1, 5, 1, 2, 19);       // 左臂
    add_box(verts, idx, 4, 14, 0,  1, 5, 1, 3, 19);       // 右臂
    add_box(verts, idx, -1.2, 5, 0, 1, 5, 1, 4, 9);       // 左腿
    add_box(verts, idx, 1.2, 5, 0,  1, 5, 1, 5, 9);       // 右腿
    // ...武器等
}
```

**方法2: 导入OBJ模型**

项目已有 `src/render/obj_loader.h`，可以加载 .obj 文件：
```cpp
// 在 renderer.h init() 中:
meshes[0] = ObjLoader::load("assets/models/infantry.obj");
```

OBJ要求：
- 三角化（不支持quad面）
- 顶点属性需要包含 part_id 和 pivot_y（通过vertex color通道传入）
- 或用Blender导出时按骨骼分组，每组对应一个part_id

**方法3: 用Blender制作**

1. 在Blender中制作模型（保持低多边形）
2. 使用顶点色（Vertex Color）的R通道存储 `part_id`(0-5)
3. G通道存储 `pivot_y`（归一化到0-1，乘以模型高度）
4. 导出为 .obj
5. 修改 `obj_loader.h` 读取顶点色

### 模型颜色
模型颜色由 fragment shader (`shaders/unit.frag`) 决定：
- 红方/蓝方基色由 instance data 传入
- 不同部位在shader中应用不同明暗
- 修改 `unit.frag` 可以自定义着色方案

---

## 战斗数值配置

所有可调数值集中在 `src/game/balance_config.h`：

```cpp
// 经济
STARTING_MONEY = 30000       // 初始金币
PASSIVE_INCOME = 200         // 每秒收入
KILL_BOUNTY_BASE = 10        // 击杀赏金

// 兵种数据 (name, type, cost, hp, damage, range, speed, scale)
{"Infantry", 0, 100, 100, 10, 8, 6.0f, 2.0f}
// 直接改数字重编译即可
```

### 快速调平衡的建议
- `damage` 翻倍 = 战斗时间减半
- `range` 增加 = 弓箭手/火炮更强
- `speed` 影响骑兵冲锋效果
- `hp` 提高 = 更肉
- `cost` 调整经济平衡

---

## 联网功能

当前网络代码在 `src/net/` 下：
- `protocol.h` - 消息协议定义
- `socket.h` - Socket封装
- `server.h` - 服务器逻辑
- `client.h` - 客户端逻辑

### 当前状态
网络代码是**骨架状态**，定义了协议但未完整实现同步。要让联网真正工作需要：

1. **状态同步**: GPU数据readback后广播位置变更
2. **指令同步**: 所有玩家的move/buy命令通过server转发
3. **确定性**: 需要锁步(lockstep)或服务器权威模式
4. **推荐方案**: 
   - 小规模(<1万): 锁步模式，只同步输入
   - 大规模(>10万): 服务器权威+客户端预测+差量同步

暂时建议**专注单机体验**，联网需要较大工作量另行处理。

---

## 文件结构速查

```
src/
  game/
    game_state.h      - 游戏状态机、菜单、胜利条件
    balance_config.h  - 所有可调数值(改这里!)
  ai/
    combat_system.h   - CPU战斗逻辑(GPU模式下作为fallback)
    movement_system.h - CPU移动逻辑(GPU模式下作为fallback)
  render/
    gpu_compute.h     - GPU计算管线(战斗+移动+实例化)
    renderer.h        - 渲染器
    terrain.h         - 地形生成
    mesh_gen.h        - 模型生成(改这里换模型)
  ui/
    hud.h             - 顶部HUD
    menu.h            - 菜单/商店面板
  audio/
    audio_system.h    - 音频引擎
shaders/
  compute_spatial_hash.glsl - GPU空间哈希
  compute_combat.glsl       - GPU战斗AI
  compute_movement.glsl     - GPU移动
  compute_units.glsl        - 实例数据生成
  unit.vert/frag            - 单位着色器(改表情在frag里)
```


### 高面模型优化（重要）

引擎已内置三层优化：
1. 视锥剔除 — gpu_compute.h 的 compute shader 自动丢弃屏幕外单位。
2. LOD 距离切换 — renderer.h 的 lod_distance：近处画完整网格，超距自动切 billboard。5万单位能跑的关键。
3. GPU 实例化 — 每种兵种一次 draw call。

导入高面 OBJ：
    meshes[0] = ObjLoader::load("assets/models/infantry_high.obj");
    lod_distance = 150.0f;  // 高面建议 100~150
经验：屏幕内可见高模总面数控制在 ~500万三角形内，其余靠 LOD 降级。

---

## 自定义场景 / 地形

地形在 src/render/terrain.h 程序化生成（多层噪声 + 生物群系着色），不是贴图。
- 高度/地貌：改 terrain.h 的 regen()/generate_with_seed() 噪声参数
- 配色：改 shaders/terrain.frag 按高度/坡度混色
- 水面：shaders/terrain.vert 第20行 Water surface animation
- 加贴图：terrain.frag 加 sampler2D，terrain.h 上传纹理
- 场景道具(树/石/建筑)：放 assets/models/，加载同兵种 OBJ，务必走实例化+LOD

---

## 自定义视觉特效 / 粒子

粒子在 src/render/particles.h（CPU spawn，GPU 绘制）。已有预设：
    spawn_cannon_blast(pos)   // 大炮爆炸
    spawn_nuke_blast(pos)     // 核弹蘑菇云

炮弹轨迹：独立系统，shader 为 shaders/projectile.vert/frag
- 轨迹外观：改 projectile.frag（颜色/发光/衰减）
- 落点特效：命中时调用 particles.spawn_cannon_blast(impact_pos)

新增特效：仿照 spawn_cannon_blast 写 spawn_my_fx()，整体风格在 shaders/particle.frag。

---

## 自定义动画

当前是顶点着色器程序化动画（无骨骼），在 shaders/unit.vert：
- 每顶点带 part_id（身/头/臂/腿）和 pivot_y（旋转轴）
- 按 u_time 给四肢正弦摆动

改动画（unit.vert 第42行附近）：
    float anim_time = u_time * 3.5 + seed;  // 摆动速度
    swing = sin(anim_time) * 0.5;            // 腿摆幅度
    if (u_state > 1.5) arm_swing = sin(u_time*12.0)*1.2; // 攻击挥砍

真骨骼动画需较大改造（glTF/FBX 骨骼 + 骨骼矩阵 SSBO + 蒙皮），建议仅近处 LOD 用。

---

## 能否接入 UE5 做成插件？

简短回答：不能直接接，但核心思路可迁移。

本项目是独立 C++/OpenGL 引擎，UE5 用自己的 RHI 和渲染框架，OpenGL 代码无法直接塞入。

可行迁移路线：
1. compute_*.glsl 移植成 UE5 的 .usf Compute Shader（逻辑近 1:1 翻译）
2. 单位渲染改用 UE5 的 HISM 或 Niagara（自带 LOD/剔除）
3. balance_config.h 改成 UE5 DataAsset
4. 用 UE5 自带的 MassEntity 框架承载海量实体（最契合）

属于移植而非封装插件，工作量数周级别。蹭画质走 MassEntity+Niagara，要极致性能则保留自研引擎。

---
