# MassRTS 体素沙盒（Minecraft 风格）多人网络设计

> 补充缺口：现有 RTS 网络架构**没有**为 Minecraft 式创造/生存 FPS 设计。
> 本文档专门解决多人**挖掘、挖矿、建造、库存、第一人称同步**。

---

## 为什么现有架构不够

| 维度 | RTS 模式（现设计） | 体素沙盒模式（需要） |
|------|------------------|-------------------|
| 核心状态 | 单位（实体） | **世界体素本身** |
| 地形 | 只读程序生成 + 偶尔雕刻 | **每体素可变、需持久化** |
| 修改频率 | 稀疏（偶尔炸坑） | **高频（每秒多次挖/放）** |
| 操作类型 | 减法（挖） | **加法 + 减法（挖 + 建）** |
| 玩家 | 上帝视角，控制单位群 | **第一人称角色实体** |
| 加载 | 全地图 | **按需区块流式加载** |
| 库存 | 无 | **物品栏 + 合成必须同步** |

**结论：体素模式不是"RTS 加个挖掘"，是一套独立的同步子系统。**

---

## 核心设计：体素是权威世界状态

### 三层体素数据
```
1. 基础层（Base）   = SDF 程序生成（确定性，种子同步，零带宽）
2. 修改层（Delta）  = 玩家挖/放的体素改动（必须存储 + 同步 + 持久化）
3. 渲染层（Mesh）   = 由 Base+Delta 合成（纯本地，不同步）
```

**关键洞察：只同步"修改层 Delta"。**
- 未触碰的世界 = 种子重建，零带宽
- 只有玩家改过的体素才进入网络和存档
- 与现有 SDF `CarveEvent` 思路一致，但需扩展为**可加可减、可持久化、可流式**

---

## 1. 体素修改协议（替代稀疏 CarveCommand）

```cpp
// 单个体素编辑（细粒度，高频）
struct VoxelEdit {
    uint64_t voxel_key;         // 全局体素坐标打包 (x:21|y:21|z:21)
    uint8_t  block_type;        // 0=空气(挖除), 1..N=方块类型(放置/挖矿)
    uint8_t  flags;             // bit0=玩家放置, bit1=自然生成
};

// 批量编辑（一次挖一片/建一墙）
struct VoxelEditBatch {
    uint32_t frame;             // 确定性帧号（与回滚系统对齐）
    uint32_t player_id;
    uint16_t edit_count;
    VoxelEdit edits[256];       // 批量打包，减少包数量
};
// 单次编辑 ~9 字节；挖矿连续操作打包后 < 2KB
```

### 与 SDF 的整合
- 现 SDF 是连续场（适合平滑地形 + 球形雕刻）
- Minecraft 是离散方块（适合精确单体素挖放）
- **方案：** 体素 Delta 层存"方块类型网格"，渲染时：
  - 自然地形 → SDF marching cubes（平滑）
  - 玩家放置方块 → 立方体网格（Minecraft 风格）
  - 玩家挖除 → 在 SDF 上做布尔减

---

## 2. 区块（Chunk）流式同步 —— 可扩展性核心

**不能把整个世界发给玩家。** 按需流式加载。

```cpp
constexpr int CHUNK_DIM = 32;       // 32³ 体素/区块

struct ChunkCoord { int32_t cx, cy, cz; };

// 客户端进入新区域时请求
struct ChunkRequest {
    ChunkCoord coords[16];          // 一次请求周围多个区块
};

// 服务器回应：只发"修改层"，基础层客户端用种子自己生成
struct ChunkData {
    ChunkCoord coord;
    uint16_t   edit_count;          // 0 = 未修改区块（客户端纯种子生成）
    VoxelEdit  edits[];             // 仅该区块的玩家改动
    uint32_t   version;             // 区块版本号（增量更新用）
};
```

### 兴趣管理（Interest Management）
```cpp
class ChunkStreamer {
    // 每玩家维护"已加载区块"集合
    std::unordered_map<uint32_t, std::set<ChunkCoord>> player_loaded;

    int VIEW_DISTANCE = 8;          // 半径 8 区块 = 256 米

    void update(Player& p) {
        auto needed = chunks_in_radius(p.pos, VIEW_DISTANCE);
        // 发送新进入视野的区块
        for (auto& c : needed)
            if (!player_loaded[p.id].count(c))
                send_chunk(p, c);
        // 卸载离开视野的区块（不再推送更新）
        unload_far_chunks(p, needed);
    }
};
```

**带宽：** 玩家只接收周围区块改动 + 移动时的新区块边缘。静态世界几乎零流量。

---

