#ifndef HARDWARE_TEST_KEY_NATIVE_H
#define HARDWARE_TEST_KEY_NATIVE_H

#include <deque>
#include <mutex>
#include <string>

#include "ehal_key.h"

namespace hardware_test {

/*
 * 按键测试模块统一管理 ehal_key 生命周期、按键状态和最近事件。
 * ehal_key 的回调线程只负责写入内存，网页线程通过 status_json 读取快照，
 * 因此不会在 SDK 回调中执行文件或网络操作。
 */
class NativeKey {
public:
    explicit NativeKey(std::string device_name);
    ~NativeKey();

    NativeKey(const NativeKey &) = delete;
    NativeKey &operator=(const NativeKey &) = delete;

    /* 启动按键输入设备监听；失败时返回 SDK 的明确错误文本。 */
    bool start(std::string *error_message);

    /* 停止监听并释放 SDK 对象。 */
    void stop();

    /* 返回四个按键的状态计数以及最近事件，供网页直接显示。 */
    std::string status_json() const;

private:
    static void key_callback(const ehal_key_event_t *event, void *user_data);
    void on_key(const ehal_key_event_t *event);
    static const char *key_name(ehal_key_code_t key);
    static const char *event_name(ehal_key_event_type_t type);
    static std::string json_escape(const std::string &value);

    std::string device_name_;
    ehal_key_t *key_ = nullptr;
    mutable std::mutex mutex_;
    std::deque<ehal_key_event_t> events_;
    bool started_ = false;
};

} // namespace hardware_test

#endif
