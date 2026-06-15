# MassRTS 美术资产工作流（模型 + 动画）

这套系统是**正规游戏引擎的做法**：一类模型配一套动画剪辑（idle/walk/attack/death），
离线导入成引擎格式，运行时 GPU 蒙皮播放。5 万单位也几乎不耗 CPU。

---

## 一、整体流程

```
  你的美术文件                离线工具                  引擎运行时
  knight.fbx        →   asset_pipeline.py    →   .mesh / .anim / .meta
  walk.fbx                (PyAssimp 解析)          ↓
  attack.fbx                                   GPU 蒙皮渲染（5万单位）
```

- **离线**：FBX/glTF → 引擎二进制格式（骨骼、蒙皮权重、动画烘培成贴图）
- **运行时**：引擎只读自定义格式，**零 Assimp 依赖**，启动快、包体小
- **GPU 蒙皮**：骨骼矩阵存在"动画纹理"里，顶点着色器采样，CPU 不算骨骼

---

## 二、准备工具（一次性）

```bash
pip install numpy pyassimp
```
还需要 Assimp 的动态库在 PATH 上（pyassimp 依赖它）。Windows 可装
`pip install pyassimp` 后把 assimp DLL 放到 PATH，或用 conda 装 assimp。

---

## 三、转换模型

### 模型自带动画（单个 FBX 里有 idle/walk 等）
```bash
python tools/asset_pipeline.py knight.fbx
```

### 模型 + 分离的动画文件（每个动作一个 FBX，常见于 Mixamo）
```bash
python tools/asset_pipeline.py knight.fbx --anims idle.fbx walk.fbx attack.fbx death.fbx
```
剪辑名取自文件名（idle/walk/attack/death）。

输出到 `assets/models/`：
- `knight.mesh`  — 几何 + 骨骼权重
- `knight.anim`  — 动画纹理（所有帧的骨骼矩阵）
- `knight.meta`  — 剪辑表（每个动作的起始帧/帧数/帧率）

---

## 四、在 manifest 里启用

编辑 `assets/manifest.json`，把兵种指向你的模型（写文件名即可，
引擎会自动找同名 `.mesh`）：
```json
{
  "models": {
    "infantry": { "file": "knight",  "scale": 1.0 },
    "cavalry":  { "file": "horseman", "scale": 1.2 },
    "archer":   { "file": "ranger",  "scale": 1.0 }
  }
}
```
- 有 `.mesh` → 走 GPU 骨骼动画
- 只有 `.obj` → 走静态网格
- 都没有 → 用引擎自带的程序化方块模型

**改完不用重新编译**，重启游戏即可。

---

## 五、当前能力与限制（务必了解）

| 项目 | 现状 |
|---|---|
| 模型格式 | FBX / glTF / GLB / OBJ / DAE（Assimp 支持的都行） |
| 独立模型槽 | **3 个**（infantry/cavalry/archer 各一套网格）；其余兵种复用或 billboard |
| 动画剪辑 | 每个模型可带多个（idle/walk/attack/death...） |
| 当前播放 | 整桶播放 locomotion（walk/idle），每单位有随机相位错开，不会齐步走 |
| 待完善 | 按单位状态（攻击/死亡）切剪辑——需把每单位 state 映射到 clip（见下） |

### 想要"按状态自动切动画"（攻击时播 attack、死亡播 death）
当前 shader 已经接收每单位 `a_inst_state`。下一步是在 `skinned.vert` 里
按 state 选择不同 clip 区间（已预留 u_clip_* uniform 架构）。这是一个
增量改动，需要时告诉我。

---

## 六、高面 / 多态性能说明

- **面数越高越慢**：5 万单位每个 2000 面 = 1 亿面，必须靠 LOD 把远处降成
  billboard。近处高模 + 远处 billboard 是关键。
- **骨骼动画几乎免费**：因为是 GPU 纹理采样蒙皮，不随单位数线性增长 CPU 开销。
  瓶颈仍是顶点数（面数），不是动画本身。
- **多套动画**：只是动画纹理更高（更多帧），显存占用略增，运行时开销几乎不变。

---

## 七、关于接入 UE5

仍是"移植非封装"。但这套资产管线的设计**和 UE5 思路一致**（离线导入 →
引擎格式 → GPU 实例化 + 动画）。若迁移 UE5，模型直接用 UE 的 Skeletal Mesh +
Animation Blueprint 即可，海量单位用 UE 的 **Vertex Animation Texture (VAT)** 或
MassEntity + Niagara，正是本管线的 UE 对应物。
