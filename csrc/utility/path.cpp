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

#include "path.h"

#include <algorithm>
#include <iostream>
#include <unistd.h>
#include <sys/stat.h>
#include <linux/limits.h>

#include "ustring.h"
#include "securec.h"

namespace Utility {

constexpr char const *PATH_SEP = "/";
constexpr char const *CURRENT_SEGMENT = ".";
constexpr char const *PARENT_SEGMENT = "..";
constexpr const uint32_t PATH_DEPTH_MAX = 32;

Path::Path(void) noexcept : absolute_{false}
{}

/* Path 构造函数
 * 构造函数中的核心逻辑为将一个路径字符串解析为一系列由 PATH_SEP 分隔的构件（components）组成，
 * 由 std::vector<std::string> 表示。路径可分为相对路径和绝对路径。相对路径在 Path 中表示为第一
 * 个构件不以分隔符开头，如 `["test", "test.cpp"]`；绝对路径的第一个构件以分隔符开头，如
 * `["/test", "test.cpp"]`。
 */
Path::Path(std::string path) noexcept : absolute_{false}
{
    path = Strip(path);
    /// 空路径表示当前路径 "."，并以空列表表示
    if (path.empty()) {
        return;
    }

    std::string route;
    std::string::size_type slow = 0UL;
    std::string::size_type fast = path.find_first_of(PATH_SEP, slow);
    /// 如果路径以分隔符开头需要特殊处理开头的部分
    if (fast == slow) {
        std::string::size_type next = path.find_first_not_of(PATH_SEP, fast);
        fast = path.find_first_of(PATH_SEP, next);
        absolute_ = true;
        /// 如果 next == std::string::npos 说明路径由若干分隔符组成，处理成 ["/"]
        if (next == std::string::npos) {
            return;
        } else if (fast == std::string::npos) {
            route = path.substr(next);
        } else {
            route = path.substr(next, fast - next);
        }
        route_.emplace_back(route);
        slow = path.find_first_not_of(PATH_SEP, fast);
    }

    for (; slow != std::string::npos;) {
        fast = path.find_first_of(PATH_SEP, slow);
        route = fast == std::string::npos ? path.substr(slow) : path.substr(slow, fast - slow);
        if (route != CURRENT_SEGMENT) {
            route_.emplace_back(route);
        }
        slow = path.find_first_not_of(PATH_SEP, fast);
    }
}

std::string Path::ToString(void) const
{
    std::string raw;
    /// 空列表表示当前路径 "."
    if (absolute_) {
        raw = PATH_SEP;
    } else if (route_.empty()) {
        return CURRENT_SEGMENT;
    }

    raw.append(Join(route_.cbegin(), route_.cend(), PATH_SEP));
    return raw;
}

std::string Path::Name(void) const
{
    if (route_.empty()) {
        return "";
    }
    return route_.back();
}

Path Path::Parent(void) const &
{
    return Path(*this).Parent();
}

Path Path::Parent(void) &&
{
    if (!route_.empty()) {
        route_.pop_back();
    }
    return std::move(*this);
}

Path Path::Absolute(void) const &
{
    return Path(*this).Absolute();
}

Path Path::Absolute(void) &&
{
    char buf[PATH_MAX] = {0};
    std::string cwd;
    if (getcwd(buf, sizeof(buf))) {
        cwd = buf;
    } else {
        std::cout << "[msmemscope] Error: Failed to get current working directory" << std::endl;
        errorOccurred_ = true;
    }
    return Path(cwd) / std::move(*this);
}

Path Path::Resolved(void) const &
{
    return Path(*this).Resolved();
}

Path Path::Resolved(void) &&
{
    Path path = std::move(*this).Absolute();
    auto fast = path.route_.cbegin();
    auto slow = path.route_.begin();
    for (; fast != path.route_.cend(); ++fast) {
        if (*fast != PARENT_SEGMENT) {
            *slow++ = *fast;
        } else if (slow > path.route_.begin()) {
            --slow;
        }
    }
    path.route_.erase(slow, path.route_.end());
    return path;
}

Path Path::operator/(Path rhs) const &
{
    return Path(*this) / rhs;
}

Path Path::operator/(Path rhs) &&
{
    if (rhs.absolute_) {
        return std::move(rhs);
    }

    for (auto &r : rhs.route_) {
        route_.emplace_back(std::move(r));
    }
    return std::move(*this);
}

bool Path::GetStat(struct stat &st) const
{
    return stat(this->ToString().c_str(), &st) == 0;
}

bool Path::Exists(void) const
{
    struct stat st {};
    return stat(this->ToString().c_str(), &st) == 0;
}

bool Path::IsReadable(void) const
{
    return access(this->ToString().c_str(), R_OK) == 0;
}

bool Path::IsValidDepth(void) const
{
    std::string path = this->ToString();
    return std::count(path.begin(), path.end(), *PATH_SEP) <= PATH_DEPTH_MAX;
}

bool Path::IsValidLength(void) const
{
    std::size_t pathNameLength = 0;
    for (auto it = this->route_.cbegin(); it != this->route_.cend(); ++it) {
        std::size_t fileNameLength = it->length();
        if (fileNameLength > NAME_MAX || fileNameLength == 0) {
            return false;
        }
        pathNameLength += fileNameLength;
    }
    if (pathNameLength > PATH_MAX || pathNameLength == 0) {
        return false;
    }
    return true;
}

bool Path::IsSoftLink(void) const
{
    struct stat buf{};
    (void)memset_s(&buf, sizeof(buf), 0, sizeof(buf));
    return lstat(this->ToString().c_str(), &buf) == 0 && (S_IFMT & buf.st_mode) == S_IFLNK;
}

void Path::DeclarePermissionRisk(void) const
{
    struct stat st;
    if (stat(this->ToString().c_str(), &st) != 0) {
        std::cout << "[msmemscope] Error: Failed to stat path: " << this->ToString() << " ." << std::endl;
        return;
    }

    // 检查属主是否为 root 或当前用户
    uid_t currentUid = geteuid();
    if (st.st_uid != currentUid) {
        if (currentUid == 0) {
            std::cout << "[msmemscope] Warn: Process is running as user root. "
                "Please confirm the path " << ToString() << " is trusted." << std::endl;
        } else {
            std::cout << "[msmemscope] Warn: The path " << ToString() <<
                " is not owned by the current user. Please confirm it is trusted." << std::endl;
        }
    }

    // 检查 group 和 other 是否有写权限
    if ((st.st_mode & S_IWGRP) != 0 || (st.st_mode & S_IWOTH) != 0) {
        std::cout << "[msmemscope] Warn: The path " << ToString() <<
            " can be modified by other users. Please confirm it is trusted." << std::endl;
    }

    return;
}

// 对于所有路径的公共检查：包含可读性，路径长度，权限校验（group和other用户组不可写，属主为root或当前用户
bool CheckIsValidInputPath(const std::string &path)
{
    if (path.empty()) {
        std::cout << "[msmemscope] Error: The file path is empty." << std::endl;
        return false;
    }

    Utility::Path inputPath = Utility::Path{path};
    Utility::Path realPath = inputPath.Resolved();
    if (realPath.ErrorOccured()) {
        return false;
    }
    std::string temp = realPath.ToString();

    if (!realPath.Exists()) {
        std::cout << "[msmemscope] Error: The file path " << temp << " do not exist." << std::endl;
        return false;
    }
    if (!realPath.IsReadable()) {
        std::cout << "[msmemscope] Error: The path " << temp << " is not readable." << std::endl;
        return false;
    }
    if (!realPath.IsValidLength()) {
        std::cout << "[msmemscope] Error: The length of path " << temp << " exceeds the maximum length." << std::endl;
        return false;
    }
    if (!realPath.IsValidDepth()) {
        std::cout << "[msmemscope] Error: The depth of path " << temp << " exceeds the maximum depth." << std::endl;
        return false;
    }
    realPath.DeclarePermissionRisk();
    return true;
}
// 特别的，对--output的参数校验，不应校验其存在性，全面的校验在Create前完成
// --output指定的参数是一个路径同时也是一个字符串，该路径未必存在，对不存在的路径校验权限、可读性是没有意义的
// 但是长度、深度、非法字符是字符串层面的校验，与路径是否存在无关，可以直接执行
bool CheckIsValidOutputPath(const std::string &path)
{
    if (path.empty()) {
        std::cout << "[msmemscope] Error: The file path is empty." << std::endl;
        return false;
    }

    Utility::Path outputPath = Utility::Path{path};
    Utility::Path realPath = outputPath.Resolved();
    if (realPath.ErrorOccured()) {
        return false;
    }
    std::string temp = realPath.ToString();
    if (CheckStrIsStartsWithInvalidChar(temp.c_str())) {
        std::cout << "[msmemscope] Error: The path " << temp << " is invalid." << std::endl;
        return false;
    }
    if (!realPath.IsValidLength()) {
        std::cout << "[msmemscope] Error: The length of path " << temp << " exceeds the maximum length." << std::endl;
        return false;
    }
    if (!realPath.IsValidDepth()) {
        std::cout << "[msmemscope] Error: The depth of path " << temp << " exceeds the maximum depth." << std::endl;
        return false;
    }
    if (realPath.Exists()) {
        if (!realPath.IsReadable()) {
            std::cout << "[msmemscope] Error: The path " << temp << " is not readable." << std::endl;
            return false;
        }
        realPath.DeclarePermissionRisk();
    }
    return true;
}
}  // namespace Utility
