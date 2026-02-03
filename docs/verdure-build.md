# Verdure 定制构建系统

这是一套完全独立的构建系统，专门用于生成带有 `-verdure` 标识的定制固件，**不会修改任何主分支文件**，避免合并冲突。

## 📁 新增文件

- `.github/workflows/build-verdure.yml` - verdure 专用 CI/CD 工作流
- `scripts/release-verdure.py` - verdure 专用构建脚本
- `docs/verdure-build.md` - 本说明文档

## ✨ 特性

### 1. **零冲突设计**
- ✅ 不修改 `CMakeLists.txt`
- ✅ 不修改 `build.yml`
- ✅ 不修改 `release.py`
- ✅ 所有修改都在新增的独立文件中

### 2. **自动版本标识**
- 版本号自动添加 `-verdure` 后缀
- 固件文件名包含 verdure 标识
- 启动时显示带后缀的版本号

### 3. **完整功能支持**
- 支持所有板型和变体
- 支持 GitHub Actions 自动构建
- 支持本地手动构建

## 🚀 使用方法

### 方式一：GitHub Actions 自动构建（推荐）

1. **创建并推送 verdure 分支**
   ```bash
   git checkout -b verdure
   git push origin verdure
   ```

2. **自动触发构建**
   - 推送代码到 `verdure` 分支会自动触发构建
   - 构建产物会自动上传到 GitHub Actions

3. **下载构建产物**
   - 在 GitHub Actions 页面下载
   - 文件名格式：`xiaozhi_<board-name>-verdure_<commit-sha>.bin`

### 方式二：本地构建

```bash
# 激活虚拟环境（如果有）
source venv/bin/activate  # Linux/Mac
# 或
.\venv\Scripts\Activate.ps1  # Windows

# 构建特定板型和变体
python scripts/release-verdure.py <board-type> --name <variant-name>

# 示例：构建 bread-compact-ml307
python scripts/release-verdure.py bread-compact-ml307 --name bread-compact-ml307

# 构建所有变体
python scripts/release-verdure.py all
```

## 📦 输出文件

### 构建产物位置
```
releases/
  └── v2.2.2-verdure_bread-compact-ml307.zip
```

### GitHub Actions 产物
```
xiaozhi_bread-compact-ml307-verdure_abc123def.bin
```

## 🔧 工作原理

### release-verdure.py 工作流程

1. **读取原始版本号** - 从 `CMakeLists.txt` 读取 `PROJECT_VER`
2. **临时修改文件** - 创建 `CMakeLists.txt.verdure.bak` 备份，修改版本号
3. **执行构建** - 调用 `idf.py build` 编译固件
4. **自动恢复** - 构建完成后自动恢复 `CMakeLists.txt` 到原始状态
5. **打包固件** - 生成带 verdure 标识的 zip 包

### 版本号处理

```python
# 原始版本
PROJECT_VER = "2.2.2"

# verdure 脚本临时修改为
PROJECT_VER = "2.2.2-verdure"

# 构建完成后自动恢复
PROJECT_VER = "2.2.2"
```

## 🌳 分支管理

### 推荐工作流

```bash
# 1. 从主分支更新
git checkout main
git pull origin main

# 2. 切换到 verdure 分支
git checkout verdure

# 3. 合并主分支更新（无冲突！）
git merge main

# 4. 推送触发自动构建
git push origin verdure
```

### 定期同步

```bash
# 定期将主分支的更新合并到 verdure 分支
git checkout verdure
git merge origin/main
git push origin verdure
```

## 📊 版本显示位置

verdure 版本标识会显示在：

1. **启动日志**
   ```
   [System] bread-compact-ml307/2.2.2-verdure
   ```

2. **HTTP User-Agent**
   ```
   User-Agent: bread-compact-ml307/2.2.2-verdure
   ```

3. **OTA 版本检查**
   ```
   Current version: 2.2.2-verdure
   ```

4. **MCP Server Info**
   ```json
   {
     "serverInfo": {
       "name": "bread-compact-ml307",
       "version": "2.2.2-verdure"
     }
   }
   ```

## 🛠️ 本地测试

### 测试构建脚本

```bash
# 列出所有支持的板型
python scripts/release-verdure.py --list-boards

# 列出所有变体（JSON 格式）
python scripts/release-verdure.py --list-boards --json

# 测试单个板型构建
python scripts/release-verdure.py bread-compact-ml307 --name bread-compact-ml307
```

### 验证版本号

构建完成后，检查：
```bash
# 查看生成的文件
ls releases/

# 应该看到
v2.2.2-verdure_bread-compact-ml307.zip
```

## ⚙️ 自定义配置

### 修改版本后缀

编辑 `scripts/release-verdure.py`：

```python
# 第 18 行
VERDURE_SUFFIX = "-verdure"  # 改为你想要的后缀
```

### 添加自定义构建选项

可以在 verdure 分支中：
- 修改 `main/boards/*/config.json` 添加板型配置
- 修改 `sdkconfig.defaults.*` 调整默认配置
- 这些修改不会影响主分支

## 🐛 故障排除

### 构建失败后 CMakeLists.txt 未恢复

如果构建过程中断，手动恢复：

```bash
# 检查是否有备份文件
ls CMakeLists.txt.verdure.bak

# 手动恢复
cp CMakeLists.txt.verdure.bak CMakeLists.txt
rm CMakeLists.txt.verdure.bak
```

### GitHub Actions 失败

检查以下几点：
1. 确保 `scripts/release-verdure.py` 有执行权限
2. 确保 verdure 分支已推送到远程
3. 查看 Actions 日志获取详细错误信息

## 📝 维护说明

### 同步主分支更新

当主分支的 `release.py` 有重要更新时：

1. 将主分支的 `release.py` 的更新手动同步到 `release-verdure.py`
2. 保持两个脚本的核心逻辑一致
3. verdure 脚本只需额外处理版本号修改逻辑

### 更新工作流

如果主分支的 `build.yml` 有更新：
- 评估是否需要同步到 `build-verdure.yml`
- 保持两个 workflow 的结构相似

## 📄 许可证

遵循项目主许可证。

## 🙋 支持

如有问题，请查看：
- 主项目文档
- GitHub Issues
- 构建日志

---

**提示**：这个系统设计为完全独立运行，不会影响主分支的任何文件，确保你可以随时合并官方更新而不产生冲突！
