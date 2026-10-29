# **msMemScope Security Statement**

Before using the tool, read the following security usage statement carefully to prevent potential risks. msMemScope is a development and debugging tool and must not be used in the production environment.

## System Security Hardening

- This repository provides VS Code and devcontainer configurations for development and debugging. Use them only to develop and debug this repository in a secure debugging environment. Review the configuration security and adjust it to your actual requirements.
- Use verification methods such as SHA256 to verify the integrity of the downloaded msMemScope installation package and related files, ensuring that the file source is secure and trustworthy and thereby effectively avoiding potential security risks.

## Recommendations for the Running User

For security and the principle of least privilege, do not install or run msMemScope with an administrator-type account such as root. Use a standard user permission instead. When the tool is run as the root user, it prints a prompt and skips the file permission verification, which poses security risks.

## File Permission Control

- Follow the principle of least privilege (for example, prevent **others** from writing data by disabling permissions such as 666 and 777).
- Ensure that the **umask** value of the execution user is greater than or equal to **0027**. Otherwise, the permissions of the directories and files where the collected performance data is located may be too high.
- Ensure that the input information provided by users is secure and trustworthy, and avoid symbolic links. Convert all files to their real absolute paths before input to prevent potential security problems.

**File Permission Reference**

| Type                              | Maximum Permission in Linux|
| ---------------------------------- | ------------------- |
| Home directory                        | 750 (rwxr-x---)   |
| Program files (including scripts and libraries)    | 550 (r-xr-x---)   |
| Program file directory                      | 550 (r-xr-x---)   |
| Configuration files                          | 640 (rw-r-----)   |
| Configuration file directory                      | 750 (rwxr-x---)   |
| Log files (recorded or archived)    | 440 (r--r-----)   |
| Log files (being recorded)                | 640 (rw-r-----)   |
| Log file directory                      | 750 (rwxr-x---)   |
| Debug files                         | 640 (rw-r-----)   |
| Debug file directory                     | 750 (rwxr-x---)   |
| Temporary file directory                      | 750 (rwxr-x---)   |
| Maintenance and upgrade file directory                  | 770 (rwxrwx---)   |
| Service data files                      | 640 (rw-r-----)   |
| Service data file directory                  | 750 (rwxr-x---)   |
| Key component, private key, certificate, and ciphertext file directory| 700 (rwx------)    |
| Key components, private keys, certificates, and ciphertext files    | 600 (rw-------)   |
| APIs and scripts for encryption and decryption            | 500 (r-x------)   |

## Data Security

During collection and analysis, msMemScope generates performance data files in the output directory (`memscopeDumpResults` by default), including memory event records, Python and C call stacks, and memory snapshots. When the full-content mode of memory block monitoring is enabled, complete Tensor data is also saved to the drive. Host heap memory leak detection generates leak overview files and memory block detail files. These files may contain sensitive information such as user code call stacks and service data content. Pay attention to the permission control of the output directory, and clean up or properly keep the files after use.

## Runtime Security

When an exception occurs during running, msMemScope exits the process and prints an error message. Locate the specific error cause based on the error message. Tool logs are saved to the `msmemscope_logs` subdirectory of the output directory and can be used as a reference for fault locating.

## Public Network Addresses

Not involved. All msMemScope functions are executed on the local host and do not access any public network address.

## Public Interfaces

msMemScope exposes Python APIs and command-line parameters. The Python APIs include collection control, memory snapshot collection, custom Trace instrumentation, and offline memory analysis (`analyze`, `check_leaks`, `check_inefficient`, and others). The command line supports parameters such as memory collection, memory comparison, and the out-of-process control channel. All interfaces are executed on the local host and do not involve network transmission. For details, see *[API Reference](../api_reference/api.md)* and *[Memory Collection](../user_guide/memory_profile.md)*.

## Communication Matrix

msMemScope does not involve network communication. The only inter-process communication is provided by the out-of-process control channel (dynamic attach) function, and the communication scope is limited to the local host, as listed in the following table.

| Communication Type | Communication Protocol/Mode | Communication Peer | Socket/Port | Purpose | Remarks |
| -------- | ------------- | -------- | ----------- | ---- | ---- |
| Local inter-process communication | Unix domain socket | msMemScope controller and target process | `/tmp/msmemscope_socket_<PID>` | Delivers control words such as start/stop, inspection, and configuration query/modification | The socket file exists only during the handshake phase and is deleted after the target process is registered successfully |