## 3. 高频编辑的同步模式（挖矿/建造）

挖掘是**高频**操作（连续挖矿每秒数次），不能走回滚锁步。

### 推荐：服务器权威 + 客户端预测（针对体素）
```cpp
class VoxelEditClient {
    void on_local_dig(uint64_t voxel) {
        // 1. 立即本地应用（即时反馈，无延迟感）
        local_world.set(voxel, AIR);
        pending_edits.push_back({voxel, AIR, current_seq++});
        rebuild_mesh_local(voxel);

        // 2. 发送给服务器
        net.send(VoxelEdit{voxel, AIR});
    }

    void on_server_confirm(uint64_t voxel, uint8_t type, uint32_t seq) {
        // 服务器确认 → 移除 pending
        pending_edits.erase_seq(seq);
    }

    void on_server_reject(uint64_t voxel, uint8_t authoritative_type) {
        // 服务器拒绝（例如方块已被别人挖了/权限不足）
        // → 用服务器权威值覆盖本地预测
        local_world.set(voxel, authoritative_type);
        rebuild_mesh_local(voxel);
    }
};
```

### 服务器验证（防作弊 + 解决冲突）
```cpp
class VoxelEditServer {
    bool validate_edit(Player& p, VoxelEdit e) {
        if (distance(p.pos, voxel_to_world(e.voxel_key)) > MAX_REACH)
            return false;                       // 距离作弊检测
        if (!p.can_modify(e.voxel_key))
            return false;                       // 保护区/权限
        if (e.block_type != AIR && !p.inventory.has(e.block_type))
            return false;                       // 放置但库存没有该方块
        return true;
    }

    void apply_edit(Player& p, VoxelEdit e) {
        if (!validate_edit(p, e)) {
            send_reject(p, e.voxel_key, world.get(e.voxel_key));
            return;
        }
        // 挖矿 → 给玩家掉落物
        if (e.block_type == AIR) {
            uint8_t mined = world.get(e.voxel_key);
            p.inventory.add(block_to_item(mined));
        } else {
            p.inventory.remove(e.block_type);   // 放置消耗库存
        }
        world.set(e.voxel_key, e.block_type);
        broadcast_edit(e);                      // 广播给附近所有玩家
    }
};
```

---

## 4. 第一人称玩家实体同步

RTS 是上帝视角控制单位群；FPS 需要同步**玩家角色本身**。

```cpp
struct PlayerState {
    uint32_t player_id;
    glm::vec3 position;
    glm::vec3 velocity;
    float     yaw, pitch;           // 视角朝向（其他玩家看你头转向）
    uint8_t   anim_state;           // 0=站立,1=走,2=跑,3=挖,4=放置,5=跳
    uint16_t  held_item;            // 手持物品（其他玩家可见）
    int16_t   health;
};

// 玩家移动：高频但数据小
struct PlayerMoveUpdate {
    uint32_t player_id;
    glm::vec3 position;
    float yaw, pitch;
    uint8_t anim_state;
};
// 20 字节 × 20Hz × N 玩家，附近玩家才发送（兴趣管理）
```

**移动预测 + 插值（与单位插值复用）：**
- 本地玩家：客户端预测移动（即时响应）+ 服务器纠正
- 远端玩家：快照插值（平滑显示）

---

## 5. 库存与合成同步

```cpp
struct InventorySlot {
    uint16_t item_id;
    uint16_t count;
};

struct Inventory {
    InventorySlot slots[36];        // 主物品栏
    InventorySlot hotbar[9];
    InventorySlot armor[4];
};

// 库存变更只发给本人（私有状态）
struct InventoryUpdate {
    uint8_t slot_index;
    uint16_t item_id;
    uint16_t count;
};

// 合成请求 → 服务器验证配方 → 返回结果
struct CraftRequest { uint16_t recipe_id; uint8_t count; };
```

**关键：库存是服务器权威**（防刷物品作弊）。客户端可乐观预测，服务器为准。

---

## 6. 体素物理传播（水流、重力沙、岩浆）

Minecraft 的方块更新会传播 —— 这是**服务器权威**的（否则不同步）。

```cpp
class VoxelPhysics {
    std::queue<uint64_t> update_queue;          // 待处理的方块更新

    void on_block_changed(uint64_t voxel) {
        // 邻居方块加入更新队列
        for (auto n : neighbors(voxel))
            schedule_update(n);
    }

    void tick() {                               // 服务器固定频率处理
        int budget = MAX_UPDATES_PER_TICK;      // 限制每 tick 更新数（防爆炸）
        while (!update_queue.empty() && budget--) {
            uint64_t v = update_queue.front(); update_queue.pop();
            if (is_water(v)) flow_water(v);     // 水流扩散
            if (is_sand(v) && air_below(v)) fall(v);  // 沙子下落
            // 产生的新改动 → 作为 VoxelEdit 广播
        }
    }
};
```

