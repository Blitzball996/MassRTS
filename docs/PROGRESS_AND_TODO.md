# MassRTS 画面/玩法改造 — 进度与待办

> 本文记录当前会话所有已完成的改动、根因分析，以及剩余待办。下次接手直接看这里。

---

## 一、运行/构建信息

- 实际运行的可执行：`build/Release/MassRTS_GPU.exe`（由 `src/main_gpu.cpp` 编译）
- 构建：`cd build && cmake --build . --config Release --target MassRTS_GPU`
- Shader 改完必须拷贝到运行目录（两处都要）：
  - `build/Release/shaders/`
  - `build/Release/shaders/shaders/`
- 性能诊断日志：`build/Release/diag.log`
  - 字段：`fps / ent / live / proj | grid / upload / disp / read / cpu / gpuMS (ms/s)`

---

## 二、已完成（已编译通过 + 冒烟测试可启动）

### 1. 快速修复组 ✅
- **Win 键/开始菜单卡顿**：`apply_fullscreen()`（main_gpu.cpp ~L105）由
  独占全屏（`glfwSetWindowMonitor(mon,...)`）改为**无边框窗口化全屏**。
  独占全屏会霸占显示器、置顶、屏蔽 Win 键与 Alt-Tab，尤其在 loading 时很明显。
- **箭矢特效变细**：`shaders/projectile.vert`（宽度系数 + stretch）
  和 `projectile.frag`（细箭杆 + 尖箭头 + 尾羽），`projectiles.h` 中
  Arrow 的 `stretch` 4→7。
- **雪地脏条纹**：根因是 `sdf_terrain.h::original_height_at()` 用最近邻采样，
  对平滑 marching-cubes 网格产生阶梯状高度参考，使 `depth=surf-p.y` 在未挖
  的雪坡上误判为正，触发地下泥土/岩石 strata 覆盖。改为**双线性采样**，并把
  `terrain.frag` 的 depth 阈值 0.5→1.5。

### 2. 音频组（真实音频文件框架）✅
- 重写 `src/audio/audio_system.h`：用 miniaudio 高层 `ma_engine` 加载真实文件
  （mp3/ogg/wav/flac 都能解码），保留程序化战场氛围作为**兜底**（无文件也不静音）。
- 期望的文件（放 `assets/audio/`，缺失则静默跳过）：
  - 音乐：`music_menu.*`、`music_battle.*`（循环）
  - 音效：`ui_click.*`、`ui_hover.*`、`sfx_arrow.*`、`sfx_cannon.*`、`sfx_nuke.*`、`sfx_explosion.*`
- 已在 main_gpu.cpp 接线：菜单进 `play_music(0)`、开战 `play_music(1)`、
  返回菜单切回菜单乐、按钮点击 `play_click()`、设置滑条实时 `set_volumes()`。
- 已建 `assets/audio/README.md` 说明文件命名。

<!-- APPEND_MARKER -->
