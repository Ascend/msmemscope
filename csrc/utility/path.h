/* -------------------------------------------------------------------------
 * This file is part of the MindStudio project.
 * Copyright (c) 2025 Huawei Technologies Co.,Ltd.
 *
 * MindStudio is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *
 *          http://license.coscl.org.cn/MulanPSL2
 *
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 * -------------------------------------------------------------------------
 */

#ifndef PATH_H
#define PATH_H

#include <string>
#include <vector>
#include <sys/stat.h>

namespace Utility {

class Path {
public:
    Path(void) noexcept;
    explicit Path(std::string path) noexcept;
    Path(Path const &) = default;
    Path(Path &&) = default;
    Path &operator=(Path const &) = default;
    Path &operator=(Path &&) = default;

    /// 将 Path 对象拼接为原始路径字符串
    std::string ToString(void) const;

    /// 获取路径中最后一个文件或目录名
    std::string Name(void) const;

    /// 获取父路径
    Path Parent(void) const &;
    Path Parent(void) &&;

    /// 获取绝对路径
    Path Absolute(void) const &;
    Path Absolute(void) &&;

    /// 路径正规化
    Path Resolved(void) const &;
    Path Resolved(void) &&;

    /// 路径拼接
    Path operator/(Path rhs) const &;
    Path operator/(Path rhs) &&;

    /// 获取路径文件状态
    bool GetStat(struct stat &st) const;

    /// 校验路径是否存在
    bool Exists(void) const;

    /// 校验路径是否可读
    bool IsReadable(void) const;

    /// 校验文件名长度
    bool IsValidLength(void) const;

    /// 校验路径深度
    bool IsValidDepth(void) const;

    /// 校验软链接
    bool IsSoftLink(void) const;

    /// 声明路径权限风险
    void DeclarePermissionRisk() const;

    /// 校验getcwd是否失败
    bool ErrorOccured(void) const { return errorOccurred_; }

private:
    bool absolute_;
    std::vector<std::string> route_;
    bool errorOccurred_ = false; // 对外提供getcwd失败的标记
};

bool CheckIsValidOutputPath(const std::string &path);
bool CheckIsValidInputPath(const std::string &path);

}  // namespace Utility

#endif