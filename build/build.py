# Copyright (c) Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.

import os
import sys
import stat
import logging
import subprocess


def download_third_party():
    logging.info("============ start download third-party ============")
    cur_dir = os.path.realpath(os.path.dirname(__file__))
    prepare_shell = os.path.join(cur_dir, "download_thirdparty.sh")
    os.chmod(prepare_shell, stat.S_IRUSR | stat.S_IXGRP | stat.S_IXUSR | stat.S_IRGRP)
    cmd = ["/bin/sh", prepare_shell]
    result = subprocess.run(cmd, check=False)
    if result.returncode != 0:
        logging.error("download third-party failed")
        return result.returncode
    logging.info("============ download third-party done ============")
    return 0


if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO)

    buildLocal = "local" in sys.argv[1:]
    buildTests = "test" in sys.argv[1:]

    if buildLocal:
        download_ret = download_third_party()
        if download_ret != 0:
            sys.exit(download_ret)

    cmakeCmd = ["cmake", ".."]
    cmakeCmd.append("-DBUILD_TESTS=ON" if buildTests else "-DBUILD_TESTS=OFF")

    ret = subprocess.run(cmakeCmd, capture_output=False, check=False)
    if ret.returncode != 0:
        sys.exit(ret.returncode)

    ret = subprocess.run(["make", "-j8"], capture_output=False, check=False)
    sys.exit(ret.returncode)
