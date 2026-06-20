# MassRTS — CloseCrab 项目规则

## 知识图谱工作流（codebase-memory MCP）

本项目已建立代码知识图谱。开发时**必须**用图谱辅助，不要盲目大量读文件。

### 动手前：先查图谱

收到任务后，先用这些工具搞清楚结构，再动代码：

- `mcp__codebase-memory__search_graph` — 按名字/关键词找函数、类、定义（替代 grep 找定义）
- `mcp__codebase-memory__trace_path` — 查某个函数被谁调用、它又调用谁（调用链）
- `mcp__codebase-memory__get_architecture` — 看项目整体结构（语言、模块、热点）
- `mcp__codebase-memory__get_code_snippet` — 按限定名直接取某函数源码
- `mcp__codebase-memory__search_code` — 图谱内的全文搜索

**禁止**：一上来就大量 Read 文件堆上下文。先查图谱定位，只读真正相关的少数文件。

### 改完后：更新图谱

- **小改动**（改了几个函数）：用 `mcp__codebase-memory__detect_changes` 看影响范围，暂不全量重索引。
- **完成一个完整功能 / 改动较多**：调一次 `mcp__codebase-memory__index_repository`，参数 `{"repo_path": "G:/CMakePJ/MassRTS"}`，让图谱跟上改动。

不要每改一行就全量重索引（扫全项目约 10 秒，太频繁会拖慢节奏）。

### 固定流程

```
查图谱（search_graph / trace_path / get_architecture）
  → 改代码
  → 更新图谱（detect_changes 增量；完整功能后 index_repository 全量）
```

## 项目信息

- 路径：`G:\CMakePJ\MassRTS`
- 构建产物在 `build/`（已被图谱索引自动排除，不要在这里改源码）
- 源码在 `src/`
