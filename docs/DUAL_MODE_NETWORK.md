# MassRTS 双模式网络架构（工业级）
## RTS 模式与第一人称体素生存模式 —— 完全分离的两套网络栈

> 设计依据：对 MinecraftConsoles（Minecraft Legacy Console Edition v1.6 / TU19 真实商业源码）
> 网络层的逐文件调研 + 现有 RTS 锁步/回滚设计。
>
> **核心决策：RTS 与第一人称体素生存是两套独立的网络同步系统，不强行统一。**

---

## 0. 为什么必须分离

| 维度 | RTS 模式 | 第一人称体素生存 |
|------|----------|------------------|
| 玩家视角 | 上帝视角，指挥单位群 | 第一人称角色 |
| 同步主体 | 命令（指令流） | 世界体素状态 + 实体 |
| 权威模型 | 确定性锁步 / 回滚 | 服务器权威 + 客户端预测 |
| 单位规模 | 数千单位 | 数十玩家 + 怪物 |
| 地形 | 战场偶尔雕刻 | 高频挖掘/建造，核心玩法 |
| 同步策略 | 同步"输入" | 同步"状态变更" |
| 参考实现 | StarCraft / AoE 锁步 | **MinecraftConsoles（本文档调研对象）** |

**结论：** 强行用一套网络代码覆盖两种模式会两头不讨好。
正确做法是抽象出共享的传输层（Connection/Packet/Socket），
在其上构建两套独立的同步逻辑（RTS 栈 / 体素栈）。

---

## 1. 调研成果：MinecraftConsoles 工业级网络架构

以下是从真实商业源码逐文件提取的可复用工业模式。**这是本设计的事实基础。**

### 1.1 分层传输架构（Connection / Packet / Socket）

源码 `Connection.h` 揭示的工业级连接管理：

```
应用层    →  各种 Packet 子类（ChunkTilesUpdatePacket, MoveEntityPacketSmall...）
分发层    →  PacketListener（虚函数 handleXXX 分发）
连接层    →  Connection（双队列 + 双线程 + 流控）
传输层    →  Socket（TCP）
```

**Connection 的关键设计（`Connection.h:42-80`）：**
- **双向独立线程**：`readThread` 和 `writeThread` 分离（`Connection.h:65-66`）
- **三个队列**（`Connection.h:55-58`）：
  - `incoming` —— 收到的包（带 `incoming_cs` 临界区保护）
  - `outgoing` —— 优先发送队列（高优先级，如玩家移动）
  - `outgoing_slow` —— 低优先级慢速队列（如区块数据，可延迟）
- **超时机制**：`MAX_TICKS_WITHOUT_INPUT = 20*60`（`Connection.h:34`）= 60 秒无输入断开（20 tick/秒）
- **流控背压**：`estimatedRemaining` 累计待发字节，超过 `1MB` 触发保护（`Connection.cpp:453`）
- **QoS 标记**：IPTOS_LOWDELAY 等 TCP 服务类型标记（`Connection.h:37-40`）

**双队列优先级（`Connection.cpp:148-180`）：**
```cpp
void Connection::send(shared_ptr<Packet> packet) {
    EnterCriticalSection(&writeLock);
    estimatedRemaining += packet->getEstimatedSize() + 1;
    if (packet->shouldDelay)
        outgoing_slow.push(packet);   // 区块等大数据走慢队列
    else
        outgoing.push(packet);        // 玩家移动等走快队列
    LeaveCriticalSection(&writeLock);
}
```
**慢队列还有节流**：`slowWriteDelay` 控制每次只发一个慢包（`Connection.cpp:235-245`），
避免区块传输挤占玩家移动等实时数据带宽。**这是工业级流控的精髓。**

---

### 1.2 数据包系统（Packet）

源码 `Packet.h` 的工业级包架构：

