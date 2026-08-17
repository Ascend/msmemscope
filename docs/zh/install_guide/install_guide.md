# msMemScope 安装指南

<br>

## 1. 安装说明

本工具已集成于CANN中，若已安装CANN且无需更新此工具，可直接使用，无需按本文档安装。

若您的环境尚未安装CANN，请参见《[CANN 快速安装](https://www.hiascend.com/cann/download)》安装昇腾NPU驱动和CANN软件（包含Toolkit和ops包），并配置环境变量。

如需单独升级本工具或使用最新版本，您可通过以下三种方式进行安装：[在线安装](#21-在线安装)、[离线安装](#22-离线安装)、[源码安装](#23-源码安装)。

## 2. 安装方式

### 2.1 在线安装

若您的设备具备互联网访问能力，可通过一条命令自动完成工具的下载与安装。请参见昇腾社区MindStudio[下载](https://www.hiascend.com/developer/software/mindstudio/download)页面，选择对应的CANN版本，并在安装方式中选择"在线安装"，系统将引导您完成后续操作。

### 2.2 离线安装

对处于企业内网等无外网环境的设备，请先在可联网的机器上下载完整的离线安装包，再将其传输至目标设备进行安装。请参见昇腾社区MindStudio[下载](https://www.hiascend.com/developer/software/mindstudio/download)页面，选择对应的CANN版本，并在安装方式中选择"离线安装"，获取对应的安装包及操作指引。

### 2.3 源码安装

如需使用最新代码的功能，或对源码进行修改以增强功能，可下载本仓库代码，自行编译、打包工具并完成安装。

#### 2.3.1 环境准备

源码编译统一使用 MindStudio 标准构建环境。

- 日常开发或使用已发布镜像，请参考《[MindStudio工具开发环境安装指导](https://gitcode.com/Ascend/msot/blob/master/docs/zh/common/dev_env_setup.md)》。
- 需要从基础操作系统复现环境、执行源码构建验证或单元测试验证时，必须参考《[MindStudio统一构建镜像制作指南](https://gitcode.com/Ascend/msot/blob/master/docs/zh/common/docker_image_build_guide.md)》，从openEuler基础镜像现场构建环境镜像。

本文档后续的源码编译和单元测试命令，均在上述指定镜像容器或现场构建的环境镜像的容器中执行，CANN 软件包版本、GCC 版本和 Python 版本以统一镜像制作指南为准，本仓库不重复维护。

镜像构建完成后，必须使用统一镜像制作指南第 7 章给出的 `ctr_in.py` 命令，在交互式终端中启动并进入容器。不得使用普通 `docker run` 创建容器，也不得使用 `docker exec <容器名> bash -c '<命令>'` 替代交互式环境；否则可能跳过 Python、GCC 和 CANN 环境初始化。

进入 `ctr_in.py` 打开的交互式容器 Shell 后，执行如下命令克隆本仓库：

```bash
cd <path>/memscope   # 进入到memscope代码仓目录，path为代码仓项目所在路径
git clone https://gitcode.com/Ascend/msmemscope.git
```

#### 2.3.2 执行编译

保持在 `ctr_in.py` 打开的同一个交互式容器 Shell 中，在仓库根目录执行以下命令，自动完成依赖下载与构建：

```bash
cd <path>/memscope   # 进入到memscope代码仓目录，path为代码仓项目所在路径
python3 build.py
```

构建成功后，安装包将生成在 `artifacts/` 目录下。

#### 2.3.3 执行单元测试（可选）

此步骤非安装必需。如需验证代码基本功能，可执行单元测试：

```bash
cd ~/msmemscope
python3 build.py test
```

命令返回码为 0，且测试用例均无失败，表示单元测试通过。

#### 2.3.4 安装

##### 2.3.4.1 准备 run 包

安装软件包前需给run包添加可执行权限。进入run包保存路径，执行如下命令，增加可执行权限。

```bash
cd ~/msmemscope/artifacts
chmod +x mindstudio-memscope_<version>_linux-<arch>.run
```

##### 2.3.4.2 安装

将run包拷贝到运行环境中，执行以下命令安装。

```bash
bash mindstudio-memscope_<version>_linux-<arch>.run --install --install-path=<path>
```

注：其中`path`为安装目录。若未指定`--install-path`参数，工具将自动检测环境变量`ASCEND_TOOLKIT_HOME`或`ASCEND_HOME_PATH`：

- 若存在上述任一环境变量，将提示用户确认是否安装至`$ASCEND_TOOLKIT_HOME/tools`（优先）或`$ASCEND_HOME_PATH/tools`目录。若该目录下已存在msmemscope子目录，将自动启用升级模式。
- 若不存在上述环境变量，或用户选择不安装至推荐路径，则默认安装至当前目录。

当回显包含以下信息时，表示软件包安装成功。

```text
source <path>/msmemscope/set_env.sh
[INFO] Installation completed successfully
```

#### 2.3.5 安装后检查

请检查并确认安装目录：`<path>/msmemscope`下已生成`set_env.sh`文件。

#### 2.3.6 安装后配置

在使用msMemScope工具前，需执行以下命令，配置PYTHONPATH和PATH环境变量。

```bash
source <path>/msmemscope/set_env.sh
```

环境变量配置成功后，打印以下信息。

```text
Setting up msmemscope environment...
bash: local: can only be used in a function
✓ Added to PYTHONPATH (forced to front):<path>/msmemscope/python
bash: local: can only be used in a function
✓ Added to PATH (forced to front): <path>/msmemscope/bin
msmemscope environment setup completed
```

## 3. 验证安装

安装完成后，执行以下命令验证工具是否安装成功：

```bash
msmemscope --help
```

若输出不报错，且能显示帮助信息，则表明安装成功。

## 4. 卸载

> [!NOTE]
>
> 如果您在使用**内存采集功能**时按照《[**内存采集**](../user_guide//memory_profile.md#使用示例)》文档中的介绍已设置`LD_PRELOAD`环境变量，为避免卸载失败，在卸载前需要执行命令：`unset LD_PRELOAD` 重置环境变量。

可通过如下步骤卸载：

1. 下载脚本。

   ```bash
   curl -O https://inst.obs.cn-north-4.myhuaweicloud.com/26.1.0/ms_install.py
   ```

   > [!NOTE]
   >
   > - 需要联网环境才能下载，若环境不允许联网或处于离线状态，请先在可联网的环境下载该脚本后拷贝到目标设备。
   > - 若执行命令无响应或出现连接失败、SSL证书错误等问题，请参见[FAQ](https://www.hiascend.com/developer/blog/details/02176213671719317003)。

2. 执行卸载。

   ```bash
   python3 ms_install.py uninstall {tools_name}
   ```

   其中{tools_name}配置为需卸载的工具名称，可通过`python3 ms_install.py help`命令查询，在打印信息中的Available Tools字段下显示工具名称。

   卸载成功打印如下信息：

   ```text
   Successfully uninstalled 1 tool ({tools_name})
   ```

## 5. 升级

升级即“先卸后装”。直接执行安装命令，工具将自动卸载旧版本，并引导您完成覆盖安装。<br>
可通过`msmemscope --version`命令查看当前环境的版本信息，再选择需要升级的版本。升级版本时需要关注版本配套关系，请参见《[版本说明](https://gitcode.com/Ascend/release-management/blob/master/MindStudio/26.1.0/release_notes.md)》。

## 6. 附录

### 参数说明

本章节介绍了run格式（.run）软件包相关参数说明，run格式软件包支持通过命令行参数进行一键安装，各个参数之间可以配合使用，用户根据安装需要选择对应参数。

安装命令格式：`./mindstudio-memscope_<version>_linux-<arch>.run [options]`

详细参数请参见[表1](#cli-args-table)。

  > [!NOTE]
  >
  > 如果通过./mindstudio-memscope_\<version>_linux-{arch}.run --help命令查询出的参数没有在如下表格中解释，则说明该参数为预留参数或适用于其他产品类型，用户无需关注。

**表 1**  参数说明

<a id="cli-args-table"></a>

<table border="1" cellpadding="8" cellspacing="0" style="border-collapse: collapse; width: 100%;">
<thead>
  <tr>
    <th>参数</th>
    <th>说明</th>
  </tr></thead>
<tbody>
  <tr>
    <td>--help</td>
    <td>查询帮助信息。</td>
  </tr>
  <tr>
    <td>--version</td>
    <td>查询版本信息。</td>
  </tr>
  <tr>
    <td>--install</td>
    <td>安装软件包。后面可以指定安装路径--install-path=&lt;path&gt;，也可以不指定安装路径。若不指定，工具将自动检测环境变量ASCEND_TOOLKIT_HOME或ASCEND_HOME_PATH，并提示用户确认安装位置；若均不存在则安装至当前目录。</td>
  </tr>
  <tr>
    <td>--upgrade</td>
    <td>升级已安装的软件，支持在低版本升级至高版本情况下使用。 如果需要从高版本回退至低版本，需卸载高版本后重新安装所需版本。</td>
  </tr>
  <tr>
    <td>--install-path</td>
    <td>指定安装路径（可选），需配合安装--install、升级--upgrade参数使用。若不指定，安装时工具将自动检测Ascend环境变量确定安装位置，升级时必须指定。</td>
  </tr>
</tbody>
</table>
