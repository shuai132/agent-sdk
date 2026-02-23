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
 * @param log_path 日志文件路径（可选，默认 ~/.config/agent-sdk/agent_sdk.log）
 * @param max_size 单个日志文件最大大小（字节），默认 10MB
 * @param max_files 保留的日志文件数量，默认 3 个
 * @param level 日志级别，默认 debug
 */
void init_log(const std::string& log_path = "", size_t max_size = 10 * 1024 * 1024, size_t max_files = 3, const std::string& level = "debug");

/**
 * 获取默认 logger
 */
std::shared_ptr<spdlog::logger> get_logger();

}  // namespace agent

#endif  // AGENT_SDK_LOG_H
