#pragma once
#include <mutex>
#include <unordered_map>
#include <memory>
#include <string>
namespace Utility {

// FileWriteManager用于控制文件的写入操作,避免同时对同一文件进行写入
// 其维护一个key为device_id,value为lock的哈希表,如需写入,可从这里拿到文件写入锁，进行加锁写入
// 当前只有开启traceback以及落盘文件格式为db时才会涉及到同时写入，其余不涉及
class FileWriteManager {
 public:
     static FileWriteManager& GetInstance() {
        static FileWriteManager instance;
        return instance;
     }

     std::mutex& GetLock(const int32_t& device_id) {
         std::lock_guard<std::mutex> lock(map_mutex_);
         auto it = device_locks_.find(device_id);
         if (it == device_locks_.end()) {
             // 创建新的锁并返回
             auto lock_ptr = std::make_shared<std::mutex>();
             device_locks_[device_id] = lock_ptr;
             return *lock_ptr;
         }
         return *(it->second);
     }

 private:
     FileWriteManager() = default;
     ~FileWriteManager() = default;

     FileWriteManager(const FileWriteManager&) = delete;
     FileWriteManager& operator=(const FileWriteManager&) = delete;

     std::mutex map_mutex_;
     std::unordered_map<int32_t, std::shared_ptr<std::mutex>> device_locks_;
 };

}