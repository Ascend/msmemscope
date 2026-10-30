# msMemScope Installation Guide

<br>

## 1. Installation Description

This tool has been integrated into CANN. If CANN has been installed and this tool does not need to be updated, you can directly use it without following the instructions in this document.

If CANN has not been installed in your environment, install the Ascend NPU driver and CANN (including the Toolkit and ops) by referring to [CANN Quick Installation](https://www.hiascend.com/en/cann/download), and configure environment variables.

If you need to upgrade this tool separately or use the latest version, you can install it in any of the following ways: [Online Installation](#21-online-installation), [Offline Installation](#22-offline-installation), and [Installation from Source](#23-installation-from-source).

## 2. Installation Methods

### 2.1 Online Installation

If your device has Internet access, you can run a single command to automatically download and install the tool. Visit the [Ascend community](https://www.hiascend.com/en/developer/software/mindstudio/download) page, select the target CANN version, and choose "online installation". The system will guide you through the subsequent operations.

### 2.2 Offline Installation

For devices that are not connected to the Internet, such as those on an enterprise intranet, download the complete offline installation package on a device that has Internet access and then transfer the package to the target device for installation. Visit the [Ascend Community](https://www.hiascend.com/en/developer/software/mindstudio/download) page, select the target CANN version, and choose "offline installation" to obtain the corresponding installation package and operation guide.

### 2.3 Installation from Source

If you want to use the features of the latest code, or modify the source code to enhance functions, you can download the code of this repository, compile and package the tool by yourself, and complete the installation.

#### 2.3.1 Environment Preparation

Source code compilation uniformly uses the MindStudio standard build environment.

- For daily development or when using a released image, see the [MindStudio Tool Development Environment Setup Guide](https://gitcode.com/Ascend/msot/blob/master/docs/en/common/dev_env_setup.md).
- To reproduce the environment from a base OS, or to perform source code build verification or unit test verification, you must refer to the [MindStudio Image Build Guide](https://gitcode.com/Ascend/msot/blob/master/docs/en/common/docker_image_build_guide.md) and build the environment image on site from the openEuler base image.

The source code compilation and unit test commands in the rest of this document are executed in the container of the image specified above or of the on-site built environment image. The CANN package version, GCC version, and Python version are subject to the unified image production guide and are not maintained separately in this repository.

After the image is built, you must use the `ctr_in.py` command provided in Chapter 7 of the *Image Build Guide* to start and enter the container in an interactive terminal. Do not use a plain `docker run` to create a container, and do not use a plain `docker exec NAME bash -c 'CMD'` instead of an interactive environment; otherwise the initialization of the Python, GCC, and CANN environments may be skipped.

After entering the interactive container shell opened by `ctr_in.py`, run the following command to clone this repository:

```bash
cd <path>/memscope   # Go to the memscope code repository directory; path is the path where the code repository project resides.
git clone https://gitcode.com/Ascend/msmemscope.git
```

#### 2.3.2 Compiling

Staying in the same interactive container shell opened by `ctr_in.py`, run the following command in the repository root directory to automatically download dependencies and build:

```bash
cd <path>/memscope   # Go to the memscope code repository directory; path is the path where the code repository project resides.
python3 build.py
```

After the build succeeds, the installation package is generated in the `artifacts/` directory.

#### 2.3.3 Running Unit Tests (Optional)

This step is not mandatory for installation. To verify the basic functions of the code, you can run unit tests:

```bash
cd ~/msmemscope
python3 build.py test
```

A return code of `0` with no failed test cases indicates that the unit tests passed.

#### 2.3.4 Installation

##### 2.3.4.1 Preparing the RUN Package

Before installing the package, grant the execute permission on the RUN package. Go to the directory where the RUN package is saved and run the following command to add the execute permission.

```bash
cd ~/msmemscope/artifacts
chmod +x mindstudio-memscope_<version>_linux-<arch>.run
```

##### 2.3.4.2 Installation Procedure

Copy the RUN package to the running environment and run the following command to install it.

```bash
bash mindstudio-memscope_<version>_linux-<arch>.run --install --install-path=<path>
```

Note: `path` indicates the installation directory. If `--install-path` is not specified, the tool automatically detects the `ASCEND_TOOLKIT_HOME` or `ASCEND_HOME_PATH` environment variable:

- If either of the above environment variables exists, you are prompted to confirm whether to install to the `$ASCEND_TOOLKIT_HOME/tools` directory (preferred) or the `$ASCEND_HOME_PATH/tools` directory. If an `msmemscope` subdirectory already exists in that directory, upgrade mode is automatically enabled.
- If neither environment variable exists, or if the user chooses not to install to the recommended path, the tool is installed to the current directory by default.

If the following information is displayed, the package is installed successfully.

```text
source <path>/msmemscope/set_env.sh
[INFO] Installation completed successfully
```

#### 2.3.5 Post-Installation Check

Check whether the `set_env.sh` file is generated in the `<path>/msmemscope` directory.

#### 2.3.6 Post-Installation Configuration

Before using msMemScope, run the following command to configure the `PYTHONPATH`and `PATH` environment variables.

```bash
source <path>/msmemscope/set_env.sh
```

After the environment variables are configured, the following information is displayed:

```text
Setting up msmemscope environment...
bash: local: can only be used in a function
✓ Added to PYTHONPATH (forced to front):<path>/msmemscope/python
bash: local: can only be used in a function
✓ Added to PATH (forced to front): <path>/msmemscope/bin
msmemscope environment setup completed
```

## 3. Installation Verification

After installation, run the following command to check whether the tool has been successfully installed:

```bash
msmemscope --help
```

If no error is outputted and the help information is displayed, the tool has been successfully installed.

## 4. Uninstallation

> [!NOTE]
>
> If you have set `LD_PRELOAD` as described in [Memory Collection](../user_guide//memory_profile.md#usage-example) when enabling memory collection, run `unset LD_PRELOAD` to reset the environment variable before uninstallation, to avoid failures.

Perform the following steps to uninstall the tool:

1. Download the script.

   ```bash
   curl -O https://inst.obs.cn-north-4.myhuaweicloud.com/26.2.0/ms_install.py
   ```

   > [!NOTE]
   >
   > - An internet connection is required for downloading. If the environment does not allow internet access or is in an offline state, download the script on a device with internet connectivity first, and then copy it to the target device.
   > - If the command does not respond, or if you encounter connection failures, SSL certificate errors, or other issues, please refer to the [FAQs](https://www.hiascend.com/developer/blog/details/02176213671719317003).

2. Uninstall the tool.

   ```bash
   python3 ms_install.py uninstall {tools_name}
   ```

   Replace `{tools_name}` with the name of the tool to be uninstalled. You can run the `python3 ms_install.py help` command to query the tool name, which is displayed under the `Available Tools` field in the command output.

   If the uninstallation is successful, the following information is displayed:

   ```text
   Successfully uninstalled 1 tool ({tools_name})
   ```

## 5. Upgrade

Upgrading follows a "uninstall first, then install" approach. Simply run the installation command, and the tool will automatically uninstall the old version and guide you through the overlay installation process.
Run `msmemscope --version` to view current version information before selecting the target version for upgrade. For details about version mapping, see [Release Notes](https://gitcode.com/Ascend/release-management/blob/master/MindStudio/26.2.0/release_notes_en.md).

## 6. Appendix

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
