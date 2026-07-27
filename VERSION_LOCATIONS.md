# 版本号位置列表

当前项目版本：`0.6.8`

## 主要位置

| 文件 | 内容 | 说明 |
|------|------|------|
| `VERSION` | `0.6.8` | 统一版本文件 |
| `Cargo.toml` | `version = "0.6.8"` | Rust 包版本 |
| `src/repl/mod.rs` | `AI Digital v0.6.8` | CLI / 日志 / 报告文本 |
| `src/gui/mod.rs` | `AI Digital v0.6.8` | GUI 启动横幅 |
| `src/gui_exchange.rs` | `0.6.8` | GUI 交换状态版本 |
| `gui.tcl` | `0.6.8` | GUI 脚本版本 |
| `engine/src/rtl_engine.cpp` | `rtl-engine 0.6.8` | C++ 引擎版本 |
| `engine/src/engine_full.cpp` | `0.6.8` | 引擎总版本 |
| `install.sh` | `AI Digital v0.6.8 - Installation` | 安装脚本标题 |

## 升级步骤

1. 更新 `VERSION`
2. 更新 `Cargo.toml`
3. 更新 CLI / GUI / engine / 安装脚本中的版本字符串
4. 重新构建并检查 `report.json`、CLI 横幅和 GUI 标题
