# MassRTS 网络多人游戏集成方案

## 当前状态
- ✅ 网络模块已完整实现（`src/net/`）
  - GameServer/GameClient (UDP lockstep)
  - RtsNetEngine (高层封装)
  - 协议、压缩、持久化全部就绪
- ✅ 集成桥接层已创建（`src/net_integration.h`）
- ❌ 主程序尚未集成（main.cpp 被 VS 锁定）
- ❌ CMakeLists.txt 需要排除测试文件（被锁定）

## 需要的修改

### 1. CMakeLists.txt 修改（第 46-48 行）
```cmake
# 原代码：
# CPU target sources (exclude main_gpu.cpp)
set(CPU_SOURCES ${ALL_SOURCES})
list(FILTER CPU_SOURCES EXCLUDE REGEX "main_gpu\.cpp$")

# 改为：
# CPU target sources (exclude main_gpu.cpp and test files)
set(CPU_SOURCES ${ALL_SOURCES})
list(FILTER CPU_SOURCES EXCLUDE REGEX "main_gpu\.cpp$")
list(FILTER CPU_SOURCES EXCLUDE REGEX "src/net/test_.*\.cpp$")
list(FILTER CPU_SOURCES EXCLUDE REGEX "src/net/rts/example_usage\.cpp$")
```

同样，GPU_SOURCES 也加这两行过滤（第 50-52 行后）。

### 2. main.cpp 修改

#### 2.1 添加 include（第 20 行后，在其他 include 之后）
```cpp
#include "net_integration.h"
```

#### 2.2 main 函数开头添加命令行解析（第 288-289 行，在 exe_path 后）
```cpp
int main(int argc, char* argv[]) {
    std::string exe_path = argv[0];
    
    // === Network Setup ===
    bool is_server = false;
    bool is_client = false;
    std::string server_ip = "127.0.0.1";
    uint16_t server_port = 27015;
    std::string player_name = "Player";
    
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--server" || arg == "-s") {
            is_server = true;
            if (i + 1 < argc && argv[i+1][0] != '-') {
                server_port = (uint16_t)std::atoi(argv[++i]);
            }
        } else if (arg == "--client" || arg == "-c") {
            is_client = true;
            if (i + 1 < argc && argv[i+1][0] != '-') {
                server_ip = argv[++i];
            }
            if (i + 1 < argc && argv[i+1][0] != '-') {
                server_port = (uint16_t)std::atoi(argv[++i]);
            }
        } else if (arg == "--name" || arg == "-n") {
            if (i + 1 < argc) {
                player_name = argv[++i];
            }
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "MassRTS Multiplayer\n";
            std::cout << "Usage:\n";
            std::cout << "  MassRTS.exe                      - Single player\n";
            std::cout << "  MassRTS.exe --server [port]      - Host (default: 27015)\n";
            std::cout << "  MassRTS.exe --client <ip> [port] - Join server\n";
            std::cout << "  MassRTS.exe --name <name>        - Set player name\n";
            return 0;
        }
    }
    
    // Initialize network sockets (Windows)
    #ifdef _WIN32
    UDPSocket::init_network();
    #endif
    
    // 继续原有的 if (!glfwInit()) ...
```

#### 2.3 创建 NetworkState（第 311 行，在 World* world_ptr 后）
```cpp
    World* world_ptr = new World();
    g_world = world_ptr;
    World& world = *world_ptr;
    
    // === Network Integration ===
    NetworkState net_state;
    if (is_server) {
        if (!net_state.startHost(server_port, player_name)) {
            std::cerr << net_state.connection_status << "\n";
            return -1;
        }
        std::cout << "[Multiplayer] " << net_state.connection_status << "\n";
    } else if (is_client) {
        if (!net_state.startClient(server_ip, server_port, player_name)) {
            std::cerr << net_state.connection_status << "\n";
            return -1;
        }
        std::cout << "[Multiplayer] " << net_state.connection_status << "\n";
    }
```

#### 2.4 游戏主循环网络轮询（第 367 行，在 glfwPollEvents() 后）
```cpp
        glfwPollEvents();
        
        // === Network Update ===
        if (net_state.isMultiplayer()) {
            net_state.update(world, net_state.current_sim_tick);
        }
        
        g_mouse_clicked_this_frame = false;
```

