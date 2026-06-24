# **msMemScope Installation Guide**

## Instructions

This document describes msMemScope installation methods, including:

- **Using the CANN package**: The complete functions of msMemScope are integrated into the CANN package. You can directly install the CANN package by referring to [CANN Quick Installation](https://www.hiascend.com/cann/download).

- **Using the RUN package**: The complete functions of msMemScope are integrated into the CANN package and depend on CANN. Therefore, you need to install the CANN package before using msMemScope. To upgrade msMemScope to the latest version, you can compile the latest msMemScope RUN package using the source code to overwrite the existing package. For details, see [Method 2: Obtain the latest RUN package](#method-2-obtain-the-latest-run-package).

> [!NOTE]Note
> If a version earlier than CANN 8.5.0 is installed, install the CANN Toolkit as described in the training, inference, and development & debugging scenarios. If CANN 8.5.0 or a later version is installed, install the CANN Toolkit and OPS. Make preparations according to documents with specific version requirements.

## Installation via RUN Package

msMemScope can be used on Linux. Currently, you can obtain its RUN package in either of the following ways:

1. Stable version: Download the RUN package from the releases page.
2. Latest version: Compile and build the RUN package from the source code.

> [!NOTE]Note
> The RUN package can be used only after it is installed in an environment where CANN has been installed.

### Method 1: Obtain the Stable Version of the RUN Package

**Package Download**

Access [releases](https://gitcode.com/Ascend/msmemscope/releases) to download the target msMemScope package.

The package name is `mindstudio-memscope_<version>_linux-<arch>.run`, where `<version>` indicates the version number and `<arch>` indicates the CPU architecture.

Once you download the software, you agree to the terms and conditions of [Huawei Enterprise End User License Agreement (EULA)](https://e.huawei.com/en/about/eula).

**Package Verification**

After downloading the package, you are advised to verify its integrity (SHA256) before installation.

```bash
sha256sum mindstudio-memscope_<version>_linux-<arch>.run
echo "<expected-sha256> mindstudio-memscope_<version>_linux-<arch>.run" | sha256sum -c
```

**NOTE**

- In the verification command, `<expected-sha256>` is the SHA256 value obtained when the package is downloaded.
- For details about the SHA256 value of installation packages for each version, see [Release Notes](../release_notes/release_note.md).

Handling suggestions for inconsistent SHA256 values:

If `FAILED` is displayed in the output of `sha256sum -c -`, do not continue the installation.
Delete the current file, download it again, and perform the SHA256 verification again.
If the verification still fails, check whether the file name and version on the **releases** page are consistent, and report the issue.

### Method 2: Obtain the Latest RUN Package

#### Installing Dependencies

Before the installation, ensure that the Git and Python environments are available. For details, see [version requirements](../development_guide/development_guide.md#1-development-environment-settings). If requirements are not met, run the following command to install dependencies.

Debian:

```bash
sudo apt-get install -y python3 git build-essential cmake
```

openEuler:

```bash
sudo yum install -y python3 git gcc gcc-c++ make cmake
```

#### Compiling and Building the Package

1. Run the `git` command on the terminal to clone the msMemScope source code.

   ```bash
   git clone https://gitcode.com/Ascend/msmemscope.git <remote-name>
   ```

   Note: `remote-name` indicates the alias of the remote repository, which needs to be specified.

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

   Pack all the outputs into a .run package for installation and upgrade. If the following information is displayed, the packaging is successful.

   ```bash
   [INFO] Run file created successfully: xx/mindstudio-memscope_<version>_linux-<arch>.run
   Usage instructions:
     Install: bash mindstudio-memscope_<version>_linux-<arch>.run --install [--install-path=/path]
     Upgrade: bash mindstudio-memscope_<version>_linux-<arch>.run --upgrade --install-path=/path
     Version: bash mindstudio-memscope_<version>_linux-<arch>.run --version
     Help:    bash mindstudio-memscope_<version>_linux-<arch>.run --help
   ```

   Note: `arch` indicates the CPU architecture.
   After the compilation is complete, the package is generated in the `./build` directory.

### Installing the RUN Package

1. Grant the execute permission on the RUN package.

    ```shell
    chmod +x mindstudio-memscope_<version>_linux-<arch>.run
    ```

2. Install the package.

   ```bash
   bash mindstudio-memscope_<version>_linux-<arch>.run --install --install-path=<path>
   ```

   Note: `path` indicates the installation directory. If `--install-path` is not specified, the tool will automatically detect the `ASCEND_TOOLKIT_HOME` or `ASCEND_HOME_PATH` environment variable:

   - If either environment variable exists, you will be prompted to confirm whether to install to `$ASCEND_TOOLKIT_HOME/tools` (preferred) or `$ASCEND_HOME_PATH/tools`. If the msmemscope subdirectory already exists in that directory, upgrade mode will be automatically enabled.
   - If neither environment variable exists, or you choose not to install to the suggested path, the tool will install to the current directory by default.

   Install msMemScope in the `path` directory. After the installation is successful, the following information is displayed:

   ```bash
   source <path>/msmemscope/set_env.sh
   [INFO] Installation completed successfully
   ```

#### Verifying the Installation

Check whether the `set_env.sh` file is generated in the `<path>/msmemscope` directory.

### Configuration After Installation

Before using msMemScope, run the following command to configure the `PYTHONPATH` and `PATH` environment variables.

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

## Upgrade

The msMemScope package includes the upgrade script.

1. [Download](https://www.openlibing.com/apps/obsDetails?bucketName=ascend-package) the target package.

2. Run the following script to upgrade the software.

   ```bash
   bash mindstudio-memscope_<version>_linux-<arch>.run --upgrade --install-path=<path>
   ```

   Parameters:

   - `--upgrade` specifies the upgrade operation.
   - `--install-path` specifies the target directory. Only the selected directory is upgraded.

   If the following information is displayed, the software is successfully upgraded:

   ```bash
   [INFO] Upgrade completed successfully
   ```

## Uninstallation

**Uninstallation Using a Script**

1. Go to the path where msMemScope is installed.

   ```bash
   cd <path>/msmemscope
   ```

   Note: `path` indicates the installation path. Replace it with the actual path.

2. Run the following command to execute the uninstallation script.

   ```bash
   ./uninstall.sh
   ```

   The uninstallation program will display a message to confirm uninstallation. Enter **y** for confirmation or **n** for cancellation.

   If the following information is printed, the software is successfully uninstalled:

   ```tex
   [INFO] Uninstallation completed successfully
   ```

## Appendix A: Reference

### Options

This section describes the options related to the RUN package, which supports one-click installation using command line options. The commands can be used together. You can select installation options as required.

Installation command syntax: `./mindstudio-memscope_<version>_linux-<arch>.run [options]`

For details, see [Table 1](#cli-args-table).

  > [!NOTE]Note
  > If options queried by running the `./mindstudio-memscope_\<version>_linux-{arch}.run --help` command are not in the following table, they are reserved or apply to other products. You do not need to pay attention to them.

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
    <td>Installs the package. You can specify the installation path (--install-path=path) or omit it. If omitted, the tool will automatically detect the ASCEND_TOOLKIT_HOME or ASCEND_HOME_PATH environment variable and prompt you to confirm the installation location; if neither exists, it installs to the current directory.</td>
  </tr>
  <tr>
    <td>--upgrade</td>
    <td>Upgrades the installed software to a later version from an earlier version. To roll back from a later version to an earlier version, uninstall the later version and install the required version.</td>
  </tr>
  <tr>
    <td>--install-path</td>
    <td>Specifies the installation path (optional for installation, required for upgrade). This option must be used together with --install and --upgrade. If not specified during installation, the tool will automatically detect Ascend environment variables to determine the installation location.</td>
  </tr>
</tbody>
</table>
