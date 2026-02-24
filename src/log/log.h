#ifndef AGENT_SDK_LOG_H
#define AGENT_SDK_LOG_H

#include <memory>
#include <string>

namespace spdlog {
class logger;
}

namespace agent {

/**
 * 初始化日志系统
 *
 * 日志轮转策略（按启动次数轮转）：
 * - 每次启动应用时，当前的 agent_sdk.log 会被清空
 * - 上次的日志重命名为 agent_sdk.0.log
 * - 历史日志依次向后移动：agent_sdk.0.log -> agent_sdk.1.log -> ... -> agent_sdk.9.log
 * - 最旧的日志（agent_sdk.9.log）被删除
 *
 * @param log_path 日志文件路径（可选，默认 ~/.config/agent_sdk/log/agent_sdk.log）
 * @param max_size 单个日志文件最大大小（字节），当前未使用，保留参数
 * @param max_files 保留的历史日志文件数量，默认 10 个（agent_sdk.0.log ~ agent_sdk.9.log）
 * @param level 日志级别，默认 debug
 */
void init_log(const std::string& log_path = "", size_t max_size = 10 * 1024 * 1024, size_t max_files = 10, const std::string& level = "debug");

/**
 * 获取默认 logger
 */
std::shared_ptr<spdlog::logger> get_logger();

}  // namespace agent

#endif  // AGENT_SDK_LOG_H