**带宽控制：** 物理产生的体素改动也走 `VoxelEdit` 广播，但**限制每 tick 数量**防止洪水。

---

## 7. 世界持久化（存档）

```cpp
// 区块存档：只存修改层（基础层种子重建）
struct ChunkSaveFormat {
    ChunkCoord coord;
    uint32_t   version;
    uint16_t   edit_count;
    VoxelEdit  edits[];             // 该区块所有玩家改动
};

class WorldPersistence {
    // 区域文件（类似 Minecraft .mca）：每文件存 32×32 区块
    void save_region(RegionCoord r);
    void load_region(RegionCoord r);

    // 增量保存：只写 dirty 区块
    std::set<ChunkCoord> dirty_chunks;
    void autosave() {
        for (auto& c : dirty_chunks) save_chunk(c);
        dirty_chunks.clear();
    }
};
```

**存档大小：** 只存改动。未触碰的世界永远是种子重建，存档极小。

---

## 8. 性能与带宽估算

| 操作 | 频率 | 带宽 |
|------|------|------|
| 连续挖矿 | 5 次/秒 | ~50 字节/秒 |
| 建造（放方块） | 5 次/秒 | ~50 字节/秒 |
| 玩家移动同步 | 20 Hz | ~400 字节/秒/玩家 |
| 区块加载（移动时） | 突发 | ~5-20 KB/突发 |
| 水流物理传播 | 限频 | < 1 KB/秒 |
| **8 人服务器总计** | — | **~100-200 KB/s** |

**世界静止时几乎零流量**（核心优势：只同步改动）。

---

## 9. 与现有架构的整合

| 现有系统 | 复用 | 改动 |
|---------|------|------|
| SDF 地形 + 种子同步 | ✅ 作为基础层 | 加"修改层"叠加 |
| `CarveEvent` 雕刻 | ✅ 思路一致 | 扩展为可加可减 + 持久化 |
| 网络模式选择器 | ✅ | 新增 `VOXEL_SANDBOX` 模式 |
| 单位插值 | ✅ | 复用给第一人称玩家插值 |
| Desync 检测 | ✅ | 体素世界校验和 |

```cpp
enum NetworkMode {
    LOCKSTEP,            // 2 人 RTS
    HYBRID,             // 3-8 人 RTS
    SERVER_AUTHORITATIVE, // 8+ 人 RTS
    VOXEL_SANDBOX       // ← 新增：Minecraft 式创造/生存
};
```

**体素沙盒模式 = 服务器权威 + 体素流式 + 客户端预测**

---

## 10. 实现清单

### Phase 4: 体素沙盒基础（在 RTS 网络之后）
- [ ] 体素修改层数据结构（Base + Delta 分离）
- [ ] `VoxelEdit` / `VoxelEditBatch` 协议
- [ ] 区块流式加载（`ChunkStreamer` + 兴趣管理）
- [ ] 客户端预测挖/放 + 服务器验证
- [ ] 第一人称玩家实体同步
- [ ] 库存 + 合成同步（服务器权威）
- [ ] 体素物理传播（水/沙/岩浆，限频广播）
- [ ] 世界持久化（区域文件 + 增量保存）

### 验证测试
```cpp
// 测试1：两玩家同时挖同一方块 → 服务器解决冲突，双方最终一致
// 测试2：玩家A建墙，玩家B走来 → B加载区块时看到墙
// 测试3：水流传播 → 所有玩家看到相同水流结果
// 测试4：重连 → 世界状态从存档+种子正确恢复
// 测试5：8人同时挖矿建造 → 带宽 < 200KB/s，无卡顿
```

---

## 结论

**现有 RTS 架构没有为 Minecraft 模式设计**，但**基础设施可复用**：
- SDF 种子同步 → 体素基础层
- 雕刻事件 → 扩展为体素 Delta
- 网络模式选择器 → 加 VOXEL_SANDBOX

**体素沙盒的核心与 RTS 根本不同：**
- RTS 同步"实体"，体素同步"世界状态本身"
- 必须有：修改层持久化、区块流式、第一人称同步、库存、体素物理

**推荐同步策略：服务器权威 + 客户端预测**
- 挖/放即时本地反馈（预测）
- 服务器验证防作弊 + 解决冲突
- 只同步改动 → 静态世界零带宽

要开始 Phase 4 还是先完成 RTS 网络（Phase 1-3）？