**包注册表（`Packet.h:50-58`）：**
```cpp
static unordered_map<int, packetCreateFn> idToCreateMap;  // id → 工厂函数
static unordered_set<int> clientReceivedPackets;          // 仅客户端可收
static unordered_set<int> serverReceivedPackets;          // 仅服务器可收
static void map(int id, bool receiveOnClient, bool receiveOnServer,
                bool sendToAnyClient, bool renderStats,
                const type_info& clazz, packetCreateFn);
```
**关键工业实践：**
- 每个包有唯一 `id`（如 ChunkTilesUpdate=52, MoveEntitySmall=162）
- **方向校验**：`clientReceivedPackets`/`serverReceivedPackets` 防止客户端伪造服务器包（安全）
- **工厂模式**：`idToCreateMap` 按 id 反序列化出正确子类（`Packet.h:65 getPacket(id)`）

**包的核心接口（`Packet.h:90-100`）：**
```cpp
virtual void read(DataInputStream *dis) = 0;    // 反序列化
virtual void write(DataOutputStream *dos) = 0;  // 序列化
virtual void handle(PacketListener *listener) = 0;  // 双分发到处理器
virtual int getEstimatedSize() = 0;             // 流控用
virtual bool canBeInvalidated();                // 可否被新包作废
virtual bool isInvalidatedBy(shared_ptr<Packet> packet);  // 包合并
```

**包合并/作废机制（关键带宽优化）：**
`MoveEntityPacketSmall.cpp:63-72` 展示了"过期包丢弃"：
```cpp
bool MoveEntityPacketSmall::canBeInvalidated() { return true; }
bool MoveEntityPacketSmall::isInvalidatedBy(shared_ptr<Packet> packet) {
    auto target = dynamic_pointer_cast<MoveEntityPacketSmall>(packet);
    return target != nullptr && target->id == id;  // 同一实体的旧移动包被新的覆盖
}
```
**意义：** 如果队列里还有实体 A 的旧移动包没发出去，新的移动包到了，
旧包直接作废 —— 只发最新位置。**这是带宽优化的核心技巧。**

---

### 1.3 实体移动位打包（极致带宽优化）

`MoveEntityPacketSmall.cpp` 是工业级带宽压缩的教科书案例。

**4 种移动包按数据量分级（只发变化的部分）：**
| 包类型 | id | 大小 | 用途 |
|--------|-----|------|------|
| MoveEntityPacketSmall | 162 | 2 字节 | 仅 id（保活）|
| Pos | 163 | 3 字节 | 位置增量 |
| Rot | 164 | 2 字节 | 仅旋转 |
| PosRot | 165 | 4 字节 | 位置+旋转 |

**位打包技巧（`MoveEntityPacketSmall.cpp:100-111`）：**
```cpp
// PosRot 把 id + yRot 塞进一个 short，xyz 增量塞进另一个 short
short idAndRot = id | yRot << 11;          // id 占 11 位(<2048), yRot 占 5 位
dos->writeShort(idAndRot);
short xAndYAndZ = (xa << 11) | ((ya & 0x3f) << 5) | (za & 0x1f);  // xyz 增量各占几位
dos->writeShort(xAndYAndZ);
```
**整个实体的一次移动同步只用 4 字节！** 因为：
- 位置是**增量**（相对上一帧的 char 偏移，不是绝对坐标）
- id 和坐标位打包进 16 位整数
- 增量超出 char 范围时才用 `TeleportEntityPacket` 发全量绝对坐标

**这正是我之前 VOXEL 文档里 24→11 字节估算的更激进版本（4 字节）。**

---

### 1.4 实体数据增量同步（SynchedEntityData 脏标记）

`SynchedEntityData.h` 是工业级"只同步变化字段"的实现。

**脏标记机制（`SynchedEntityData.h:108-112`）：**
```cpp
shared_ptr<DataItem> itemsById[MAX_ID_VALUE+1];  // 按字段 id 索引
bool m_isDirty;
void markDirty(int id);                           // 字段变化时标脏
vector<DataItem>* packDirty();                    // 只打包脏字段
void clearDirty();                                // 发送后清除
```

