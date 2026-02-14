#include "agent/core/types.hpp"

namespace agent {

std::string to_string(FinishReason reason) {
    switch (reason) {
        case FinishReason::Stop: return "stop";
        case FinishReason::ToolCalls: return "tool_calls";
        case FinishReason::Length: return "length";
        case FinishReason::Error: return "error";
        case FinishReason::Cancelled: return "cancelled";
    }
    return "unknown";
}

FinishReason finish_reason_from_string(const std::string& str) {
    if (str == "stop" || str == "end_turn") return FinishReason::Stop;
    if (str == "tool_calls" || str == "tool_use") return FinishReason::ToolCalls;
    if (str == "length" || str == "max_tokens") return FinishReason::Length;
    if (str == "error") return FinishReason::Error;
    if (str == "cancelled") return FinishReason::Cancelled;
    return FinishReason::Stop;
}

std::string to_string(AgentType type) {
    switch (type) {
        case AgentType::Build: return "build";
        case AgentType::Explore: return "explore";
        case AgentType::General: return "general";
        case AgentType::Plan: return "plan";
        case AgentType::Compaction: return "compaction";
    }
    return "build";
}

AgentType agent_type_from_string(const std::string& str) {
    if (str == "build") return AgentType::Build;
    if (str == "explore") return AgentType::Explore;
    if (str == "general") return AgentType::General;
    if (str == "plan") return AgentType::Plan;
    if (str == "compaction") return AgentType::Compaction;
    return AgentType::Build;
}

}  // namespace agent
