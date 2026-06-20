# MassRTS 回滚网络代码（Rollback Netcode）工业级设计

> 补充 NETWORK_ARCHITECTURE.md 的关键缺口。
> 这是真正的工业级回滚（参考 GGPO / 现代格斗游戏与 RTS 同步）。

---

## 为什么原设计的回滚不够

原文档的回滚伪代码有 3 个致命问题：

```cpp
// ❌ 错误示范
void on_server_snapshot(Snapshot snap) {
    game_state = snap.state;            // 问题1：整状态深拷贝，几千单位每帧 = 卡死
    for (auto& cmd : unconfirmed_commands)
        game_state.apply(cmd);          // 问题2：无逐帧历史，无法精确重放
}                                       // 问题3：没有"是否需要回滚"的判断，每帧都回滚
```

工业级回滚必须解决：
1. **零拷贝/低成本状态保存**（环形缓冲 + 紧凑序列化）
2. **逐帧确定性重模拟**（bit-precise replay）
3. **仅在预测错误时回滚**（输入对齐检测）

---

## 核心架构：环形帧缓冲 + 确定性重放

```
        过去 <─────────────────────────────> 现在
   frame: N-7   N-6   N-5  ...  N-1    N (当前预测帧)
          │     │     │         │      │
   状态:  [S]   [S]   [S]  ...  [S]    [S]   ← 环形缓冲保存每帧快照
   输入:  [I]   [I]   [I]  ...  [I?]   [I?]  ← 远端输入未到时用预测值 I?
                              ↑
                     收到真实输入，若与预测不符：
                     1. 回滚到该帧的 [S]
                     2. 用真实输入重模拟到当前帧
```

### 关键参数
```cpp
constexpr int ROLLBACK_WINDOW = 8;      // 最多回滚 8 帧（~133ms @60fps）
constexpr int INPUT_DELAY     = 2;      // 本地输入延迟 2 帧（平滑预测）
constexpr int FRAME_HISTORY   = 16;     // 环形缓冲容量（> ROLLBACK_WINDOW 留余量）
```

---

## 1. 状态序列化（核心 —— 决定性能）

**绝不能 `game_state = snapshot`。** 必须紧凑二进制序列化，且可池化复用。

```cpp
struct GameStateSnapshot {
    uint32_t frame;
    uint32_t checksum;                  // 用于 desync 检测

    // 紧凑 SoA 布局，避免指针/虚函数（确定性 + 缓存友好）
    std::vector<uint32_t> unit_ids;
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> velocities;
    std::vector<int16_t>   hp;
    std::vector<uint8_t>   state;
    std::vector<uint32_t>  target_ids;
    // ... 其余确定性游戏状态

    void serialize(ByteBuffer& out) const;   // 写入预分配缓冲
    void deserialize(ByteBuffer& in);
    size_t byte_size() const;
};

// 环形缓冲，预分配，零运行时分配
class StateRingBuffer {
    std::array<GameStateSnapshot, FRAME_HISTORY> buffer;
public:
    GameStateSnapshot& at(uint32_t frame) {
        return buffer[frame % FRAME_HISTORY];
    }
    void save(uint32_t frame, const GameState& gs) {
        gs.snapshot_into(buffer[frame % FRAME_HISTORY]); // 原地写，不分配
    }
    void restore(uint32_t frame, GameState& gs) {
        gs.load_from(buffer[frame % FRAME_HISTORY]);
    }
};
```

**性能要点：**
- SoA（Structure of Arrays）布局 —— 序列化时整块 memcpy
- 预分配环形缓冲 —— 运行时零堆分配
- 仅保存"确定性逻辑状态"，**不保存渲染/插值/特效数据**

---

## 2. 输入预测与对齐

