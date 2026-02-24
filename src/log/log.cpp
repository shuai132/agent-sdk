#include "log/log.h"

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <iostream>

#include "core/config.hpp"

namespace agent {

namespace {

// 每次启动时轮转日志文件
// 策略：agent_sdk.log -> agent_sdk.0.log -> ... -> agent_sdk.9.log（最旧的被删除）
void rotate_logs_on_startup(const std::filesystem::path& log_dir, size_t max_files) {
  namespace fs = std::filesystem;

  // 确保日志目录存在
  std::error_code ec;
  fs::create_directories(log_dir, ec);
  if (ec) {
    std::cerr << "Failed to create log directory: " << ec.message() << "\n";
    return;
  }

  // 当前日志文件路径
  fs::path current_log = log_dir / "agent_sdk.log";

  // 如果当前日志文件不存在，无需轮转
  if (!fs::exists(current_log)) {
    return;
  }

  // 删除最旧的日志文件 (agent_sdk.{max_files-1}.log)
  fs::path oldest = log_dir / ("agent_sdk." + std::to_string(max_files - 1) + ".log");
  if (fs::exists(oldest)) {
    fs::remove(oldest, ec);
  }

  // 从后往前依次重命名：agent_sdk.8.log -> agent_sdk.9.log, ...
  for (int i = static_cast<int>(max_files) - 2; i >= 0; --i) {
    fs::path old_name = log_dir / ("agent_sdk." + std::to_string(i) + ".log");
    fs::path new_name = log_dir / ("agent_sdk." + std::to_string(i + 1) + ".log");
    if (fs::exists(old_name)) {
      fs::rename(old_name, new_name, ec);
    }
  }

  // 把当前日志文件重命名为 agent_sdk.0.log
  fs::path first_backup = log_dir / "agent_sdk.0.log";
  fs::rename(current_log, first_backup, ec);
}

}  // namespace

void init_log(const std::string& log_path, size_t /* max_size */, size_t max_files, const std::string& level) {
  try {
    namespace fs = std::filesystem;

    // 确定日志目录和文件路径
    fs::path log_dir;
    fs::path actual_path;

    if (log_path.empty()) {
      log_dir = config_paths::config_dir() / "log";
      actual_path = log_dir / "agent_sdk.log";
    } else {
      actual_path = log_path;
      log_dir = actual_path.parent_path();
    }

    // 确保日志目录存在
    std::error_code ec;
    fs::create_directories(log_dir, ec);

    // 每次启动时轮转日志
    rotate_logs_on_startup(log_dir, max_files);

    // 创建 basic file sink（每次启动都是新的干净文件）
    auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(actual_path.string(), true);

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

    // 每条日志都立即刷新，避免缓存导致日志不及时
    logger->flush_on(spdlog::level::trace);

    // 注册并设为默认 logger
    spdlog::register_logger(logger);
    spdlog::set_default_logger(logger);

    spdlog::info("=== agent_cli started (log: {}) ===", actual_path.string());
  } catch (const spdlog::spdlog_ex& ex) {
    std::cerr << "Failed to init logger: " << ex.what() << "\n";
  }
}

std::shared_ptr<spdlog::logger> get_logger() {
  return spdlog::default_logger();
}

}  // namespace agent
