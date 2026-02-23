#include "log/log.h"

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/spdlog.h>

#include <iostream>

#include "core/config.hpp"

namespace agent {

void init_log(const std::string& log_path, size_t max_size, size_t max_files, const std::string& level) {
  try {
    // 确定日志路径
    std::string actual_path = log_path;
    if (actual_path.empty()) {
      actual_path = (config_paths::config_dir() / "agent_sdk.log").string();
    }

    // 创建 rotating file sink
    auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(actual_path, max_size, max_files);

    // 创建 logger
    auto logger = std::make_shared<spdlog::logger>("agent_sdk", file_sink);

    // 设置日志级别
    spdlog::level::level_enum log_level = spdlog::level::info;
    if (level == "trace")
      log_level = spdlog::level::trace;
    else if (level == "debug")
      log_level = spdlog::level::debug;
    else if (level == "info")
      log_level = spdlog::level::info;
    else if (level == "warn")
      log_level = spdlog::level::warn;
    else if (level == "err")
      log_level = spdlog::level::err;
    else if (level == "critical")
      log_level = spdlog::level::critical;
    else if (level == "off")
      log_level = spdlog::level::off;

    logger->set_level(log_level);

    // 设置日志格式：[时间] [级别] [线程 ID] 消息
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%t] %v");

    // 注册并设为默认 logger
    spdlog::register_logger(logger);
    spdlog::set_default_logger(logger);

    spdlog::info("=== agent_cli started (log: {}) ===", actual_path);
  } catch (const spdlog::spdlog_ex& ex) {
    std::cerr << "Failed to init logger: " << ex.what() << "\n";
  }
}

std::shared_ptr<spdlog::logger> get_logger() {
  return spdlog::default_logger();
}

}  // namespace agent
