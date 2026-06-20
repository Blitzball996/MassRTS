#!/usr/bin/env python3
"""
Apply network integration patches to MassRTS
Run this after closing Visual Studio
"""
import sys
import os

def patch_cmake():
    """Add test file exclusions to CMakeLists.txt"""
    path = "CMakeLists.txt"
    with open(path, 'r', encoding='utf-8') as f:
        lines = f.readlines()
    
    # Find CPU_SOURCES section (around line 46-48)
    for i, line in enumerate(lines):
        if 'list(FILTER CPU_SOURCES EXCLUDE REGEX "main_gpu' in line:
            # Check if already patched
            if i+1 < len(lines) and 'test_' in lines[i+1]:
                print("✓ CMakeLists.txt already patched")
                return
            # Insert exclusions
            lines.insert(i+1, 'list(FILTER CPU_SOURCES EXCLUDE REGEX "src/net/test_.*\\.cpp$")\n')
            lines.insert(i+2, 'list(FILTER CPU_SOURCES EXCLUDE REGEX "src/net/rts/example_usage\\.cpp$")\n')
            break
    
    # Find GPU_SOURCES section
    for i, line in enumerate(lines):
        if 'list(FILTER GPU_SOURCES EXCLUDE REGEX "src/main' in line:
            if i+1 < len(lines) and 'test_' in lines[i+1]:
                break  # already patched
            lines.insert(i+1, 'list(FILTER GPU_SOURCES EXCLUDE REGEX "src/net/test_.*\\.cpp$")\n')
            lines.insert(i+2, 'list(FILTER GPU_SOURCES EXCLUDE REGEX "src/net/rts/example_usage\\.cpp$")\n')
            break
    
    with open(path, 'w', encoding='utf-8') as f:
        f.writelines(lines)
    print("✓ CMakeLists.txt patched")

