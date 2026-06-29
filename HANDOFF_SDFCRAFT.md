# SDFCraft 交接文档（给新会话）

> 项目根目录：`G:\CMakePJ\MassRTS`，源码在 `src/sdfcraft/`。
> 这是一个仿 Minecraft 的体素游戏。本文档记录当前进度和下一步计划。

---

## ✅ 最新完成：背包和合成系统重做（2026-06-29）

用户的核心要求是 **背包和合成系统完全照搬 Minecraft**。上一会话做歪了，本次已修正。

### 已完成的修正
1. ✅ **删除** `craft_screen.h`（R 键配方列表，错误方式）
2. ✅ **恢复** `inventory_screen.h` 里的 **2×2 合成格**（左侧，盔甲槽下方）
3. ✅ 盔甲槽从横排改为**竖排**（左上角，头/胸/腿/脚，MC 标准）
4. ✅ 合成格支持 MC 手动拖放交互（点击格子放材料，自动匹配配方显示结果，点结果拿走成品）
5. ✅ 移除 R 键绑定（`main_sdfcraft.cpp` + `mode.h`），V 键起飞保留
6. ✅ 移除 `mode.h` 中所有 `craft_screen` / `craft_toggle` 相关代码
7. ✅ 编译通过，截图验证布局正确

### 正确目标（已实现）
**背包界面（按 E）= MC 经典布局：**
- 左上：**4 个盔甲槽竖排**（头/胸/腿/脚）
- 左下：**2×2 合成格** + 箭头 + 结果格（随身合成）
- 右侧：3×9 主背包 + 9 格 hotbar

**交互方式（已实现）：**
- 鼠标点起/放下材料到 2×2 格子
- 靠**配方形状匹配**实时显示结果（`RecipeBook::match`）
- 点结果格拿走成品（消耗每格一个材料）
- **不是**点列表自动合成

### 测试验证
```bash
cd G:\CMakePJ\MassRTS\build\Release
.\SDFCraft.exe --openinv --shot inv_2x2_craft.png --frames 5
```
截图显示：盔甲槽竖排、2×2 合成格、箭头、结果槽、主背包、hotbar 布局全部正确 ✅

---

## 🔜 下一步：工作台 3×3 界面

用户要求右键工作台方块（`BLOCK_CRAFTING_TABLE`）打开 3×3 合成界面。

### 待实现
1. 新建工作台界面类（可在 `inventory_screen.h` 里加个 `bench_mode_` 模式，或单独文件）
   - 顶部：**3×3 合成格** + 箭头 + 结果格
   - 下面：3×9 背包 + hotbar
2. `mode.h` 里加方块右键交互逻辑：检测 `last_hit_.block == BLOCK_CRAFTING_TABLE` 时打开 3×3
3. 与背包的 2×2 类似交互：手动拖放、配方匹配、点结果消耗材料

---

## ✅ 已完成并验证（这些是对的，保留）

### 1. 物品数量显示
- hotbar + 背包 + 鼠标拖拽的物品都显示 MC 风格白字带阴影数量
- 工具（max_stack=1）正确不显示数字
- 实现：`hud_renderer.h` 和 `inventory_screen.h` 各有 `draw_count()`，用 `assets/textures/gui/default.png`（128×128 ASCII 字体）
- **已截图验证**

### 2. 3D 等距方块图标（后台 agent 完成）
- 方块物品在 hotbar/背包/合成结果里渲染成立体方块（不是平面贴图），工具保持平面
- 接口在 `item_icons.h`：`block_face(b, face)`、`struct IsoTri`、`emit_iso_cube(...)`
- `hud_renderer.h` 已接入
- **已截图验证**

### 3. 盔甲数据层（全部正确，保留）
- `items.h`：12 件盔甲物品（皮革/铁/钻石 × 头胸腿靴），`enum class ArmorSlot`，ItemDef 加了 `armor`(防御点) + `slot`(ArmorSlot) 字段，helper：`item_armor(id)`、`item_armor_slot(id)`、`total_armor(inv)`
- `inventory.h`：加了 `std::array<ItemStack,4> armor{}`、`count(id)`、`remove(id,n)`
- `server_sim.h`：`ServerPlayer.armor_points` 字段；`damage_nearest_player()` 里按 4%/点、上限 80% 减伤
- `mode.h`：每 tick 把本地玩家 `total_armor(inv)` 同步到 `ServerPlayer.armor_points`
- `crafting.h`：`add_armor_set()` + 皮革/铁/钻石三套配方（MC 形状）
- **测试验证**：`selftest.cpp` 里 ARMOR 测试通过——铁套 11 点防御 → 44% 减伤，配方能合成
- 盔甲槽点击穿戴逻辑（`move_armor` + `armor_slot` 匹配）在 `inventory_screen.h`，但布局要改竖排

