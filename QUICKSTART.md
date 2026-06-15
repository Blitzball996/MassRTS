# MassRTS 快速上手

## 最快启动方式

**双击 `PLAY.bat`** —— 就这么简单。

- 如果游戏没编译过，它会自动先编译再启动。
- 如果已经编译好，直接开始游戏。

---

## 如果是第一次（需要先装好环境）

需要两样东西（一次性安装）：

1. **Visual Studio 2022**（Community 免费版即可）— 安装时勾选"使用 C++ 的桌面开发"
2. **CMake**（https://cmake.org/download/）— 安装时选"Add to PATH"

装好后，**双击 `build.bat`** 编译，再双击 `PLAY.bat` 玩。

> 也可以命令行：
> ```
> cmake -B build -G "Visual Studio 17 2022" -A x64
> cmake --build build --config Release --target MassRTS_GPU
> ```

---

## 游戏操作

### 镜头
| 按键 | 作用 |
|---|---|
| `W A S D` | 平移镜头 |
| `Q` / `E` | 旋转镜头 |
| 鼠标滚轮 | 缩放 |
| `ESC` | 退出 |

### 战斗 / 单位
| 操作 | 作用 |
|---|---|
| 鼠标左键拖拽 | 框选己方单位 |
| 鼠标右键 | 命令选中单位移动/攻击 |
| `N` | 发射核弹（就绪时） |
| 屏幕上的购买按钮 / `+` `=` 键 | 增加购买数量、买兵 |

### 地形雕刻
| 按键 | 作用 |
|---|---|
| `B` | 开/关雕刻模式（屏幕底部出现笔刷面板） |
| `1` `2` `3` `4` | 隆起 / 挖坑 / 平滑 / 压平 |
| `[` `]` | 缩小 / 放大笔刷 |
| 按住左键拖动 | 在地面涂抹改地形（消耗金钱） |

---

## 换美术素材（不用编程）

1. 编辑 `assets/manifest.json` 指定模型 / 缩放 / 特效颜色
2. 换 3D 模型（含动画）：见 `ART_WORKFLOW.md`
3. 换音效 / 地形 / 特效细节：见 `CUSTOMIZATION.md`

改完**重启游戏**即可生效，无需重新编译。

---

## 文件结构速查

```
MassRTS/
  PLAY.bat            ← 双击开始游戏
  build.bat           ← 双击编译
  assets/
    manifest.json     ← 美术资产配置（改这里换素材）
    models/           ← 放你的 .obj / .mesh 模型
    audio/            ← 放你的 .wav 音效
  shaders/            ← 渲染着色器
  src/                ← 源代码
  tools/
    asset_pipeline.py ← FBX/glTF 转换工具
  build/Release/
    MassRTS_GPU.exe   ← 编译产物（实际运行的程序）
```

---

## 常见问题

**Q: 双击 PLAY.bat 闪退？**
A: 多半是没编译。先双击 `build.bat`，看有没有报错。

**Q: 提示缺 CMake / 编译器？**
A: 装 Visual Studio 2022 + CMake（见上面"第一次"）。

**Q: 帧率低？**
A: 关掉其他占用显卡的程序。5 万单位需要独立显卡。也可以在源码里
   调小 `MAX_LIVE_UNITS`（`src/ecs/world.h`）降低规模。

**Q: 黑屏？**
A: 看 `build/Release/diag.log`，里面记录了每秒的帧率和单位数，
   出问题时会记录 GPU 状态。
