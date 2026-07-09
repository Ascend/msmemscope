# **msMemScope Installation Guide**

## 1. Installation Description

This tool has been integrated into CANN. If CANN has been installed and this tool does not need to be updated, you can directly use it without following the instructions in this document.

If CANN has not been installed in your environment, install the Ascend NPU driver and CANN software (including the Toolkit and ops) by referring to [CANN Quick Installation](https://www.hiascend.com/cann/download), and configure environment variables.

If you need to upgrade this tool separately or use the latest version, you can install it in any of the following ways: [Online Installation](#21-online-installation), [Offline Installation](#22-offline-installation), and [Installation from Source](#23-installation-from-source).

## 2. Installation Methods

### 2.1 Online Installation

If your device has Internet access, you can run a single command to automatically download and install the tool. Visit the [Ascend community](https://www.hiascend.com/developer/software/mindstudio/download ), select the target CANN version, and choose "online installation". The system will guide you through the subsequent operations.

### 2.2 Offline Installation

For devices that are not connected to the Internet, such as those on an enterprise intranet, download the complete offline installation package on a device that has Internet access and then transfer the package to the target device for installation. Visit the [Ascend community](https://www.hiascend.com/developer/software/mindstudio/download ), select the target CANN version, and choose "offline installation". The system will guide you through the subsequent operations.

### 2.3 Installation from Source

#### 2.3.1 Installing Dependencies

Before the installation, ensure that the Git and Python environments are available. For details, see [version requirements](./development_guide/development_guide.md#development environment-settings). If requirements are not met, run the following command to install dependencies.

Debian:

```bash
sudo apt-get install -y python3 git build-essential cmake
```

openEuler:

```bash
sudo yum install -y python3 git gcc gcc-c++ make cmake
```

#### 2.3.2 Compiling and Building a RUN Package

1. Run the following git command on the terminal to clone the msMemScope source code.

   ```bash
   git clone https://gitcode.com/Ascend/msmemscope.git -b 26.0.0
   ```

2. Download the Python third-party dependencies. Note that `sqlite3` is an optional choice and is used for offline functions.

   ```bash
   pip3 install -r ./requirements.txt
   ```

3. Download and build dependencies.

   ```bash
   cd ./<remote-name>/build
   python3 build.py local test
   ```

   Parameters:

   - `local`: local building. If this parameter is added, dependencies such as gtest and json are downloaded for local building. Generally, these dependencies are downloaded only for the first building unless they are updated.
   - `test`: test cases.

4. Compile the package in the `./build` directory.

   ```bash
   bash make_run.sh
   ```

   Pack all the outputs into a RUN package for installation and upgrade. If the following information is displayed, packaging is successful.

   ```bash
   [INFO] Run file created successfully: xx/mindstudio-memscope_<version>_linux-<arch>.run
   Usage instructions:
     Install: bash mindstudio-memscope_<version>_linux-<arch>.run --install --install-path=/path
     Upgrade: bash mindstudio-memscope_<version>_linux-<arch>.run --upgrade --install-path=/path
     Version: bash mindstudio-memscope_<version>_linux-<arch>.run --version
     Help:    bash mindstudio-memscope_<version>_linux-<arch>.run --help
   ```

   Note: `arch` indicates the CPU architecture.
   After the compilation is complete, the package is generated in the `./build` directory.

#### 2.3.3 Installing the RUN Package

1. Grant the execute permission on the RUN package.

    ```shell
    chmod +x mindstudio-memscope_<version>_linux-<arch>.run
    ```

2. Install the package.

   ```bash
   bash mindstudio-memscope_<version>_linux-<arch>.run --install --install-path=<path>
   ```

   Note: `path` indicates the installation directory.

   Install msMemScope in the `path` directory. After the installation is successful, the following information is displayed:

   ```bash
   source <path>/msmemscope/set_env.sh
   [INFO] Installation completed successfully
   ```

#### 2.3.4 Verifying the Installation

Check whether the `set_env.sh` file is generated in the `<path>/msmemscope` directory.

#### 2.3.5 Configuring Environment Variables

Before using msMemScope, run the following command to configure the `PYTHONPATH`and `PATH` environment variables.

```bash
source <path>/msmemscope/set_env.sh
```

After the environment variables are configured, the following information is displayed:

```tex
Setting up msmemscope environment...
bash: local: can only be used in a function
✓ Added to PYTHONPATH (forced to front):<path>/msmemscope/python
bash: local: can only be used in a function
✓ Added to PATH (forced to front): <path>/msmemscope/bin
msmemscope environment setup completed
```

## 3. Uninstallation

> [!NOTE]
> 
> If you have set the `LD_PRELOAD` environment variable when using the [memory profiling function](./memory_profile.md), run the `unset LD_PRELOAD` command to reset the environment variable before uninstallation to avoid uninstallation failure.

Perform the following steps to uninstall the tool:

1. Download the script.

   ```bash
   curl -O https://inst.obs.cn-north-4.myhuaweicloud.com/26.0.0/ms_install.py
   ```

   > [!NOTE]
   >
   > - An internet connection is required for downloading. If the environment does not allow internet access or is offline, download the script on a machine with internet connectivity first, and then copy it to the target device.
   > - If the command does not respond, or if you encounter connection failures, SSL certificate errors, or other issues, please refer to the [FAQs](https://www.hiascend.com/developer/blog/details/02176213671719317003).

2. Uninstall the tool.

   ```bash
   python ms_install.py uninstall {tools_name}
   ```

   Replace `{tools_name}` with the name of the tool to be uninstalled. You can run the `python ms_install.py help` command to query the tool name, which is displayed under the `Available Tools` field in the command output.

   If the uninstallation is successful, the following information is displayed:

   ```ColdFusion
   Successfully uninstalled 1 tool ({tools_name})
   ```

## 4. Upgrade

Upgrading follows a "uninstall first, then install" approach. Simply run the installation command, and the tool will automatically uninstall the old version and guide you through the overlay installation process.

## 5. Appendix

### Option Description

This section describes the parameters related to the RUN package. This package supports one-click installation using command line options. The options can be used together. You can select the options as required.

Installation command syntax: `./mindstudio-memscope_<version>_linux-<arch>.run [options]`

For details, see [Table 1](#cli-args-table).

  > [!NOTE]
  > 
  > If options queried by running the `./mindstudio-memscope_<version>_linux-{arch}.run --help` command are not in the following table, they are reserved or apply to other products. You do not need to pay attention to them.

**Table 1** Options

<a id="cli-args-table"></a>

<table border="1" cellpadding="8" cellspacing="0" style="border-collapse: collapse; width: 100%;">
<thead>
  <tr>
    <th>Option</th>
    <th>Description</th>
  </tr></thead>
<tbody>
  <tr>
    <td>--help</td>
    <td>Queries help information.</td>
  </tr>
  <tr>
    <td>--version</td>
    <td>Queries version information.</td>
  </tr>
  <tr>
    <td>--install</td>
    <td>Installs the package. You can specify the installation path `--install-path=&lt;path&gt` or use the default installation path.</td>
  </tr>
  <tr>
    <td>--upgrade</td>
    <td>Upgrades the installed software to a later version from an earlier version. To roll back from a later version to an earlier version, uninstall the later version and install the required version.</td>
  </tr>
  <tr>
    <td>--install-path</td>
    <td>Specifies the installation path. This option must be used together with --install and --upgrade.</td>
  </tr>
</tbody>
</table>
