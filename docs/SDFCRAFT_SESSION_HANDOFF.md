# sdfCraft 地形渲染会话交接文档（HANDOFF）

> 用途：在**新对话**里读这份文档即可接上全部上下文，无需回看旧对话。
> 项目：MassRTS / sdfCraft，路径 `G:\CMakePJ\MassRTS`，分支 `feat/sdfcraft-marching-cubes`
> 目标：做"SDF 版 Minecraft"——平滑/自然的可破坏体素地形（不是方块）。
> 配套理论文档：`docs/SDF_TERRAIN_REALISM.md`（真实感原理 + 新颖性/商业评估）

---

## 0. 立刻要知道的（TL;DR）

- 引擎：SDF（signed distance field）+ Marching Cubes 网格化，可挖掘/填充。可执行 `build/Release/SDFCraft.exe`（源 `src/main_sdfcraft.cpp`）。
- 这次会话**从头到尾在修地形画质**：条纹、怪多边形、挖掘材质、树木、灰片、折页。**全部已修并提交**。
- **最重要的两条"别再踩"**：
  1. **材质别用逐顶点材质码**（会乱色"怪多边形"）。earthy 地形用**逐顶点深度**编码，特殊材质用 `200+code`。
  2. **挖掘 carve 必须有 anti-plateau clamp**（`op<0 && sphere>0 && cur<0 → nv=cur`），否则反复挖出现**扁平灰片/板**。
- 项目记忆：`C:\Users\Administrator\.claude\projects\C--Users-Administrator\memory\sdfcraft-terrain-rendering.md`（已记录所有根因）。

---

## 1. 构建与测试

```bash
# 构建（改了 .h/.cpp 必须重编；shader 改了运行时加载，无需重编）
cd /g/CMakePJ/MassRTS && cmake --build build --config Release --target SDFCraft

# headless 截图测试
./build/Release/SDFCraft.exe --shot out.png --frames N [--dig] --fly --pos X Y Z --look YAW PITCH SEED
# 例：总览  --frames 100 --fly --pos 40 135 40 --look 225 -30 1337
# 例：挖洞  --frames 150 --dig --pos 50 85 50 --look 20 -48 1337
```

**测试硬限制（关键）**：headless 相机被钉死、挖掘 raycast 只有 6 格，**无法下到深洞内部**。所以"深挖的灰片/折页/侵蚀感"这类**只能靠用户在真实游戏里验证**，我自己截不到。别把"我截图看着没问题"当成深挖场景已验证。
另注：cmake 有个 `pwsh.exe 不是命令` 的 post-build 报错，**不影响 exe 生成**，忽略即可。

**像素级诊断法**（屡试不爽）：用 PIL 数像素分类颜色，别靠肉眼猜。还可临时改 shader 把材质类别输出成纯色（earthy=红/特殊=黄/水=蓝）来定位某个面到底是什么材质——本会话靠这招定位了"绿卡片=树残片""灰片=特殊材质分支"。

---

## 2. 架构速览

- **SDF 场**：`world.h::analytic_field = (y - surface_height_f(x,z)) * inv`（梯度归一化）。负=固体正=空气。`surface_height_f` = 5-oct fBm + 山脉项。
- **carve**：`world.h::carve_sphere`，smin/smax 球并集，写 per-chunk float SDF 覆盖层；未触动体素回退解析场。
- **MCMesher**（`mc_mesher.h`）：自然地形等值面。顶点 10 floats `(pos3,normal3,color3,mat1)`。法线=SDF 梯度中心差分（步长 1.0）。
- **Cube mesher**（`mesher.h`）：物体方块（原木/叶/水/玻璃/放置块），立方体，不透明+透明两段。
- **着色**（`shaders/sdfcraft_chunk.frag`）：triplanar 纹理 + hemisphere 环境光 + 仅水高光 + 雾 + tonemap。
- **生成**：`world.h::generate`，顶块 `surface_block`（grass/snow 高山/sand 海滩），下面 dirt/stone，矿石 pocket，树 `maybe_tree`。

---

## 3. 本会话修复全记录（含根因，按 commit）

| commit | 修了什么 | 根因 |
|--------|---------|------|
| `39b4b5b` | **per-triangle 单一材质**（怪多边形检查点） | 逐顶点材质码 → GPU 跨三角形插值 v_mat 经过不相关材质 → 乱色"怪多边形" |
| `3a66fcc` | **深度混合地形 + winding 纠正剔除** | ① per-triangle 单一码导致硬锯齿 → 改 earthy 用逐顶点**深度**；② MC winding 不一致原先关剔除→两面画→**折页**，用 SDF 梯度法线纠正 winding 后重开背面剔除 |
| `eefc12c` | 去 auto-smooth + carve k 调整 + 法线步长↑ | auto-smooth box 模糊把场抹成平台→扁平片；法线步长 0.5→1.0 减 facet |
| `7b36cd8` | 灰片**上色**（不够，见下） | rock-触-ore 三角形 snap 成 `get_material_color(ROCK)`→无纹理纯色灰片 |
| `faec575` | **灰片几何根因（anti-plateau carve）** | 挖掘 smax 把球**外侧**实心格抬升到接近零，反复挖→接近零平台→MC 网格成**扁平片**。修：球外侧实心格保持不变 |

