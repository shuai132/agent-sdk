#include <iostream>
#include <sstream>

#include "core/config.hpp"
#include "session/session.hpp"

using namespace agent;

int main() {
  Config config = Config::load_default();
  
  // 设置特定的工作目录进行测试
  config.working_dir = "/tmp/test_project";
  
  asio::io_context io_ctx;
  
  try {
    auto session = Session::create(io_ctx, config, AgentType::Build);
    
    // 获取 system prompt
    auto agent_config = session->agent_config();
    
    std::cout << "=== System Prompt ===" << std::endl;
    std::cout << agent_config.system_prompt << std::endl;
    
    // 检查工作目录是否被注入
    bool has_working_dir = agent_config.system_prompt.find("当前工作目录：/tmp/test_project") != std::string::npos;
    bool has_note = agent_config.system_prompt.find("默认相对于此工作目录进行") != std::string::npos;
    
    std::cout << "\n=== 测试结果 ===" << std::endl;
    std::cout << "工作目录信息已注入: " << (has_working_dir ? "✅ 是" : "❌ 否") << std::endl;
    std::cout << "说明信息已注入: " << (has_note ? "✅ 是" : "❌ 否") << std::endl;
    
    return (has_working_dir && has_note) ? 0 : 1;
    
  } catch (const std::exception& e) {
    std::cerr << "错误: " << e.what() << std::endl;
    return 1;
  }
}