**字段类型系统（`SynchedEntityData.h:59-66`）：**
```cpp
TYPE_BYTE=0, TYPE_SHORT=1, TYPE_INT=2, TYPE_FLOAT=3,
TYPE_STRING=4, TYPE_ITEMINSTANCE=5, TYPE_POS=6
```
**类型 + id 打包进一个字节（`SynchedEntityData.h:73-77`）：**
```cpp
TYPE_MASK = 0xe0;  TYPE_SHIFT = 5;  // 高 3 位是类型
MAX_ID_VALUE = 0x1f;               // 低 5 位是字段 id（最多 32 个字段）
```
**意义：** 实体的血量、状态、装备等任意字段，
只有改变的才进网络，每个字段开销仅 1 字节头 + 数据。
**新玩家加入时用 `packAll()` 发全量，之后只发 `packDirty()`。**

---

### 1.5 区块流式同步与可见性

调研发现的区块同步包族：
- `ChunkVisibilityPacket`（id=50, `ChunkVisibilityPacket.h:13-14`）：`{int x, int z, bool visible}`
  —— 服务器告诉客户端某区块进入/离开视野（按需加载/卸载）
- `ChunkVisibilityAreaPacket`：批量区块可见性
- `ChunkTilesUpdatePacket`（id=52, `ChunkTilesUpdatePacket.h:11-16`）：
  ```cpp
  int xc, zc;             // 区块坐标
  shortArray positions;   // 变化的方块位置（区块内偏移）
  byteArray blocks;       // 新方块 id
  byteArray data;         // 方块附加数据
  byte count;             // 变化数（注释说"never higher than 10"）
  ```
  —— **一个包携带多个方块变更，批量打包**
- `BlockRegionUpdatePacket`：大范围区域更新（如爆炸）
- `TileUpdatePacket`：单个方块更新

**工业级洞察：**
- 区块可见性与数据**分离**：先发 `ChunkVisibilityPacket` 声明，再发 `ChunkTilesUpdatePacket` 数据
- 方块变更**批量化**（count≤10）减少包数量
- 区块数据走 `outgoing_slow` 慢队列，不挤占实时数据

---

### 1.6 持久化（RegionFile）

Minecraft 的 `.mca` 区域文件格式（`RegionFile.h/cpp`）：
- 每个区域文件存 32×32 个区块
- 文件头有**扇区偏移表**：每区块 4 字节（3 字节偏移 + 1 字节扇区数）
- 时间戳表：每区块 4 字节
- 区块数据按 4KB 扇区对齐，zlib 压缩
- **按需读写单个区块**，无需加载整个文件

**这正是我之前 VOXEL 文档"区域文件"设计的工业级原型，直接采用。**

---

## 2. 共享传输层（两套栈复用）

基于调研，抽象出两种模式都用的传输基础设施：

```cpp
// 共享传输层 —— 直接移植 MinecraftConsoles 架构
namespace net {

// 数据包基类（移植自 Packet.h）
class Packet {
public:
    virtual void read(ByteBuffer& in) = 0;
    virtual void write(ByteBuffer& out) = 0;
    virtual void handle(PacketHandler& h) = 0;
    virtual int getId() const = 0;
    virtual int estimatedSize() const = 0;
    virtual bool canBeInvalidated() const { return false; }
    virtual bool isInvalidatedBy(const Packet& p) const { return false; }

    // 包注册表（移植自 Packet::map / idToCreateMap）
    static void registerPacket(int id, bool clientOk, bool serverOk,
                               PacketFactory factory);
    static std::unique_ptr<Packet> create(int id);
    static bool canReceive(int id, bool isServer);  // 方向校验防伪造
};

// 连接管理（移植自 Connection.h 的双线程双队列）
class Connection {
    std::thread readThread, writeThread;
    std::queue<PacketPtr> incoming;       std::mutex incoming_mtx;
    std::queue<PacketPtr> outgoing;       // 实时优先队列
    std::queue<PacketPtr> outgoing_slow;  // 大数据慢队列（区块）
    std::mutex write_mtx;

    size_t estimatedRemaining = 0;        // 流控背压
    int noInputTicks = 0;                 // 超时检测
    static constexpr int MAX_TICKS_WITHOUT_INPUT = 20 * 60;
    static constexpr size_t BACKPRESSURE_LIMIT = 1024 * 1024;

    void send(PacketPtr p) {
        std::lock_guard lk(write_mtx);
        estimatedRemaining += p->estimatedSize() + 1;
        if (p->shouldDelay) outgoing_slow.push(std::move(p));
        else                outgoing.push(std::move(p));
    }
    // 写线程：优先发 outgoing，慢队列节流（移植 slowWriteDelay）
    bool writeTick();
    bool readTick();
};

}  // namespace net
```

