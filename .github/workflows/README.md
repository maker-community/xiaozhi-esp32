# GitHub Actions Workflows 说明

本项目包含两个独立的 CI/CD 工作流：

## 📄 build.yml（主构建流程）

**用途**：官方主分支构建

**触发条件**：
- 推送到 `main` 分支
- 推送到 `ci/*` 分支
- 针对 `main` 分支的 Pull Request

**输出产物**：
```
xiaozhi_<board-name>_<commit-sha>.bin
releases/v2.2.2_<board-name>.zip
```

**版本号**：`2.2.2`（无后缀）

**维护**：官方维护，请勿修改

---

## 📄 build-verdure.yml（Verdure 定制构建）

**用途**：verdure 分支定制构建

**触发条件**：
- 推送到 `verdure` 分支
- 针对 `verdure` 分支的 Pull Request

**输出产物**：
```
xiaozhi_<board-name>-verdure_<commit-sha>.bin
releases/v2.2.2-verdure_<board-name>.zip
```

**版本号**：`2.2.2-verdure`（带后缀）

**维护**：verdure 分支维护者

**特点**：
- ✅ 完全独立运行
- ✅ 不影响主构建流程
- ✅ 使用独立的构建脚本 `release-verdure.py`
- ✅ 产物自动添加 verdure 标识

---

## 🔄 工作流对比

| 特性 | build.yml | build-verdure.yml |
|------|-----------|-------------------|
| 触发分支 | main, ci/* | verdure |
| 构建脚本 | release.py | release-verdure.py |
| 版本号 | 2.2.2 | 2.2.2-verdure |
| 产物名称 | xiaozhi_xxx.bin | xiaozhi_xxx-verdure.bin |
| 用途 | 官方发布 | 定制版本 |
| 维护者 | 官方团队 | verdure 分支维护者 |

---

## 📝 使用建议

### 对于主分支开发者
- 只关注 `build.yml`
- 推送到 main 分支触发官方构建
- 不需要了解 verdure 相关内容

### 对于 verdure 分支开发者
- 推送到 verdure 分支触发定制构建
- 可以自由修改 `build-verdure.yml`
- 定期同步主分支更新

### 对于贡献者
- PR 到 main 分支使用 `build.yml`
- PR 到 verdure 分支使用 `build-verdure.yml`
- 两个流程完全独立，互不干扰

---

## 🛠️ 本地测试

### 测试主构建脚本
```bash
python scripts/release.py --list-boards
python scripts/release.py <board> --name <variant>
```

### 测试 verdure 构建脚本
```bash
python scripts/release-verdure.py --list-boards
python scripts/release-verdure.py <board> --name <variant>
```

---

## 📚 更多信息

- Verdure 构建文档：[../docs/verdure-build.md](../docs/verdure-build.md)
- Verdure 快速开始：[../docs/verdure-quickstart.md](../docs/verdure-quickstart.md)
- 文件清单：[../docs/verdure-build-files.md](../docs/verdure-build-files.md)

---

**注意**：这两个 workflow 设计为完全独立运行，互不影响，可以在同一仓库和平共存。
