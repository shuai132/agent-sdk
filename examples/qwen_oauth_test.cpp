#include "plugin/qwen/qwen_oauth.hpp"

#include <spdlog/spdlog.h>

#include <iostream>

#include "agent/agent.hpp"
#include "plugin/qrcode.hpp"

using namespace agent;
using namespace agent::plugin::qwen;
using agent::plugin::QrCode;

// 简单的终端输出回调
void print_auth_status(const std::string& message) {
  std::cout << "[Auth Status] " << message << std::endl;
}

void print_user_code(const std::string& uri, const std::string& code, const std::string& uri_complete) {
  std::string auth_url = uri_complete.empty() ? uri : uri_complete;

  std::cout << "\n";
  std::cout << "╭──────────────────────────────────────────────────────────────────╮\n";
  std::cout << "│                                                                  │\n";
  std::cout << "│  Qwen OAuth 认证                                                 │\n";
  std::cout << "│                                                                  │\n";
  std::cout << "│  请扫描二维码或访问以下 URL 进行授权：                           │\n";
  std::cout << "│                                                                  │\n";
  std::cout << "╰──────────────────────────────────────────────────────────────────╯\n";
  std::cout << "\n";

  // 生成并显示 QR 码
  std::string qr_str = QrCode::encode(auth_url);
  std::cout << qr_str << std::endl;

  std::cout << "╭──────────────────────────────────────────────────────────────────╮\n";
  std::cout << "│  授权链接: " << auth_url << "\n";
  std::cout << "│  验证码: " << code << "\n";
  std::cout << "│                                                                  │\n";
  std::cout << "│  等待授权中...                                                   │\n";
  std::cout << "╰──────────────────────────────────────────────────────────────────╯\n";
  std::cout << std::endl;
}

int main() {
  // 启用 debug 日志
  spdlog::set_level(spdlog::level::debug);

  std::cout << "=== Qwen OAuth API Test ===\n" << std::endl;

  // 注册 Qwen 插件
  register_qwen_plugin();

  // 1. 检查 OAuth Token
  auto& auth = qwen_portal_auth();
  auto token = auth.load_token();

  // 2. 如果没有 token，自动触发认证流程
  if (!token) {
    std::cout << "No Qwen OAuth token found. Starting authentication..." << std::endl;

    // 设置回调
    auth.set_status_callback(print_auth_status);
    auth.set_user_code_callback(print_user_code);

    // 开始认证流程
    auto future = auth.authenticate();

    std::cout << "Waiting for authentication to complete..." << std::endl;
    std::cout << "(Please complete the authorization in your browser)" << std::endl;

    // 等待认证完成
    token = future.get();

    if (!token) {
      std::cerr << "Authentication failed." << std::endl;
      return 1;
    }

    std::cout << "\nAuthentication successful!" << std::endl;
  }

  std::cout << "Token loaded successfully:" << std::endl;
  std::cout << "  Provider: " << token->provider << std::endl;
  std::cout << "  Access Token: " << token->access_token.substr(0, 20) << "..." << std::endl;
  std::cout << "  Is Expired: " << (token->is_expired() ? "Yes" : "No") << std::endl;
  std::cout << "  Needs Refresh: " << (token->needs_refresh() ? "Yes" : "No") << std::endl;
  std::cout << std::endl;

  // 3. 设置配置
  Config config;
  config.providers["openai"] = ProviderConfig{
      "openai",
      "qwen-oauth",              // 使用 OAuth 占位符
      "https://portal.qwen.ai",  // Qwen Portal API base URL
      std::nullopt,
      {},
  };
  config.default_model = "coder-model";  // portal.qwen.ai 使用的模型名称

  // 4. 初始化
  asio::io_context io_ctx;
  agent::init();

  // 5. 创建 Session
  auto session = Session::create(io_ctx, config, AgentType::Build);

  // 6. 设置回调
  std::string response_text;
  bool completed = false;
  bool has_error = false;
  std::string error_msg;

  session->on_stream([&response_text](const std::string& text) {
    std::cout << text << std::flush;
    response_text += text;
  });

  session->on_error([&](const std::string& error) {
    has_error = true;
    error_msg = error;
    std::cerr << "\n[Error] " << error << std::endl;
  });

  session->on_complete([&](FinishReason reason) {
    completed = true;
    std::cout << "\n\n[Complete] Finish reason: ";
    switch (reason) {
      case FinishReason::Stop:
        std::cout << "Stop";
        break;
      case FinishReason::ToolCalls:
        std::cout << "ToolCalls";
        break;
      case FinishReason::Length:
        std::cout << "Length";
        break;
      case FinishReason::Error:
        std::cout << "Error";
        break;
      case FinishReason::Cancelled:
        std::cout << "Cancelled";
        break;
    }
    std::cout << std::endl;
  });

  // 7. 运行 IO context (需要在发送消息之前启动)
  std::thread io_thread([&io_ctx]() {
    auto work = asio::make_work_guard(io_ctx);
    io_ctx.run();
  });

  // 8. 发送测试消息
  std::cout << "Sending test prompt to Qwen API..." << std::endl;
  std::cout << "Model: " << config.default_model << std::endl;
  std::cout << "Base URL: https://portal.qwen.ai" << std::endl;
  std::cout << "\n--- Response ---\n" << std::endl;

  session->prompt("Say 'Hello from Qwen!' in exactly 5 words.");

  // 等待完成（最多 30 秒）
  auto start = std::chrono::steady_clock::now();
  while (!completed && !has_error) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    auto elapsed = std::chrono::steady_clock::now() - start;
    if (elapsed > std::chrono::seconds(30)) {
      std::cerr << "\n[Timeout] No response received within 30 seconds." << std::endl;
      break;
    }
  }

  // 9. 清理
  session->cancel();
  io_ctx.stop();
  if (io_thread.joinable()) io_thread.join();

  std::cout << "\n=== Test Complete ===" << std::endl;

  if (has_error) {
    return 1;
  }

  if (response_text.empty()) {
    std::cerr << "Warning: No response text received." << std::endl;
    return 1;
  }

  return 0;
}