**两套同步栈都构建在这个 `net::Connection` 之上。**

---

## 3. RTS 网络栈（确定性锁步 + 回滚）

> 详见 `NETWORK_ARCHITECTURE.md` 和 `ROLLBACK_NETCODE.md`。这里只说与共享层的关系。

```cpp
namespace rts_net {

// RTS 只在共享传输层上跑"命令流"
class RtsPacket_Commands : public net::Packet {
    uint32_t frame;
    std::vector<Command> commands;   // 该帧该玩家的命令
    int getId() const override { return RTS_PKT_COMMANDS; }
};

// 同步校验（防 desync）
class RtsPacket_SyncCheck : public net::Packet {
    uint32_t frame;
    uint32_t checksum;               // 定点数状态哈希
};

// RTS 引擎：锁步 + 回滚（详见 ROLLBACK_NETCODE.md）
class RtsNetEngine {
    RollbackEngine rollback;         // 环形缓冲 + 确定性重放
    void tick();
};

}
```

**RTS 栈特点（与体素栈完全不同）：**
- 只同步**命令**，不同步状态（地图用种子，单位用确定性模拟）
- 走 `outgoing` 实时队列（命令必须低延迟）
- 定点数确定性（跨平台同步生死线）
- 回滚纠正预测错误

---

## 4. 第一人称体素生存网络栈（服务器权威，参考 MinecraftConsoles）

> 这是全新设计，**直接采用 MinecraftConsoles 的工业模式**。

### 4.1 架构总览

```
客户端（第一人称）          服务器（权威）              其他客户端
  │                          │                          │
  │  PlayerInputPacket ─────>│                          │
  │  （移动/挖/放）           │ [权威验证 + 模拟]         │
  │                          │  MoveEntitySmall ───────>│ （4字节增量）
  │<───── ChunkVisibility ───│  ChunkTilesUpdate ──────>│
  │<───── ChunkTilesUpdate ──│                          │
  │  [本地预测]               │  SetEntityData(脏) ─────>│
  │  [快照插值]               │                          │
```

### 4.2 体素栈数据包族（移植 MC 设计）

```cpp
namespace voxel_net {

// === 区块流式同步（移植 ChunkVisibility + ChunkTilesUpdate）===
class VoxelChunkVisibility : public net::Packet {  // 移植 id=50 设计
    int32_t cx, cz; bool visible;     // 进入/离开视野
    int estimatedSize() const override { return 9; }
};

class VoxelChunkData : public net::Packet {
    int32_t cx, cy, cz;
    uint16_t editCount;               // 该区块的玩家改动数（0=纯种子生成）
    std::vector<VoxelEdit> edits;     // 只发"修改层"，基础层种子重建
    bool shouldDelay = true;          // 走慢队列！（关键）
};

// === 方块编辑批量更新（移植 ChunkTilesUpdatePacket id=52）===
class VoxelEditBatch : public net::Packet {
    int32_t cx, cz;
    std::vector<uint16_t> positions;  // 区块内偏移
    std::vector<uint8_t>  blockTypes;
    uint8_t count;                    // 批量（MC 经验：≤10）
};

// === 玩家动作（移植 PlayerActionPacket / UseItemPacket）===
class VoxelPlayerAction : public net::Packet {
    uint32_t playerId;
    uint8_t  action;                  // 0=开始挖, 1=挖完, 2=放置, 3=取消
    int64_t  voxelKey;                // 目标体素
    uint8_t  face;                    // 操作面（放置方向）
};

// === 玩家移动（移植 MoveEntityPacketSmall 位打包）===
class VoxelPlayerMove : public net::Packet {
    int16_t idAndYRot;                // id(11位) | yRot(5位) —— 移植 MC 位打包
    int16_t xyzDelta;                 // 增量位打包 —— 4 字节同步一次移动
    int estimatedSize() const override { return 4; }
    bool canBeInvalidated() const override { return true; }  // 旧移动包作废
    bool isInvalidatedBy(const Packet& p) const override;    // 同 id 覆盖
};

// 增量超范围时用全量传送包（移植 TeleportEntityPacket）
class VoxelPlayerTeleport : public net::Packet {
    uint32_t id; double x, y, z; float yaw, pitch;
};

// === 实体数据增量（移植 SynchedEntityData 脏标记）===
class VoxelEntityData : public net::Packet {
    uint32_t entityId;
    std::vector<DataItem> dirtyFields;  // 只发变化字段（血量/状态/装备）
};

// === 库存（移植 ContainerSetContent / ContainerSetSlot）===
class VoxelInventorySlot : public net::Packet {  // 单格变化
    uint8_t containerId, slotIndex;
    uint16_t itemId, count;
};

}  // namespace voxel_net
```

