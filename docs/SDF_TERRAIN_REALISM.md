# SDF 体素地形的"自然真实感"——sdfCraft 技术总结与理论文档

> 项目：MassRTS / sdfCraft（G:\CMakePJ\MassRTS）
> 主题：基于 Signed Distance Field + Marching Cubes 的可破坏体素地形，如何从"假"做到"像真石头"
> 性质：工程总结 + 视觉感知理论梳理 + 诚实的新颖性/商业评估

---

## 0. 一句话结论

**视觉真实感 ≈ 打破对称 + 消除大平面 + 多尺度粗糙 + 合理地质分层。**
本项目的"变真实"不是加了侵蚀噪声，而是**修掉了违反前两条的人造瑕疵**（纯球、抬升平台/扁平片），让 SDF 多球并集本身的有机形状显现出来。

---

## 1. 系统架构

### 1.1 地形表示
- **连续 SDF 场**：`analytic_field(x,y,z) = (y - surface_height_f(x,z)) * inv`，其中 `inv = 1/sqrt(1+gx²+gz²)` 是用地表梯度归一化的因子，保证斜坡上 Marching Cubes 放点干净。负=固体，正=空气。
- **地表高度**：5-octave fBm + 山脉项（`mountain²` 放大），sea_level 基准。
- **carve 层**：每 chunk 一个 float SDF 覆盖层，挖掘/填充写入；未触动的体素回退到解析场（不向方块占用钳制——那是阶梯状与 raycast 不一致的根源）。

### 1.2 网格化（Marching Cubes）
- 标准 Lorensen–Cline / Paul Bourke 256 行表。
- 局部 dense field buffer（chunk + 1 体素梯度环）一次采样，march + 着色全从内存读（O(1)，内循环零噪声）→ 单 chunk 构建 <1ms。
- 顶点法线 = SDF 梯度中心差分（步长 1.0，宽邻域平均 → 平滑法线，隐藏 1 体素 facet）。
- 顶点格式：10 floats `(pos3, normal3, color3, mat1)`。

### 1.3 双网格器
- **MCMesher**：自然地形（土/石/草/沙/雪/矿/砾）的平滑等值面。
- **Cube mesher**：物体方块（原木/树叶/水/玻璃/放置块）保持立方，发不透明+透明两段。

### 1.4 着色（PBR-like）
- Triplanar 三向投影纹理（消 UV 拉伸），双尺度细节混合。
- Hemisphere 环境光 + Lambert + 仅水面 Blinn-Phong 高光。
- 大气雾（二次衰减）+ Reinhard tonemap。

---

## 2. 核心工程模式：材质如何不"乱色"又"平滑分层"

### 2.1 问题
每个顶点独立查最近方块的材质码（grass=1/dirt=2/rock=3/ore=8…），GPU 跨三角形**插值材质码**会经过所有中间值 → 在不相关材质调色板间扫过 → **刺眼的多色"怪多边形"**（挖掘墙面最明显）。

### 2.2 失败的两个极端
- **逐顶点材质码**：边界平滑，但跨码插值 → 乱色。
- **整三角形单一材质码**：无乱色，但材质边界变**硬锯齿**，且 rock-触-ore 的三角形会被 snap 成 `get_material_color(ROCK)` → 无纹理纯色"灰片"。

### 2.3 解决方案：深度编码 + 质心决策
- **earthy 地形（草/土/岩）改用"深度" `depth = surface_height_f - y` 编码进 v_mat**。深度是**连续几何量**，插值安全 → grass→dirt→rock 平滑过渡、零乱色。Shader 按 depth + slope + 世界噪声抖动混合三种纹理。
- **特殊材质（矿/木/叶/水/沙/雪/砾）** snap 成 `v_mat = 200 + code`，shader 走离散材质分支。
- **earthy/special 用质心材质决策**，不是"三顶点全 earthy"——岩石为中心、仅一角触矿的三角形仍走深度路径出岩石纹理（消灰片）。
- cube mesher 也用 `200+code` 编码，避免树叶码被当深度渲成岩石。

> **要点**：把"可安全插值的连续量(深度)"和"不可插值的离散类型(材质码)"分开编码，是这套地形着色的关键工程模式。

---

## 3. 几何真实感：消除人造瑕疵

### 3.1 扁平"灰片/板"——anti-plateau carve（最关键的真实感来源）
- **机理**：挖掘 `smax(cur, -sphere, k)` 会把**球体外侧的实心格也抬升**到接近零（深 -30 的格被抬到 -1.9）。反复/重叠挖 → 整个挖掘区 SDF 被逐步抬到接近零 → **大片接近零平台 → MC 网格成悬空扁平片**。
- **修复**：挖掘时**球外侧实心格保持不变**（`op<0 && sphere>0 && cur<0 → 保持 cur`）。洞 = 干净的"球∩地形"，无抬升壳、无平台、无扁平片。

### 3.2 折页/双面——winding 纠正 + 背面剔除
- MC 表对复杂构型 winding 不一致，原先只能关 `GL_CULL_FACE` → 两面都画 → 薄壁像折叠纸。
- 用 SDF 梯度顶点法线**纠正每个三角形 winding**（`dot(faceN, avgVertN)<0 就 swap`）→ 全部朝外 → 重开背面剔除 → 折页消失，不带回穿洞。

### 3.3 退化三角形 / 折角
- `area2 < 1e-4` 剔除近退化三角形（叉积/微面积→垃圾法线→乱色）。
- 法线中心差分步长 0.5→1.0，宽邻域平均 → 1 体素 facet 着色成圆面。

