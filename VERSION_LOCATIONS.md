# 版本号位置列表

## 统一版本号文件

版本号统一定义在 `VERSION` 文件中，内容为：`0.6.7`

## 所有版本号位置

| 文件 | 行号 | 内容 | 说明 |
|------|------|------|------|
| `VERSION` | 1 | `0.6.7` | **统一版本号文件** |
| `Cargo.toml` | 3 | `version = "0.6.7"` | Rust包版本 |
| `README.md` | 1 | `# AI Digital v0.6.7` | 标题 |
| `README.md` | 24 | `Cargo.toml # Rust project config (ai_digital v0.6.7)` | 注释 |
| `README.md` | 319 | `║       AI Digital v0.6.7                 ║` | 示例输出 |
| `README.md` | 323 | `● Engine: rtl-engine 0.6.7` | 示例输出 |
| `engine/src/rtl_engine.cpp` | 158 | `return "rtl-engine 0.6.7 (industrial RTLIL)";` | 引擎版本 |
| `engine/src/rtl_engine.cpp` | 159 | `"version":"0.6.7"` | 引擎信息JSON |
| `engine/src/engine_full.cpp` | 457 | `return "0.6.7";` | 引擎版本 |
| `src/repl/mod.rs` | 1398 | `║        AI Digital v0.6.7                 ║` | CLI横幅 |

## 升级版本号步骤

1. 修改 `VERSION` 文件内容
2. 更新 `Cargo.toml` 中的 `version`
3. 更新 `README.md` 中的所有版本号引用
4. 更新 `engine/src/rtl_engine.cpp` 中的版本号
5. 更新 `engine/src/engine_full.cpp` 中的版本号
6. 更新 `src/repl/mod.rs` 中的CLI横幅

## 注意事项

- 依赖库的版本号（如rustyline、serde等）不需要更新
- 只更新项目本身的版本号