### 4.3 挖掘/建造完整流程（服务器权威 + 客户端预测）

```cpp
// 客户端：挖方块
void VoxelClient::onDig(int64_t voxel) {
    // 1. 立即本地预测（零延迟反馈）
    localWorld.set(voxel, AIR);
    rebuildMeshLocal(voxel);
    pendingEdits.push_back({voxel, AIR, seq++});

    // 2. 发服务器（走实时队列，玩家动作要快）
    conn.send(make<VoxelPlayerAction>(playerId, ACTION_BREAK, voxel));
}

// 服务器：权威验证（移植 MC 思路 + 反作弊）
void VoxelServer::onPlayerAction(Player& p, VoxelPlayerAction& act) {
    // 验证：距离、权限、库存（防作弊）
    if (dist(p.pos, voxelToWorld(act.voxelKey)) > MAX_REACH) return reject(p, act);
    if (!p.canModify(act.voxelKey)) return reject(p, act);

    if (act.action == ACTION_BREAK) {
        uint8_t mined = world.get(act.voxelKey);
        p.inventory.add(blockToItem(mined));      // 掉落进背包
        world.set(act.voxelKey, AIR);
    } else if (act.action == ACTION_PLACE) {
        if (!p.inventory.has(act.heldBlock)) return reject(p, act);
        p.inventory.remove(act.heldBlock);
        world.set(act.voxelKey, act.heldBlock);
    }
    markChunkDirty(act.voxelKey);
    broadcastToNearby(act.voxelKey, make<VoxelEditBatch>(...));  // 广播给附近玩家
}

// 客户端：服务器拒绝 → 回滚预测
void VoxelClient::onReject(int64_t voxel, uint8_t authoritative) {
    localWorld.set(voxel, authoritative);  // 用权威值覆盖
    rebuildMeshLocal(voxel);
}
```

### 4.4 区块可见性管理（兴趣管理）

```cpp
// 移植 MC 的 ChunkVisibility 推拉机制
class VoxelChunkStreamer {
    std::unordered_map<uint32_t, std::set<ChunkCoord>> playerLoaded;
    int VIEW_DISTANCE = 8;  // 区块

    void update(Player& p) {
        auto needed = chunksInRadius(p.pos, VIEW_DISTANCE);
        for (auto& c : needed)
            if (!playerLoaded[p.id].count(c)) {
                conn.send(make<VoxelChunkVisibility>(c.x, c.z, true));   // 实时声明
                conn.send(make<VoxelChunkData>(c, getEdits(c)));        // 慢队列数据
                playerLoaded[p.id].insert(c);
            }
        // 离开视野的卸载
        for (auto it = playerLoaded[p.id].begin(); it != playerLoaded[p.id].end(); )
            if (!needed.count(*it)) {
                conn.send(make<VoxelChunkVisibility>(it->x, it->z, false));
                it = playerLoaded[p.id].erase(it);
            } else ++it;
    }
};
```

