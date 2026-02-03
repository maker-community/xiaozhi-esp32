# Verdure 快速开始指南

## 🚀 5分钟快速开始

### 步骤 1：创建 verdure 分支

```bash
# 确保在项目根目录
cd C:\github\xiaozhi-esp32

# 从 main 分支创建 verdure 分支
git checkout main
git pull origin main
git checkout -b verdure

# 推送到远程
git push -u origin verdure
```

### 步骤 2：验证文件

确认以下新文件存在：

```
✅ .github/workflows/build-verdure.yml
✅ scripts/release-verdure.py
✅ docs/verdure-build.md
```

### 步骤 3：测试本地构建（可选）

```powershell
# Windows PowerShell
.\venv\Scripts\Activate.ps1

# 列出所有支持的板型
python scripts/release-verdure.py --list-boards

# 测试构建（选择一个你的板型）
python scripts/release-verdure.py bread-compact-ml307 --name bread-compact-ml307
```

### 步骤 4：触发 GitHub Actions 构建

```bash
# 提交并推送任何更改
git add .
git commit -m "verdure: 添加定制构建系统"
git push origin verdure
```

### 步骤 5：查看构建结果

1. 访问 GitHub 仓库
2. 点击 "Actions" 标签
3. 查看 "Build Verdure Boards" 工作流
4. 下载构建产物

## 📦 预期输出

### 文件名格式

```
GitHub Actions 产物：
├── xiaozhi_bread-compact-ml307-verdure_<commit-sha>.bin
├── xiaozhi_esp-box-verdure_<commit-sha>.bin
└── ...

本地构建产物 (releases/ 目录):
├── v2.2.2-verdure_bread-compact-ml307.zip
├── v2.2.2-verdure_esp-box.zip
└── ...
```

### 版本号显示

固件启动时会显示：
```
[System] bread-compact-ml307/2.2.2-verdure
```

## 🔄 日常使用

### 添加自定义修改

```bash
# 在 verdure 分支进行修改
git checkout verdure

# 修改代码...
vim main/application.cc

# 提交并推送（自动触发构建）
git add .
git commit -m "verdure: 添加自定义功能"
git push origin verdure
```

### 同步主分支更新

```bash
# 定期同步官方更新
git checkout verdure
git merge origin/main
git push origin verdure
```

## ✅ 验证清单

构建完成后，验证：

- [ ] 固件文件名包含 `-verdure`
- [ ] 启动日志显示 `2.2.2-verdure`
- [ ] HTTP User-Agent 包含 verdure
- [ ] CMakeLists.txt 没有被修改
- [ ] 没有 `.verdure.bak` 文件残留

## 🆘 常见问题

### Q: 构建失败怎么办？

A: 检查以下几点：
1. 确保在 verdure 分支
2. 查看 GitHub Actions 日志
3. 本地测试 `python scripts/release-verdure.py --list-boards`

### Q: CMakeLists.txt 被修改了？

A: 这是正常的临时行为：
- 构建过程中会临时修改
- 构建完成后自动恢复
- 检查是否有 `.verdure.bak` 残留

### Q: 如何更改版本后缀？

A: 编辑 `scripts/release-verdure.py` 第 18 行：
```python
VERDURE_SUFFIX = "-verdure"  # 改为你想要的后缀
```

### Q: 能否在 main 分支使用？

A: 不建议！verdure 系统专为独立分支设计：
- 保持 main 分支纯净
- 避免意外修改主分支
- 方便合并官方更新

## 📚 更多信息

- 详细文档：[docs/verdure-build.md](verdure-build.md)
- 文件清单：[docs/verdure-build-files.md](verdure-build-files.md)
- 主项目文档：[README.md](../README.md)

## 🎉 完成！

现在你有了一个完全独立的定制构建系统，可以：
- ✅ 自由定制固件
- ✅ 自动添加版本标识
- ✅ 无冲突合并官方更新
- ✅ CI/CD 自动构建

享受你的 verdure 定制固件吧！🚀