---

## 关键文件清单

| 文件 | 状态 |
|---|---|
| `items.h` | ✅ 保留（盔甲物品/字段/helper） |
| `inventory.h` | ✅ 保留（armor 数组、count、remove） |
| `server_sim.h` | ✅ 保留（armor_points、减伤） |
| `crafting.h` | ✅ 保留（盔甲配方、match 支持 shaped/shapeless 2×2 和 3×3）|
| `item_icons.h` | ✅ 保留（盔甲图标 + iso cube） |
| `hud_renderer.h` | ✅ 保留（数量显示 + iso 图标） |
| `inventory_screen.h` | ✅ **已重做**：2×2 合成格 + 竖排盔甲槽，完全 MC 标准 |
| `craft_screen.h` | ✅ **已删除**（R 列表方式错误） |
| `mode.h` | ✅ **已清理**：移除 craft-screen 所有引用；保留 armor 同步 |
| `main_sdfcraft.cpp` | ✅ **已清理**：移除 R→craft 绑定；V=起飞 |

---

## 当前键位（main_sdfcraft.cpp）

- `E` = 背包（含 2×2 合成格 + 盔甲槽）
- `V` = 起飞（飞行模式）
- `C` = 降落
- `F` = 切换飞行
- `G` = 星球视图
- `Q` = 吃东西
- `1-9` = hotbar 选择，滚轮 = hotbar 切换
- `R` = **已移除**（将来用于工作台右键交互）

---

## 构建 & 测试 & QA 截图

- 构建：用 `cmake --build build --config Release --target SDFCraft`（exe 输出 `build/Release/SDFCraft.exe`）
- 测试：`test_sdfcraft` target → `build/Release/test_sdfcraft.exe`，逻辑在 `src/sdfcraft/selftest.cpp`
- QA 无头截图（验证 UI）：
  - `--openinv` 开背包截图
  - `--opencraft` 上一会话加的（列表界面，重做后改掉）
  - 截图写到 `build/Release/*.png`
- 放大截图看细节：用 Python PIL crop，注意路径是 `build/Release/`（不是当前目录）
- **教训**：裁切宽度不够会切掉边缘内容，导致误判（上次误以为盔甲槽只有 3 个，其实是 4 个被切了）

---

## 整体路线图（用户已确认顺序）

1. **UI 收尾**（当前）：背包 2×2 + 工作台 3×3 完全照搬 MC + 盔甲槽竖排 ← **重做中**
2. **多重噪声真实山体**：domain warp + ridged noise + 按高度/坡度分层。改 `world` 的 `surface_height_f`/`sdf_at`。便宜、立竿见影、不动架构
3. **Voxy 式远视距 LOD + 异步 worker 线程 + 实体双缓冲**（打包一起做）：
   - 已有地基：`planet_mesh.h` 的 cube-sphere 四叉树 + 距离 LOD refine；sim 已对 `chunks_` 只读
   - 还需：给 `mc_mesher.h` 的 `build()` 加 `step`（粗体素）参数做 LOD；chunk 按距离选 LOD；远 LOD 后台线程异步生成
   - **关键前提**：网格化目前还会往 `chunks_` 插 chunk（`get_chunk(true)`），开 worker 线程前必须先让网格化只读，否则 unordered_map 数据竞争崩溃
   - worker 线程现在没收益（sim 0.4ms、网格化 <1ms，没重活可藏），所以推迟到这阶段——那时它有真任务（异步生成远 LOD）

---

## 给新会话的建议开场

1. 先 `git status` 看上一会话的改动，`git log --oneline -20` 看提交历史
2. 读本文档 + `git log -p src/sdfcraft/inventory_screen.h` 找回被删的 2×2 合成格代码
3. 从"重做步骤清单"第 1 步开始：删 craft_screen.h、恢复 2×2、盔甲竖排
4. 用户偏好：**一口气做完一块再截图验证**（不用每步问），但**做之前要确认方向对**（上次就是没确认方向做歪了）
5. 沟通用中文
