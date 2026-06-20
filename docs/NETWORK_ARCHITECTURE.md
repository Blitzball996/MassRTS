# MassRTS 网络架构设计
## 混合同步方案 - 自适应高性能多人游戏

---

## 核心设计理念

**根据玩家数量自动选择最优同步方案：**
- **2 人游戏：** 纯锁步确定性同步（最高性能，零延迟感知）
- **3-8 人游戏：** 混合方案（地形锁步 + 单位快照）
- **8+ 人游戏：** 完全服务器权威（可扩展性优先）

---

## 方案对比

| 特性 | 锁步 (2P) | 混合 (3-8P) | 服务器权威 (8+P) |
|------|-----------|-------------|------------------|
| 带宽 | 5-10 KB/s | 50-150 KB/s | 200-500 KB/s |
| 延迟感知 | 无 | 低 | 中等 |
| 作弊防护 | 弱 | 中 | 强 |
| 掉线容忍 | 差 | 中 | 好 |
| 服务器负载 | 无 | 低 | 高 |

---

## 方案 A: 纯锁步（2 人对战）

### 原理
所有客户端：
1. 用相同**地图种子**生成完全相同的地形
2. 以相同顺序执行相同的**输入命令**
3. 游戏逻辑**完全确定性**，保证状态一致

### 数据流

```
客户端 A                服务器（中继）              客户端 B
  |                         |                         |
  |---- [帧5: 命令A] ------>|                         |
  |                         |---- [帧5: 命令A] ------>|
  |                         |<---- [帧5: 命令B] ------|
  |<---- [帧5: 命令B] ------|                         |
  |                         |                         |
  [执行帧5: A+B命令]        |         [执行帧5: A+B命令]
```

### 协议结构

```cpp
struct LockstepPacket {
    uint32_t frame_number;          // 帧序号
    uint8_t command_count;          // 命令数量
    struct Command {
        uint8_t type;               // 0=Move, 1=Attack, 2=Build, 3=Carve
        uint16_t unit_ids[64];      // 批量单位 ID
        float x, y, z;              // 目标位置
        uint16_t target_id;         // 攻击目标
    } commands[8];                  // 每帧最多 8 个命令
};
// 大小：~100 字节/帧，60fps = 6 KB/s
```

### 确定性要求

**必须确定性：**
- ✅ 地形生成（Perlin 噪声用固定种子）
- ✅ 单位寻路（A* 相同代价函数）
- ✅ 战斗计算（整数伤害，固定随机种子）
- ✅ 物理模拟（固定时间步长）

**禁止非确定性：**
- ❌ `std::unordered_map` 遍历（改用 `std::map`）
- ❌ 浮点数不同优化等级（编译器统一选项）
- ❌ 系统时间 `time()` （用游戏 tick）
- ❌ 多线程无序执行（关键路径单线程）

### 实现清单

1. **确定性地图生成**
   ```cpp
   struct GameInit {
       uint64_t terrain_seed;
       uint32_t map_size;
       float height_scale;
   };
   ```

2. **命令队列同步**
   ```cpp
   class LockstepEngine {
       std::map<uint32_t, std::vector<Command>> frame_commands; // 有序
       uint32_t current_frame = 0;
       uint32_t confirmed_frame = 0; // 所有玩家确认到此帧
       
       void add_local_command(Command cmd);
       void receive_remote_commands(uint32_t frame, std::vector<Command> cmds);
       bool can_simulate_frame(uint32_t frame); // 是否收到所有玩家此帧命令
       void simulate_frame(uint32_t frame);
   };
   ```

3. **延迟隐藏**
   - 本地输入延迟 3-5 帧执行（给网络时间传输）
   - 预测显示（本地立即显示，等服务器确认）
   - 回滚纠正（服务器命令冲突时）

4. **同步检测**
   ```cpp
   struct SyncCheck {
       uint32_t frame;
       uint32_t hash; // 所有单位位置/HP 哈希
   };
   // 每 60 帧发送一次，不匹配则同步失败
   ```

### 优点
- 带宽：**5-10 KB/s**（仅命令）
- 延迟：**无感知**（本地预测）
- 服务器：**仅转发**（无计算）

### 缺点
- 一个人卡 = 所有人等
- 不支持中途加入
- 作弊检测困难

---

## 方案 B: 混合同步（3-8 人）

### 原理
- **地形：** 锁步（雕刻命令同步）
- **单位：** 快照（服务器广播状态）
- **服务器：** 轻量级权威（不运算游戏逻辑，只验证+转发）

### 数据流

```
客户端                  服务器                    客户端
  |                       |                         |
  |--- [雕刻命令] -------->|                         |
  |                       |--- [广播雕刻] --------->|
  |                       |                         |
  |--- [单位移动命令] ---->|                         |
  |                       | [验证合法性]            |
  |                       |--- [状态快照] --------->|
  |<--- [状态快照] --------|                 (20次/秒)
  |                       |                         |
  [本地模拟 60fps]         |         [插值显示 60fps]
  [应用服务器快照纠正]      |         [应用服务器快照纠正]
```

