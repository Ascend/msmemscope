# **msMemScope安装指南**

## 1. 安装说明

本工具已集成于CANN中，若已安装CANN且无需更新此工具，可直接使用，无需按本文档安装。

若您的环境尚未安装CANN，请参见《[CANN 快速安装](https://www.hiascend.com/cann/download)》安装昇腾NPU驱动和CANN软件（包含Toolkit和ops包），并配置环境变量。

如需单独升级本工具或使用最新版本，您可通过以下三种方式进行安装：[在线安装](#21-在线安装)、[离线安装](#22-离线安装)、[源码安装](#23-源码安装)。

## 2. 安装方式

### 2.1 在线安装

若您的设备具备互联网访问能力，可通过一条命令自动完成工具的下载与安装。请参见昇腾社区MindStudio[下载](https://www.hiascend.com/developer/software/mindstudio/download)页面，选择对应的CANN版本，并在安装方式中选择“在线安装”，系统将引导您完成后续操作。

### 2.2 离线安装

对处于企业内网等无外网环境的设备，请先在可联网的机器上下载完整的离线安装包，再将其传输至目标设备进行安装。请参见昇腾社区MindStudio[下载](https://www.hiascend.com/developer/software/mindstudio/download)页面，选择对应的CANN版本，并在安装方式中选择“离线安装”，获取对应的安装包及操作指引。

### 2.3 源码安装

#### 2.3.1 安装依赖

安装前需确保Git、Python等环境可用，请满足[版本依赖](../development_guide/development_guide.md#1-开发环境配置)限制，若不满足可执行以下命令安装。

Debian系列：

```bash
sudo apt-get install -y python3 git build-essential cmake
```

openEuler系列：

```bash
sudo yum install -y python3 git gcc gcc-c++ make cmake
```

#### 2.3.2 编译构建run包

1. 在终端执行以下git命令，克隆（clone）msMemScope源码。

   ```bash
   git clone https://gitcode.com/Ascend/msmemscope.git <remote-name>
   ```

   注：其中`remote-name`为远程仓库别名，需要指定。

2. 执行以下命令下载Python三方依赖。注：`sqlite3`为离线功能使用依赖，可选安装。

   ```bash
   pip3 install -r ./requirements.txt
   ```

3. 下载构建依赖以及编译。

   ```bash
   cd ./<remote-name>/build
   python3 build.py local test
   ```

   其中参数说明如下：

   - `local`：代表是否本地构建，添加会下载gtest、json等依赖库用于本地构建，一般只有第一次需要，除非依赖库有更新。
   - `test`：代表是否要构建测试用例。

4. 在`./build`目录下执行以下命令，编译软件包。

   ```bash
   bash make_run.sh
   ```

   将工具的产物打包成一个run包，回显信息如下，表示打包成功，该包支持安装和升级的能力。

   ```bash
   [INFO] Run file created successfully: xx/mindstudio-memscope_<version>_linux-<arch>.run
   Usage instructions:
     Install: bash mindstudio-memscope_<version>_linux-<arch>.run --install --install-path=/path
     Upgrade: bash mindstudio-memscope_<version>_linux-<arch>.run --upgrade --install-path=/path
     Version: bash mindstudio-memscope_<version>_linux-<arch>.run --version
     Help:    bash mindstudio-memscope_<version>_linux-<arch>.run --help
   ```

   注：其中`arch`表示CPU架构。
   编译完成后，会在`./build`目录下生成软件包。

#### 2.3.3 安装run包

1. 增加对run包的可执行权限。

    ```shell
    chmod +x mindstudio-memscope_<version>_linux-<arch>.run
    ```

2. 执行以下命令，安装软件包。

   ```bash
   bash mindstudio-memscope_<version>_linux-<arch>.run --install --install-path=<path>
   ```

   注：其中`path`为安装目录。

   将msMemScope安装在`path`目录下，安装成功后，打印以下信息。

   ```bash
   source <path>/msmemscope/set_env.sh
   [INFO] Installation completed successfully
   ```

#### 2.3.4 安装后检查

请检查并确认安装目录：`<path>/msmemscope`下已生成`set_env.sh`文件。

#### 2.3.5 安装后配置

在使用msMemScope工具前，需执行以下命令，配置PYTHONPATH和PATH环境变量。

```bash
source <path>/msmemscope/set_env.sh
```

环境变量配置成功后，打印以下信息。

```tex
Setting up msmemscope environment...
bash: local: can only be used in a function
✓ Added to PYTHONPATH (forced to front):<path>/msmemscope/python
bash: local: can only be used in a function
✓ Added to PATH (forced to front): <path>/msmemscope/bin
msmemscope environment setup completed
```

## 3. 卸载

> [!NOTE]
>
> 如果您在使用**内存采集功能**时按照《[**内存采集**](../user_guide//memory_profile.md#使用示例)》文档中的介绍已设置`LD_PRELOAD`环境变量，为避免卸载失败，在卸载前需要执行命令：`unset LD_PRELOAD` 重置环境变量。

可通过如下步骤卸载：

1. 下载脚本。

   ```bash
   curl -O https://inst.obs.cn-north-4.myhuaweicloud.com/26.0.0/ms_install.py
   ```

   > [!NOTE]
   >
   > - 需要联网环境才能下载，若环境不允许联网或离线状态，请先在可联网的环境下载该脚本后拷贝到目标设备。
   > - 若执行命令无响应或出现连接失败、SSL证书错误等问题，请参见[FAQ](https://www.hiascend.com/developer/blog/details/02176213671719317003)。

2. 执行卸载。

   ```bash
   python ms_install.py uninstall {tools_name}
   ```

   其中{tools_name}配置为需要卸载的工具名称，可通过`python ms_install.py help`命令查询，在打印信息中的Available Tools字段下显示工具名称。

   卸载成功打印如下信息：

   ```ColdFusion
   Successfully uninstalled 1 tool ({tools_name})
   ```

## 4. 升级

升级即“先卸后装”。直接执行安装命令，工具将自动卸载旧版本，并引导您完成覆盖安装。

## 5. 附录

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
    <td>安装软件包。后面可以指定安装路径--install-path=&lt;path&gt;，也可以不指定安装路径，直接安装到默认路径下。</td>
  </tr>
  <tr>
    <td>--upgrade</td>
    <td>升级已安装的软件，支持在低版本升级至高版本情况下使用。 如果需要从高版本回退至低版本，需卸载高版本后重新安装所需版本。</td>
  </tr>
  <tr>
    <td>--install-path</td>
    <td>指定安装路径，需配合安装--install、升级--upgrade参数使用。</td>
  </tr>
</tbody>
</table>