### 3.4 草坪脉络 / 树木悬浮（顺带）
- 自然顶面用生成器的 `surface_block_at`（精确生物群系顶块）→ 消草坪 dirt 迷宫脉络。
- 树根下沉 2×2 LOG 到 h/h-1（贴浮点 MC 地表）；地形挖掘接入 `check_tree_collapse`（挖树下地基则坍塌掉落）。

---

## 4. 视觉真实感理论（为什么纯圆假、现在像石头）

### 4.1 "几何恐怖谷"
人眼对**完美几何图元**（正圆/正球/大平面/直线）极敏感，因为自然界几乎不存在它们。一出现完美对称 → 大脑判定人造。原洞 = 单球 + 抬升平台，全是规则图元 → 一眼假。

### 4.2 自然 = 四要素
1. **打破对称**：多个挖掘球并集 ∩ 起伏地形 → 不重复、不规则。对应真实岩石"无数次断裂/侵蚀事件叠加"。
2. **无大平面**：真石头除非沿层理，否则无大平整面。消扁平片 = 去掉最强的"假"信号。
3. **多尺度粗糙（近分形）**：SDF+MC 的连续曲面法线在各尺度变化；自然表面的核心是粗糙度**多尺度自相似**。
4. **合理分层**：深度驱动 grass→dirt→rock = 真实地层序，大脑识别"土覆岩"→判定真地面。

### 4.3 为什么 SDF 球并集天生像有机物
SDF + smin/smax 球并集是 CG 建模有机体/石头/熔岩的经典手法（metaball/blobby surfaces）。smooth-min 在融合处产生的圆滑过渡曲率，与水滴、岩石、生物连接处同源——**数学上模拟了材料堆积/侵蚀的方式**。只要别让规则图元（纯球、平台）露出来，它天生"长得像自然物"。

---

## 5. 诚实的新颖性评估：这算新发现吗？

**坦率说：不算基础科学新发现。** 这是把成熟技术做对的扎实工程，外加一组少见但有用的实战修复经验。

### 5.1 全是已知的先验技术（部分 30+ 年）
- Marching Cubes：Lorensen & Cline, 1987
- Perlin / fBm 噪声：Perlin, 1985；Musgrave 分形地形, 1990s
- Metaball / blobby / smooth-min CSG：Blinn, 1982；Inigo Quilez 把 SDF + smin 写成现代范式
- Triplanar 映射、距离场渲染：业界标准
- "真实 = 打破对称 + 多尺度分形粗糙 + 分层"：见 *Texturing & Modeling: A Procedural Approach*（Ebert/Musgrave/Perlin 等），早成经典

### 5.2 真正有价值的部分（是工程 know-how，不是论文）
把"SDF + MC 体素**可破坏**地形在 1 体素分辨率下"的一组失败模式与修复，整理成连贯一套，**业界没有公开的系统化文档**：
- anti-plateau carve clamp（反复挖出现平台/扁平片的根因与修复）
- 深度编码 vs 材质码编码分离（消乱色又保平滑）
- 质心材质决策（消 rock-触-ore 灰片）
- winding 纠正以重开背面剔除（消折页）

> 这些适合做**技术博客 / GDC 风格分享 / 开源范例**，而不是专利或学术论文——因为底层都是先验技术的组合应用。

---

## 6. 诚实的商业评估：有商机吗？

### 6.1 这条赛道已被验证、也很拥挤
可破坏/可形变体素-SDF 地形的已上市作品：
- **Astroneer**（可形变体素地形）、**No Man's Sky**（SDF 风格星球）
- **Teardown**（体素破坏物理）、**Deep Rock Galactic**（可挖掘）
- **Vintage Story / 7 Days to Die / Space Engineers / Empyrion / Enshrouded**

技术本身**不是壁垒**——这些都做到了类似甚至更强。

### 6.2 机会在哪
- **不在"技术新"**，在**产品**：独特玩法 + 美术方向 + 打磨度。
- 单人做"SDF 版 Minecraft"硬刚 Minecraft/体素大盘，商业上**非常难**。
- 更现实的切入：
  1. **可破坏地形作为差异化玩法核心**的小体量独立游戏（挖掘/塑形/地质本身是乐趣点）。
  2. **Unity/Unreal 资产或插件**：把这套 SDF 可破坏地形 + 真实感修复封装成中间件卖给开发者（市场存在，但已有 Voxel Plugin、Cubiquity 等竞品）。
  3. **技术内容创作**：博客/课程/开源，建立技术影响力再变现（更稳）。

### 6.3 一句话
**技术不构成商机壁垒；可成为商机的是"用这套技术做出的好游戏或好工具"，外加把这些少见的实战修复经验做成内容资产。**

---

## 7. 后续可加强（若要更强侵蚀感）
- carve 的 sphere 半径叠低频噪声：`sphere += fbm(pos)*amp` → 每次挖的"球"本身坑洼，洞壁不规则（真正的程序化侵蚀）。
- 多尺度：大半径粗挖 + 小半径细节噪声两层。
- 岩石走向/层理：按世界 Y 加水平层理色带 + 法线扰动，强化"地层"感。

---

## 附：关键文件与提交
- `src/sdfcraft/mc_mesher.h`：MC 网格化、深度/材质编码、winding 纠正、退化剔除
- `src/sdfcraft/world.h`：analytic_field、carve_sphere（anti-plateau clamp）、surface_block_at、tree
- `src/sdfcraft/mode.h`：do_dig（挖掘接入 tree-collapse）
- `src/sdfcraft/chunk_renderer.h`：纹理绑定、背面剔除
- `shaders/sdfcraft_chunk.frag`：earthy 深度混合 + 特殊材质 + 光照
- 相关 commit：`39b4b5b`(per-triangle 材质) → `3a66fcc`(深度混合+winding) → `eefc12c`(去 auto-smooth) → `7b36cd8`(灰片上色) → `faec575`(anti-plateau 几何根因)
