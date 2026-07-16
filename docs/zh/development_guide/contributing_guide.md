# 贡献指南

感谢您考虑为 msMemScope 做贡献！

## 提交错误报告

如果您在msMemScope中发现了一个不存在安全问题的漏洞，请在msMemScope仓库中的Issues中搜索，以防该漏洞被重复提交，如果找不到漏洞可以创建一个新的Issues。如果发现了一个安全问题请不要将其公开，请参阅安全问题处理方式。提交错误报告时应该包含完整信息。

## 安全问题处理

本项目中对安全问题处理的方式，请通过邮箱通知项目核心人员确认并编辑。

## 解决现有问题

通过查看仓库的Issues列表可以发现需要处理的问题信息，可以尝试解决其中的某个问题。

## 如何提出新功能

请使用Issues的Feature标签进行标记，我们会定期处理和确认开发。

## 开始贡献

1. Fork本项目的仓库。
2. Clone到本地。
3. 创建开发分支。
4. 本地测试：提交前请通过所有单元测试，包括新增的测试用例。
5. 提交代码。
6. 新建Pull Request。
7. 代码检视：您需要根据评审意见修改代码，并再次推送更新。此流程可能涉及多轮迭代。
8. 当您的PR获得足够数量的检视者批准后，Committer会进行最终审核。
9. 审核和测试通过后，CI会将您的PR合并入项目的主干分支。

详细步骤请参见[开发者指南](./development_guide.md)。

## 构建与测试

在提交PR之前，建议您先在本地搭建开发环境，构建`msMemScope`并运行相关测试。
[搭建开发及测试环境](./development_guide.md)。

## PR标题与分类

只有特定类型的PR才会被审核。请在PR标题前添加合适的前缀，以明确PR类型。请使用以下分类之一。

- `[feature]`: 新增模块功能、基础功能等。
- `[bugfix]`: 修复项目bug。
- `[refactor]`: 对现有功能模块进行重构。
- `[docs]`: 有关说明文档的新增或修改。
- `[test]`: 对UT/ST测试用例的新增或修改。
- `[build]`: 构建系统变更（CMake、build.py 等）。

若您的PR当前只是草稿状态，不需要审核，请使用如下标签。

- `[WIP]`: 草稿，勿 review。

## Commits要求

1. 每一次commit的message需要明确对应代码的功能，无效信息不会通过，如“添加适配文件”、“First commit”等。
2. 对于多余无效的commits需要压缩，如连续的相同commit messages的commits、连续的codecheck修改等。

若存在多次commit，请按照两种方式其一，压缩为一条commit记录（尽管GitCode在合并PR时提供了`Squash 合并`的选项, 提前将PR整理为单个简洁的commit仍然被视为最佳实践，并且深受committer们的欢迎）。

### 方式一：交互式变基（推荐）

1. 查看需要合并的最近几个commit（例如最近3个）。

    ``` shell
    git log --oneline -n 3
    ```

2. 选中需要合并的几条commit记录的前一条commit ID，运行命令。

    ```shell
    # commit_id 需要替换为实际值
    git rebase -i commit_id
    ```

3. 将要压缩的commits ID对应的pick改为squash，需要至少保留一个pick。
4. 保存退出之后，跳入第二个编辑环境，需要调整commit messages。
5. 修改对应pick的commit的message。
6. 保存退出。
7. 强制推送更新后的分支 (仅限于您自己的特性分支)。

    ```shell
    # branch_name 替换为实际分支名
    git push -f origin branch_name
    ```

8. 最后对应的PR改变为我们需要的状态。

### 方式二：reset + 新建commit

```bash
# 获取最新的待合入的目标分支（如main）
git fetch origin main

# Soft-reset到主干分支--此操作会保存所有修改并回归到暂存区。
git reset --soft origin/main

# 将所有更改提交为一个新的commit
git commit -m "feat: concise description of your change"

# 强制推送以更新PR分支
git push --force-with-lease origin your-branch-name

```

## PR合入流程

1. 提出PR后，评论`compile`，触发CI编译。
2. 等待CI编译完成，检查是否有编译错误。
3. 若编译通过，联系维护团队，请求检视合入。
4. 维护团队会对PR进行审核，若符合要求，会合并PR。

### 联系维护团队

若您在合入PR过程中遇到任何问题或需要进一步的帮助，请联系维护团队。您可以通过以下方式联系：

- 邮件：`memscope@outlook.com`
- 群聊：扫码添加昇腾开源小助手，获取群链接，进入 MindStudio 社区技术交流群，获取更多帮助和支持。详情请查看[群聊介绍](../communication_guide/communication.md#3-开源小助手)。

## 提交Issue

提交Issue相关操作请参见[Issue指南](../communication_guide/how_to_issue.md)。