```cpp
struct PlayerInput {
    uint32_t frame;
    uint8_t  player_id;
    std::vector<Command> commands;      // 该帧该玩家的命令
    bool     is_predicted = false;      // true=本地猜测，false=真实收到
};

class InputManager {
    // [player][frame % FRAME_HISTORY] = 输入
    std::array<std::array<PlayerInput, FRAME_HISTORY>, MAX_PLAYERS> inputs;

    // 预测策略：复用上一帧的输入（RTS 命令稀疏，多数帧无新命令）
    PlayerInput predict(uint8_t player, uint32_t frame) {
        PlayerInput p = last_known_input(player);
        p.frame = frame;
        p.is_predicted = true;
        p.commands.clear();             // RTS 默认：无新命令最可能正确
        return p;
    }

    // 返回需要回滚到的最早帧（-1 表示无需回滚）
    int32_t on_remote_input(PlayerInput real) {
        auto& slot = inputs[real.player_id][real.frame % FRAME_HISTORY];
        if (slot.is_predicted && !inputs_equal(slot, real)) {
            slot = real;
            return real.frame;          // 预测错误 → 从此帧回滚
        }
        slot = real;
        return -1;                      // 预测正确 → 无需回滚
    }
};
```

**关键洞察：RTS 与格斗游戏不同**
- 格斗游戏：每帧都有输入，预测错误率高
- RTS：命令稀疏（大部分帧玩家什么都没按）→ "预测无命令" 几乎总是对的
- **结果：RTS 回滚实际触发率极低，性能开销极小**

---

## 3. 回滚 + 重模拟主循环

```cpp
class RollbackEngine {
    GameState        game;              // 当前模拟状态
    StateRingBuffer  history;
    InputManager     input_mgr;
    uint32_t         current_frame = 0;
    uint32_t         confirmed_frame = 0;   // 所有玩家真实输入已到达的最高帧

    void advance_frame() {
        // 1. 保存当前帧状态（回滚点）
        history.save(current_frame, game);

        // 2. 收集本帧所有玩家输入（缺失则预测）
        auto inputs = gather_inputs(current_frame);

        // 3. 确定性模拟一帧
        game.simulate(inputs, FIXED_DT);   // 必须 bit-precise 确定性

        current_frame++;
    }

    // 收到远端真实输入时调用
    void on_remote_input(PlayerInput real) {
        int32_t rollback_to = input_mgr.on_remote_input(real);
        if (rollback_to < 0) return;       // 预测正确，无需回滚

        // === 回滚发生 ===
        uint32_t saved_frame = current_frame;

        // 1. 恢复到错误帧的状态
        history.restore(rollback_to, game);
        current_frame = rollback_to;

        // 2. 用真实输入重模拟到当前
        while (current_frame < saved_frame) {
            history.save(current_frame, game);     // 重新保存（输入已修正）
            auto inputs = gather_inputs(current_frame);
            game.simulate(inputs, FIXED_DT);
            current_frame++;
        }
        // 回滚完成，状态已纠正，渲染层下一帧自然平滑过渡
    }
};
```

**性能分析：**
- 回滚成本 = 回滚帧数 × 单帧模拟成本
- RTS 命令稀疏 → 回滚极少发生 → 平均开销 < 1% CPU
- 最坏情况 = 8 帧重模拟 = 一次性 8× 单帧成本（< 16ms，玩家无感）

---

## 4. Desync（不同步）检测与恢复

工业级必须有，否则一旦不同步整局作废。

```cpp
class DesyncDetector {
    // 每 N 帧交换校验和
    static constexpr int CHECK_INTERVAL = 30;

    struct ChecksumRecord {
        uint32_t frame;
        uint32_t checksum;
    };
    std::map<uint32_t, uint32_t> local_checksums;   // 我的校验和历史

    uint32_t compute_checksum(const GameState& g) {
        uint32_t h = 2166136261u;       // FNV-1a，确定性
        for (size_t i = 0; i < g.unit_count(); i++) {
            h = fnv_mix(h, g.id(i));
            h = fnv_mix(h, quantize(g.pos(i)));   // 量化避免浮点噪声
            h = fnv_mix(h, g.hp(i));
        }
        return h;
    }

    // 收到对方校验和，比对
    void on_remote_checksum(uint32_t frame, uint32_t remote) {
        auto it = local_checksums.find(frame);
        if (it != local_checksums.end() && it->second != remote) {
            // 不同步！工业级处理：
            // 1) 记录日志（帧号、双方校验和）
            // 2) 触发状态重传（HYBRID/权威模式可从服务器拉取权威快照）
            // 3) 纯锁步模式只能中止对局并报错
            on_desync_detected(frame);
        }
    }
};
```