### 协议结构

#### 1. 初始化
```cpp
struct GameInit {
    uint64_t terrain_seed;
    uint32_t map_size;
    uint8_t player_count;
    struct PlayerInfo {
        uint32_t player_id;
        char name[32];
        float start_x, start_z;
    } players[8];
};
```

#### 2. 单位快照（每 50ms）
```cpp
struct UnitSnapshot {
    uint16_t unit_count;
    struct UnitState {
        uint32_t id;
        uint16_t type;              // 单位类型
        float x, y, z;              // 位置（12 字节）
        int16_t hp;                 // 生命值
        uint8_t state;              // 状态：0=Idle, 1=Moving, 2=Attacking
        uint32_t target_id;         // 攻击目标 ID
    } units[MAX_VISIBLE_UNITS];     // 视野裁剪后
};
// 1000 单位 × 24 字节 = 24 KB × 20次/秒 = 480 KB/s
```

#### 3. 增量压缩（优化版）
```cpp
struct DeltaSnapshot {
    uint32_t base_frame;            // 基准帧
    uint16_t changed_count;         // 变化的单位数
    struct UnitDelta {
        uint32_t id;
        uint8_t fields;             // 位掩码：bit0=pos, bit1=hp, bit2=state
        float x, y, z;              // 仅当 fields & 0x01
        int16_t hp;                 // 仅当 fields & 0x02
        uint8_t state;              // 仅当 fields & 0x04
    } deltas[500];                  // 最多 500 个变化
};
// 静止单位不传输，平均 100-200 单位变化 = 5-10 KB × 20次/秒 = 100-200 KB/s
```

#### 4. 雕刻命令（锁步）
```cpp
struct CarveCommand {
    uint32_t frame;                 // 执行帧号
    uint8_t op;                     // 0=Dig, 1=Fill
    float center_x, center_y, center_z;
    float radius;
    uint32_t player_id;
};
// 稀疏事件，平均 1-2 KB/s
```

### 客户端预测 + 插值

#### 输入预测
```cpp
class ClientPredictor {
    std::vector<Command> unconfirmed_commands; // 等待服务器确认
    
    void apply_local_command(Command cmd) {
        unconfirmed_commands.push_back(cmd);
        game_state.apply(cmd);          // 立即本地模拟
    }
    
    void on_server_snapshot(Snapshot snap) {
        // 回滚到服务器状态
        game_state = snap.state;
        // 重新应用未确认命令
        for (auto& cmd : unconfirmed_commands) {
            if (cmd.frame > snap.frame)
                game_state.apply(cmd);
        }
    }
};
```

#### 位置插值（60fps 显示，20Hz 更新）
```cpp
class UnitInterpolator {
    glm::vec3 pos_server;           // 服务器位置（50ms 前）
    glm::vec3 pos_target;           // 目标位置（当前快照）
    float interp_time = 0.0f;
    
    void on_snapshot(glm::vec3 new_pos) {
        pos_server = pos_target;
        pos_target = new_pos;
        interp_time = 0.0f;
    }
    
    glm::vec3 get_render_pos(float dt) {
        interp_time += dt;
        float t = glm::clamp(interp_time / 0.05f, 0.0f, 1.0f);
        return glm::mix(pos_server, pos_target, t);
    }
};
```

### 视野裁剪（FOV Culling）
```cpp
struct PlayerView {
    std::unordered_set<uint32_t> visible_units;
    
    void update_visibility(Player& player, GameState& state) {
        visible_units.clear();
        for (auto& unit : state.units) {
            if (player.can_see(unit))   // 战争迷雾检测
                visible_units.insert(unit.id);
        }
    }
};
// 每玩家只发送他能看见的单位，大幅减少带宽
```

### 优点
- 带宽：**50-150 KB/s**（可接受）
- 支持 3-8 人
- 地形同步零开销（锁步雕刻）
- 掉线可踢出/重连

### 缺点
- 需要客户端预测逻辑
- 单位数 >2000 时带宽压力

---

## 方案 C: 完全服务器权威（8+ 人）

### 原理
- 服务器运行**完整游戏模拟**
- 客户端纯显示 + 输入
- 服务器每帧验证所有操作

### 架构
```
客户端（纯渲染器）
  ↓ 输入命令
服务器（游戏引擎）
  - 地形生成
  - 单位 AI
  - 战斗计算
  - 碰撞检测
  ↓ 状态快照
客户端（插值显示）
```

### 带宽优化

#### 1. 空间分区（Interest Management）
```cpp
struct SpatialGrid {
    std::map<GridCell, std::vector<uint32_t>> cell_units;
    
    std::vector<uint32_t> get_relevant_units(Player& p) {
        // 只返回玩家周围 9 个格子的单位
        return nearby_units;
    }
};
// 10,000 单位，玩家只能看见 500 个
```