### 4.5 持久化（移植 RegionFile .mca 格式）

```cpp
// 移植 MC RegionFile：32×32 区块/文件，扇区偏移表 + zlib
class VoxelRegionFile {
    static constexpr int REGION_DIM = 32;
    static constexpr int SECTOR_SIZE = 4096;
    struct Header {
        uint32_t offsets[REGION_DIM * REGION_DIM];   // 3字节偏移+1字节扇区数
        uint32_t timestamps[REGION_DIM * REGION_DIM];
    };
    // 只存"修改层"VoxelEdit，基础层种子重建 → 存档极小
    void writeChunk(ChunkCoord c, const std::vector<VoxelEdit>& edits);
    std::vector<VoxelEdit> readChunk(ChunkCoord c);
};
```

---

## 5. 两套栈的对照总表

| 要素 | RTS 栈 | 体素生存栈 |
|------|--------|-----------|
| 权威模型 | 确定性锁步 + 回滚 | 服务器权威 + 客户端预测 |
| 同步内容 | 命令流 | 体素 Delta + 实体状态 |
| 地形 | 种子 + 确定性 | 种子（基础层）+ Delta（修改层）|
| 玩家 | 单位群（确定性模拟）| 第一人称实体（4字节增量移动）|
| 发送队列 | outgoing（实时）| 命令走实时，区块走 slow |
| 反作弊 | 校验和 desync 检测 | 服务器验证每个动作 |
| 确定性要求 | 严格（定点数）| 不要求（服务器权威）|
| 持久化 | 命令日志（可选回放）| RegionFile .mca 增量 |
| 参考来源 | StarCraft/AoE | **MinecraftConsoles** |
| 共享 | net::Connection / net::Packet / Socket / ByteBuffer | （同左）|

---

## 6. 实现路线

### 阶段 0：共享传输层（两栈地基）
- [ ] `net::Packet` 基类 + 注册表 + 方向校验（移植 Packet.h）
- [ ] `net::Connection` 双线程双队列 + 流控（移植 Connection.h/cpp）
- [ ] `ByteBuffer` 序列化 + 位打包工具
- [ ] 包合并/作废机制（canBeInvalidated）

### 阶段 1：RTS 栈
- [ ] 命令流同步 + 锁步
- [ ] 回滚 + 定点数（详见 ROLLBACK_NETCODE.md）

### 阶段 2：体素生存栈
- [ ] 区块流式同步（VoxelChunkVisibility + VoxelChunkData）
- [ ] 挖/放服务器权威 + 客户端预测
- [ ] 玩家 4 字节增量移动（移植 MoveEntityPacketSmall 位打包）
- [ ] 实体数据脏标记增量（移植 SynchedEntityData）
- [ ] 库存/合成服务器权威
- [ ] RegionFile 持久化

---

## 7. 结论

**核心决策（回答你的问题）：**
1. **RTS 与第一人称生存彻底分离** —— 两套独立同步栈，各用各的网络模式
2. **RTS 用 RTS 的**：确定性锁步 + 回滚（命令同步）
3. **第一人称生存用第一人称的**：服务器权威 + 客户端预测（状态同步）
4. **共享传输层**：Connection/Packet/Socket/ByteBuffer 两栈复用，不重复造轮子

**工业化水准来源：** 体素栈的每个关键设计都有 MinecraftConsoles（真实商业 Minecraft 源码）的对应实现作为依据：
- 双队列流控 ← Connection.cpp
- 包注册表 + 方向校验 ← Packet.h
- 4 字节增量移动位打包 ← MoveEntityPacketSmall.cpp
- 脏标记增量同步 ← SynchedEntityData.h
- 区块可见性推拉 ← ChunkVisibilityPacket
- 包合并作废 ← isInvalidatedBy
- RegionFile 持久化 ← RegionFile.cpp

**这不是凭空设计，是对一份经过商业验证的 Minecraft 网络栈的逐文件提炼 + 适配到 SDF 体素世界。**
