# 单位卡住问题 - 调试记录

## 症状
- 两边兵力交战后，大量单位看起来卡在原地不动
- 玩家选中这些单位后右键移动无反应

## 架构概览（GPU pipeline）

每帧执行顺序：
1. CPU `upload_combat_data` → 全量上传单位数据到 GPU
2. CPU 清除到达/死亡的 move command → `upload_move_commands`
3. GPU dispatch: `spatial_hash` → `combat_shader` → `movement_shader`
4. CPU `readback_combat` → 读回 position, velocity, state, target, cooldown
5. CPU `perform_attack` → 执行实际伤害（GPU 只做 AI 决策不造成伤害）

## 已修复的问题

### 1. Movement shader 到达目标后强制清零 velocity
**文件**: `shaders/compute_movement.glsl`

**问题**: 当玩家命令的移动目标到达后（dist < 5.0），movement shader 设 `velocity = vec2(0)` 和 `state = 0`。由于 movement shader 在 combat shader 之后执行，这会覆盖 combat shader 设的 AI velocity。

**修复**: 到达后只清 `move_cmds[idx].has = 0u`，不再动 velocity/state。

### 2. 搜索半径 cap 太小
**文件**: `shaders/compute_combat.glsl`

**问题**: `find_nearest_enemy` 的 cell 搜索半径被 cap 在 8（= 160 距离），声明的搜索范围 500 根本覆盖不到。大量单位找不到目标。

**修复**: cap 从 8 提升到 20（= 400 距离）。

### 3. Idle AI 移动太慢 + patrol 减速
**文件**: `shaders/compute_combat.glsl`

**问题**: 找不到目标的 idle 单位以 speed*0.6 向敌方中心移动，靠近后再减半到 speed*0.3，视觉上几乎不动。

**修复**: 基础速度提升到 0.85，近距离不再减速，改为全速散开。

### 4. 玩家命令被 CPU 清除（控制不了）
**文件**: `src/main_gpu.cpp`

**问题**: CPU 每帧检查 has_move_command 的单位，如果 state==Attacking 或 Retreating 就清除命令。但 GPU readback 可能在玩家发命令的同一帧把 state 覆盖回 Attacking，导致下一帧 CPU 立即清掉玩家刚发的命令。

**修复**: 只在 state==Dead 时清除 move command。玩家命令优先级高于 AI。

## 当前诊断数据

修正诊断标准后（只统计 state=Moving/Idle 且 velocity<0.3 的单位）：
```
f=4860 stuck=2 alive=11243/14264  (激烈战斗中)
```
真正移动状态下卡住的单位几乎为 0。

## 仍然存在的视觉问题

大量单位处于 **Attacking 状态 (state=2)**，velocity=0 站在原地。这是正常行为（在射程内停下来攻击），但视觉上看起来像"卡住"。

可能的改进方向：
- 攻击动画/特效不明显，玩家无法区分"在攻击"和"卡住"
- Attacking 单位杀死目标后应该更快找到新目标开始移动
- 考虑给 Attacking 单位加轻微的前后摇摆动画

## 阵营人数显示

HUD 上当前显示了双方 alive 人数（红/蓝），你提到要改。
代码位置: `src/ui/hud.h` line 78-87，`draw_number` 调用直接显示 `red_alive` / `blue_alive`。

## 待清理

- `src/main_gpu.cpp` 中的 DIAG printf 诊断代码（调试完后移除）
- `fix_*.py` 临时脚本文件
