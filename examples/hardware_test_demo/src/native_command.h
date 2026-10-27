#ifndef HARDWARE_TEST_NATIVE_COMMAND_H
#define HARDWARE_TEST_NATIVE_COMMAND_H

#include <cstddef>
#include <string>
#include <vector>

namespace hardware_test {

/*
 * 原生系统命令的执行结果。
 *
 * 该结构只描述命令执行事实，不承载任何硬件封装逻辑。调用方可以据此
 * 把退出码、超时和原始输出返回到网页，保证板端问题具备可追溯依据。
 */
struct CommandResult {
    int exit_code = -1;
    bool timed_out = false;
    bool started = false;
    std::string output;
};

/*
 * 使用 fork/exec 执行固定的原生命令。
 *
 * 参数以数组形式传给 execvp，不经过 Shell，因此 SSID、网络 ID 和接口
 * 名称不会被解释为命令或重定向语法。标准输出和标准错误合并捕获。
 */
CommandResult run_native_command(const std::vector<std::string> &arguments,
                                 int timeout_ms,
                                 size_t max_output_bytes = 65536U);

} // namespace hardware_test

#endif