#### 2. 优先级更新
```cpp
struct UpdatePriority {
    enum Level { CRITICAL, HIGH, NORMAL, LOW };
    
    Level get_priority(Unit& unit, Player& player) {
        float dist = distance(unit.pos, player.camera);
        if (unit.in_combat) return CRITICAL;    // 20 次/秒
        if (dist < 100) return HIGH;            // 10 次/秒
        if (dist < 500) return NORMAL;          // 5 次/秒
        return LOW;                             // 1 次/秒
    }
};
```

#### 3. 压缩协议
```cpp
// 位打包：位置量化到 cm 精度
struct CompressedUnitState {
    uint32_t id;
    int16_t x_cm, y_cm, z_cm;   // ±327 米，1cm 精度
    uint8_t hp_percent;         // 0-100%
    uint8_t state : 3;          // 3 位状态
    uint8_t flags : 5;          // 5 位标志
};
// 从 24 字节降到 11 字节
```

### 服务器负载分布
```cpp
class DistributedServer {
    // 地图分区（每区独立线程）
    struct MapShard {
        AABB bounds;
        std::thread worker;
        std::vector<Unit*> units;
        
        void tick(float dt) {
            // 并行模拟各区域
        }
    };
    
    // 跨区单位通过消息队列同步
    std::map<ShardID, std::queue<Message>> shard_messages;
};
```

### 优点
- 支持 **100+ 玩家**
- 强作弊防护
- 支持实时加入/退出
- 可录像回放

### 缺点
- 服务器成本高（需运行游戏引擎）
- 延迟 50-150ms
- 实现复杂度高

---

## 自适应切换逻辑

### 启动时自动选择
```cpp
enum NetworkMode {
    LOCKSTEP,           // 2 人
    HYBRID,             // 3-8 人
    SERVER_AUTHORITATIVE  // 8+ 人
};

NetworkMode select_mode(int player_count) {
    if (player_count == 2) return LOCKSTEP;
    if (player_count <= 8) return HYBRID;
    return SERVER_AUTHORITATIVE;
}
```

### 实现共享
```cpp
class NetworkEngine {
    virtual void init(GameInit& init) = 0;
    virtual void send_command(Command cmd) = 0;
    virtual void tick(float dt) = 0;
};

class LockstepEngine : public NetworkEngine { /* ... */ };
class HybridEngine : public NetworkEngine { /* ... */ };
class AuthoritativeEngine : public NetworkEngine { /* ... */ };

// 游戏逻辑无感知切换
std::unique_ptr<NetworkEngine> net_engine;
net_engine = create_engine(select_mode(player_count));
```

---

## 实现优先级

### Phase 1: 锁步基础（2 周）
- [x] 地图种子同步
- [ ] 确定性命令队列
- [ ] 帧同步机制
- [ ] 简单预测

### Phase 2: 混合方案（3 周）
- [ ] 单位快照协议
- [ ] 增量压缩
- [ ] 客户端插值
- [ ] 视野裁剪

### Phase 3: 完全权威（4 周）
- [ ] 服务器游戏引擎
- [ ] 空间分区
- [ ] 优先级更新
- [ ] 分布式服务器

---

## 性能目标

| 指标 | 锁步 | 混合 | 权威 |
|------|------|------|------|
| 延迟 | <30ms | <80ms | <150ms |
| 带宽（上行） | 5 KB/s | 20 KB/s | 50 KB/s |
| 带宽（下行） | 5 KB/s | 150 KB/s | 300 KB/s |
| 服务器 CPU | 5% | 30% | 200% |
| 最大单位数 | 5000 | 2000 | 10000 |

---

## 测试方案

### 延迟模拟
```cpp
class NetworkSimulator {
    void add_latency(int ms) { /* 延迟队列 */ }
    void add_packet_loss(float percent) { /* 丢包模拟 */ }
    void add_jitter(int ms) { /* 抖动模拟 */ }
};
```

### 压力测试
- 2 人 × 5000 单位（锁步）
- 8 人 × 1000 单位（混合）
- 32 人 × 500 单位（权威）

### 同步验证
```cpp
// 定期发送校验和
uint32_t compute_checksum() {
    uint32_t hash = 0;
    for (auto& u : units)
        hash ^= std::hash<glm::vec3>{}(u.pos);
    return hash;
}
```

---

## 总结

**推荐路线：**
1. 先实现锁步（2 人对战立即可玩）
2. 再加混合方案（支持 3-8 人）
3. 最后做完全权威（长期目标）

**关键技术点：**
- 确定性模拟（锁步基础）
- 增量压缩（带宽优化）
- 客户端预测（延迟隐藏）
- 视野裁剪（可扩展性）

开始实现？