def patch_main():
    """Add network integration to main.cpp"""
    path = "src/main.cpp"
    with open(path, 'r', encoding='utf-8') as f:
        content = f.read()
    
    # Check if already patched
    if 'net_integration.h' in content:
        print("✓ main.cpp already patched")
        return
    
    # 1. Add include after other includes (around line 20)
    include_pos = content.find('#include "ui/menu.h"')
    if include_pos == -1:
        include_pos = content.find('#include "ai/movement_system.h"')
    include_insert = content.find('\n', include_pos) + 1
    content = content[:include_insert] + '#include "net_integration.h"\n' + content[include_insert:]
    
    # 2. Add global NetworkState pointer (after other globals, around line 30)
    global_pos = content.find('Camera g_camera;')
    global_insert = content.find('\n', global_pos) + 1
    content = content[:global_insert] + 'NetworkState* g_net_state = nullptr;\n' + content[global_insert:]
    
    # 3. Add argument parsing in main (after exe_path line)
    main_start = content.find('int main(int argc, char* argv[]) {')
    exe_path_line = content.find('std::string exe_path = argv[0];', main_start)
    parse_insert = content.find('\n', exe_path_line) + 1
    
    arg_parse = '''    
    // === Network Setup ===
    bool is_server = false, is_client = false;
    std::string server_ip = "127.0.0.1", player_name = "Player";
    uint16_t server_port = 27015;
    
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--server" || arg == "-s") {
            is_server = true;
            if (i+1 < argc && argv[i+1][0] != '-') server_port = (uint16_t)std::atoi(argv[++i]);
        } else if (arg == "--client" || arg == "-c") {
            is_client = true;
            if (i+1 < argc && argv[i+1][0] != '-') server_ip = argv[++i];
            if (i+1 < argc && argv[i+1][0] != '-') server_port = (uint16_t)std::atoi(argv[++i]);
        } else if (arg == "--name" || arg == "-n") {
            if (i+1 < argc) player_name = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "MassRTS Multiplayer\\n  MassRTS.exe --server [port]\\n  MassRTS.exe --client <ip> [port]\\n  --name <name>\\n";
            return 0;
        }
    }
    #ifdef _WIN32
    UDPSocket::init_network();
    #endif
'''
    content = content[:parse_insert] + arg_parse + content[parse_insert:]
    
    # 4. Initialize NetworkState after World creation
    world_create = content.find('World* world_ptr = new World();')
    world_end = content.find('World& world = *world_ptr;')
    net_init_pos = content.find('\n', world_end) + 1
    
    net_init = '''
    // === Network Integration ===
    NetworkState net_state;
    g_net_state = &net_state;
    if (is_server && !net_state.startHost(server_port, player_name)) { std::cerr << net_state.connection_status << "\\n"; return -1; }
    if (is_client && !net_state.startClient(server_ip, server_port, player_name)) { std::cerr << net_state.connection_status << "\\n"; return -1; }
    if (net_state.isMultiplayer()) std::cout << "[Net] " << net_state.connection_status << "\\n";
'''
    content = content[:net_init_pos] + net_init + content[net_init_pos:]
    
    # 5. Hook right-click move to send network commands
    rmb_block = 'world.commands.has_move_command[e] = true;'
    rmb_pos = content.find(rmb_block)
    if rmb_pos > 0:
        # Find end of the for loop (after state assignment)
        state_line = content.find('world.units.state[e] = UnitState::Moving;', rmb_pos)
        block_end = content.find('\n', state_line) + 1
        # Insert network send after the formation loop
        inject_net = '''            }
            // Send move command to network (if multiplayer)
            if (g_net_state && g_net_state->isMultiplayer() && !selected.empty()) {
                g_net_state->queueMoveCommand(target, selected.front(), selected.back() + 1);
'''
        content = content[:block_end] + inject_net + content[block_end:]
    
    # 6. Add network update in game loop (after glfwPollEvents)
    poll_pos = content.find('glfwPollEvents();')
    poll_end = content.find('\n', poll_pos) + 1
    content = content[:poll_end] + '        if (net_state.isMultiplayer()) net_state.update(world, dt);\n' + content[poll_end:]
    
    # 7. Disable enemy AI in multiplayer
    enemy_ai_start = content.find('enemy_buy_timer += dt;')
    if enemy_ai_start > 0:
        line_start = content.rfind('\n', 0, enemy_ai_start) + 1
        indent = content[line_start:enemy_ai_start]
        content = content[:line_start] + indent + 'if (!net_state.isMultiplayer()) {\n' + content[line_start:]
        # Find the closing of enemy AI block (next top-level block)
        enemy_end = content.find('// === Combat AI ===', enemy_ai_start)
        if enemy_end > 0:
            block_end = content.rfind('\n', 0, enemy_end)
            content = content[:block_end] + '\n        }\n' + content[block_end:]
    
    # 8. Cleanup before return
    return_pos = content.rfind('return 0;')
    cleanup_pos = content.rfind('\n', 0, return_pos)
    content = content[:cleanup_pos] + '''
    g_net_state = nullptr;
    #ifdef _WIN32
    UDPSocket::shutdown_network();
    #endif
''' + content[cleanup_pos:]
    
    with open(path, 'w', encoding='utf-8') as f:
        f.write(content)
    print("✓ main.cpp patched")

if __name__ == '__main__':
    os.chdir(os.path.dirname(os.path.abspath(__file__)))
    
    print("=== MassRTS Network Integration Patcher ===\n")
    
    try:
        patch_cmake()
        patch_main()
        print("\n✓ All patches applied successfully!")
        print("\nNext steps:")
        print("  1. Open CMake GUI or run: cmake -B build")
        print("  2. Build: cmake --build build --config Release")
        print("  3. Test:")
        print("     Terminal 1: build/Release/MassRTS.exe --server")
        print("     Terminal 2: build/Release/MassRTS.exe --client 127.0.0.1")
    except Exception as e:
        print(f"\n✗ Error: {e}")
        sys.exit(1)