### 当前材质编码方案（务必理解）
- **earthy（草/土/岩）**：`v_mat = depth = surface_height_f - y`（0~60，连续可插值）。shader 里 `v_mat<100` → 按 depth+slope+世界噪声混 grass→dirt→rock。
- **特殊材质（矿/木/叶/水/沙/雪/砾）**：`v_mat = 200+code`，shader `v_mat>=100` → `get_material_color`。
- **earthy/special 由质心材质决定**（不是"三顶点全 earthy"）——岩石为中心仅一角触矿仍走深度路径出岩石纹理（消灰片）。
- **cube mesher 也用 `200+code`**（否则树叶 code 7 被当 depth 渲成岩石）。

### 其他已修（散落各 commit）
- 草坪 dirt 迷宫脉络：自然顶面用 `surface_block_at`（精确生物群系顶块），像素验证脉络 8354→0。
- dirt.png 原是世界地图（蓝海洋→蓝波纹），已用 `tools/gen_dirt.py` 换成棕土；`chunk_renderer.h` 加载 dirt.png 不是 dirt.jpg。
- 洞壁假反光：specular 只留水面。
- 树木漂浮：树根下沉 2×2 LOG 到 h/h-1（贴浮点 MC 地表）。
- 挖树下地基不倒：`do_dig` 接入 `check_tree_collapse`（扫挖掘足迹±(radius+1)列）。

---

## 4. 当前用户反馈状态

- ✅ 条纹、怪多边形、草坪脉络、假反光、树木漂浮、折页：用户确认改善/解决。
- ✅ 灰片：anti-plateau 之后用户说"最新的感觉对了"，洞不再纯圆、像真石头。
- ✅ 用户问"为什么更真实"→ 已解释（几何恐怖谷 / 打破对称 / 无大平面 / 多尺度粗糙 / 地质分层），写进 `docs/SDF_TERRAIN_REALISM.md`。
- ✅ 用户问"算新发现/有商机吗"→ 诚实结论：**不算基础科学新发现**（全是 30+ 年先验技术的正确组合）；**技术不是商业壁垒**（Valheim/Enshrouded/Vintage Story 等已验证品类，但赢在玩法/美术/打磨，不是地形技术）。当前是"平滑/自然风"非"photorealistic"。

---

## 5. 已知遗留 / 下一步可做

1. **真正的程序化侵蚀**（用户感兴趣但暂没做）：carve 的 sphere 半径叠低频噪声 `sphere += fbm(pos)*amp`，让每次挖的"球"本身坑洼、洞壁不规则；可做大半径粗挖+小半径细节两层。
2. **岩石层理**：按世界 Y 加水平层理色带 + 法线扰动，强化"地层"感。
3. **背面剔除副作用**：若用户反馈深挖处偶有穿洞，是 winding 纠正对某些退化构型失效→检查 `emit_tri` 的 avgN 回退逻辑。
4. **树木美术**：目前是标准体素树（橡/松），用户曾嫌丑；如要更精致需美术活。
5. **真写实（若要冲 AAA 画面）**：PBR 材质 + 法线贴图 + GI + 植被/水体——另一座山，当前远未到。

---

## 6. 关键"别再踩"清单

- ❌ 别把 `emit_tri` 改回**逐顶点材质码** → 乱色怪多边形复发（Desktop/ddddd.txt 记录过当时的教训）。
- ❌ 别去掉 carve 的 **anti-plateau clamp** → 扁平灰片复发。
- ❌ 别加**长宽比 sliver 剔除**（`max_edge²>k·area2`）→ GL_CULL_FACE 关时删大细三角形→真穿洞（注：现在剔除已开，但仍不建议 sliver 剔除）。
- ❌ 别用 box 模糊 `smooth_terrain` 抹挖掘区 → 平台/扁平片。
- ❌ 别在 headless 把相机 pos 设在山体内部测挖洞 → 拍到相机埋土的假象。
- ✅ 视觉 bug 先做**像素采样/材质可视化诊断**，别靠肉眼猜颜色来源（本会话两次猜错才学乖）。

---

## 7. 关键文件
- `src/sdfcraft/mc_mesher.h` — MC 网格化、深度/材质编码、winding 纠正、退化剔除、tree-collapse 句柄
- `src/sdfcraft/world.h` — analytic_field、carve_sphere(anti-plateau)、surface_block_at、surface_height_f、check_tree_collapse、maybe_tree
- `src/sdfcraft/mode.h` — do_dig（挖掘→carveSphere + tree-collapse）
- `src/sdfcraft/mesher.h` — cube mesher（物体方块，200+code 编码）
- `src/sdfcraft/chunk_renderer.h` — 纹理加载/绑定、不透明背面剔除/透明双面
- `shaders/sdfcraft_chunk.frag` — earthy 深度混合 + 特殊材质 + 光照
- `assets/textures/blocks/` — grass/dirt/rock/rock2/mossy_rock/sand/snow/gravel/wood/bark；`dirt_earthmap_backup.png` 是原错图
- `tools/gen_dirt.py` — 生成棕土纹理
- `docs/SDF_TERRAIN_REALISM.md` — 理论 + 新颖性/商业评估
- `CLAUDE.md` — 项目规则（用 codebase-memory 图谱、源码在 src、产物 build 不改）