#### 2.5 禁用多人模式下的敌方 AI（第 378 行，enemy_buy_timer 逻辑前）
```cpp
        // Enemy auto-buy (disabled in multiplayer)
        if (!net_state.isMultiplayer()) {
            enemy_buy_timer += dt;
            if (enemy_buy_timer > 8.0f && combat->faction_alive[1] < 20000) {
                // ... 原有的敌方 AI 购买逻辑
            }
        }
```

#### 2.6 右键移动命令同步到网络（在 mouse_button_callback 中，~第 170 行处理右键的地方）
找到这段：
```cpp
if (button == GLFW_MOUSE_BUTTON_RIGHT && action == GLFW_PRESS) {
    // ... 计算 target_world ...
    for (uint32_t i = 0; i < g_world->entity_count; i++) {
        if (g_world->selection.selected[i] && g_world->is_alive(i)) {
            g_world->units.target_position[i] = glm::vec2(target_world.x, target_world.z);
            // ...
        }
    }
}
```

改为：
```cpp
if (button == GLFW_MOUSE_BUTTON_RIGHT && action == GLFW_PRESS) {
    // ... 原有的 target_world 计算 ...
    
    // 找到选中单位的 ID 范围
    uint32_t first_selected = INVALID_ENTITY, last_selected = 0;
    for (uint32_t i = 0; i < g_world->entity_count; i++) {
        if (g_world->selection.selected[i] && g_world->is_alive(i)) {
            if (first_selected == INVALID_ENTITY) first_selected = i;
            last_selected = i + 1;
        }
    }
    
    // 发送网络命令（单机模式下直接本地应用）
    extern NetworkState* g_net_state;  // 需要在全局声明
    if (g_net_state && g_net_state->isMultiplayer() && first_selected != INVALID_ENTITY) {
        g_net_state->queueMoveCommand(glm::vec2(target_world.x, target_world.z),
                                     first_selected, last_selected,
                                     g_net_state->current_sim_tick);
    } else {
        // 单机模式：立即应用
        for (uint32_t i = 0; i < g_world->entity_count; i++) {
            if (g_world->selection.selected[i] && g_world->is_alive(i)) {
                g_world->units.target_position[i] = glm::vec2(target_world.x, target_world.z);
                g_world->units.target[i] = INVALID_ENTITY;
                g_world->units.state[i] = UnitState::Moving;
            }
        }
    }
}
```

#### 2.7 main 开头添加全局变量（第 30 行左右，其他全局变量旁）
```cpp
NetworkState* g_net_state = nullptr;
```

在 main 中初始化后设置：
```cpp
    NetworkState net_state;
    g_net_state = &net_state;  // 设置全局指针供回调使用
```

#### 2.8 清理（main 函数 return 前）
```cpp
    g_net_state = nullptr;
    // 原有的 cleanup ...
    #ifdef _WIN32
    UDPSocket::shutdown_network();
    #endif
```

## 使用方法

### 编译后测试
```cmd
# 终端 1 - 启动服务器（host）
MassRTS.exe --server --name "Host"

# 终端 2 - 加入客户端
MassRTS.exe --client 127.0.0.1 --name "Client1"

# 终端 3 - 第二个客户端
MassRTS.exe --client 127.0.0.1 --name "Client2"
```

### 预期行为
- Host 窗口：控制红方（Faction::Red）
- Client 窗口：控制蓝方（Faction::Blue）
- 右键移动命令通过网络同步
- 敌方 AI 自动禁用（多人模式下由真人控制）

## 注意事项
1. **确定性问题**：当前游戏使用浮点数模拟（glm::vec2/vec3），不是完全确定性的。长时间运行可能出现轻微不同步（desync）。协议中使用了 fixed-point (16.16) 传输坐标，但实际模拟仍是浮点。
2. **lockstep 延迟**：INPUT_DELAY = 3 帧（~100ms @ 30Hz），用户操作会有轻微延迟。
3. **初始同步**：当前实现假设所有客户端同时启动。完整的加入中途游戏需要 GameState 全量同步（TODO）。

## 下一步改进
- [ ] 完整的 desync 检测（checksum 对比）
- [ ] 中途加入支持（发送完整世界状态）
- [ ] 单位购买命令同步
- [ ] 延迟优化（预测 + rollback）
- [ ] 聊天/游戏大厅 UI

---
**状态**：文件已准备就绪，等待 Visual Studio 关闭后应用修改。