**浮点确定性陷阱（必须处理）：**
- 同一份代码在不同 CPU/编译器/优化等级下浮点结果可能不同
- 解决方案（择一）：
  - **定点数（fixed-point）** —— 最稳，工业级 RTS 常用（如《帝国时代》）
  - **统一编译选项** `-ffp-contract=off -fno-fast-math` + 同一架构
  - **软件浮点库** —— 跨平台最稳但最慢
- **推荐：关键逻辑（位置/战斗）用定点数，表现层用浮点**

---

## 5. 与三种网络模式的关系

| 模式 | 回滚角色 |
|------|----------|
| **锁步 (2P)** | 核心机制。本地预测 + 远端输入到达时回滚纠正 |
| **混合 (3-8P)** | 单位用服务器快照纠正（轻量回滚到快照帧重放本地命令） |
| **权威 (8+P)** | 客户端纯预测，服务器快照为准，回滚仅用于本地预测纠正 |

**统一实现：** 三种模式共用 `RollbackEngine` 的"保存-恢复-重放"机制，
区别只在"权威输入来源"（对等输入 vs 服务器快照）。

---

## 6. 实现清单（补充到 Phase 1）

### Phase 1.5: 回滚基础（插入到锁步之后）
- [ ] `GameStateSnapshot` SoA 序列化（serialize/deserialize）
- [ ] `StateRingBuffer` 预分配环形缓冲
- [ ] `GameState::snapshot_into` / `load_from`（零分配）
- [ ] 确定性模拟验证（同输入 → 同校验和）
- [ ] `InputManager` 预测 + 对齐
- [ ] `RollbackEngine` 回滚重放循环
- [ ] `DesyncDetector` 校验和交换
- [ ] **定点数迁移**（位置/战斗逻辑）—— 跨平台同步前必做

### 验证测试
```cpp
// 测试1：确定性 —— 同种子同输入，两个实例校验和必须逐帧一致
// 测试2：回滚正确性 —— 注入延迟输入，回滚后状态 == 无延迟基准
// 测试3：性能 —— 最坏 8 帧回滚 5000 单位，单次 < 16ms
// 测试4：长时间 —— 10 分钟对局无 desync
```

---

## 7. 工业级评估对照表

| 工业级要素 | 原设计 | 本文档补充后 |
|-----------|--------|-------------|
| 分层自适应同步 | ✅ | ✅ |
| 增量压缩/视野裁剪 | ✅ | ✅ |
| **环形帧缓冲** | ❌ | ✅ |
| **零拷贝状态保存** | ❌ | ✅ |
| **确定性重模拟** | ❌ | ✅ |
| **输入预测对齐** | ⚠️ 粗糙 | ✅ |
| **Desync 检测** | ⚠️ 提及未设计 | ✅ |
| **浮点确定性方案** | ❌ | ✅（定点数）|
| **状态重传恢复** | ❌ | ✅ |

---

## 结论

**补充后达到工业级。** 关键新增：
1. 真正的回滚（环形缓冲 + 确定性重放，非整状态拷贝）
2. 浮点确定性（定点数）—— 这是跨平台同步的**生死线**
3. Desync 检测与恢复

**最重要的认知：** RTS 命令稀疏，所以回滚开销远低于格斗游戏，
"预测无新命令"几乎总是正确 —— 这让高性能锁步在 RTS 上完全可行。

**实现顺序调整：**
- Phase 1：锁步基础（命令同步）
- **Phase 1.5：回滚 + 定点数（本文档）← 新增，必做**
- Phase 2：混合方案
- Phase 3：完全权威
