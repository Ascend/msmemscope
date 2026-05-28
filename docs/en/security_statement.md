# **msMemScope Security Statement**

Before using the tool, read the following security instructions carefully to prevent potential risks.

## Installation and Usage Constraints

  msMemScope is a development and debugging tool and should not be used in the production environment.

## File Verification Constraints

  Use verification methods like SHA256 to verify the integrity of downloaded files (especially model weight files) to ensure that the files are secure and reliable, thereby avoiding potential security risks.

## File Permission Constraints

  - For security purpose and the principle of least privilege, you are advised to use a common user instead of a high-privilege user (such as root) to install and use msMemScope.
  - Follow the principle of least privilege. For example, prevent other users (**others**) from writing data by disabling permissions like 666 and 777.
  - Ensure that the execution user's **umask** value is greater than or equal to **0027**; otherwise, the permissions of directories and files where performance data is collected may be too high.
  - Ensure that performance data is saved in the current user's directory and the directory does not contain symbolic links, to prevent potential security problems.

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
