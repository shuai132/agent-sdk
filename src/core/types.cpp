#include "core/types.hpp"

namespace agent {

std::string to_string(FinishReason reason) {
  switch (reason) {
    case FinishReason::Stop:
      return "stop";
    case FinishReason::ToolCalls:
      return "tool_calls";
    case FinishReason::Length:
      return "length";
    case FinishReason::Error:
      return "error";
    case FinishReason::Cancelled:
      return "cancelled";
  }
  return "unknown";
}

FinishReason finish_reason_from_string(const std::string &str) {
  if (str == "stop" || str == "end_turn") return FinishReason::Stop;
  if (str == "tool_calls" || str == "tool_use") return FinishReason::ToolCalls;
  if (str == "length" || str == "max_tokens") return FinishReason::Length;
  if (str == "error") return FinishReason::Error;
  if (str == "cancelled") return FinishReason::Cancelled;
  return FinishReason::Stop;
}

std::string to_string(AgentType type) {
  switch (type) {
    case AgentType::Build:
      return "build";
    case AgentType::Explore:
      return "explore";
    case AgentType::General:
      return "general";
    case AgentType::Plan:
      return "plan";
    case AgentType::Compaction:
      return "compaction";
  }
  return "build";
}

AgentType agent_type_from_string(const std::string &str) {
  if (str == "build") return AgentType::Build;
  if (str == "explore") return AgentType::Explore;
  if (str == "general") return AgentType::General;
  if (str == "plan") return AgentType::Plan;
  if (str == "compaction") return AgentType::Compaction;
  return AgentType::Build;
}

std::string sanitize_utf8(const std::string &input) {
  std::string output;
  output.reserve(input.size());

  size_t i = 0;
  while (i < input.size()) {
    unsigned char c = static_cast<unsigned char>(input[i]);

    if (c <= 0x7F) {
      // ASCII byte
      output.push_back(static_cast<char>(c));
      i++;
    } else if ((c & 0xE0) == 0xC0) {
      // 2-byte sequence
      if (i + 1 < input.size() && (static_cast<unsigned char>(input[i + 1]) & 0xC0) == 0x80) {
        uint32_t cp = ((c & 0x1F) << 6) | (static_cast<unsigned char>(input[i + 1]) & 0x3F);
        if (cp >= 0x80) {
          output.push_back(input[i]);
          output.push_back(input[i + 1]);
        } else {
          output.append("\xEF\xBF\xBD");
        }
        i += 2;
      } else {
        output.append("\xEF\xBF\xBD");
        i++;
      }
    } else if ((c & 0xF0) == 0xE0) {
      // 3-byte sequence
      if (i + 2 < input.size() && (static_cast<unsigned char>(input[i + 1]) & 0xC0) == 0x80 &&
          (static_cast<unsigned char>(input[i + 2]) & 0xC0) == 0x80) {
        uint32_t cp =
            ((c & 0x0F) << 12) | ((static_cast<unsigned char>(input[i + 1]) & 0x3F) << 6) | (static_cast<unsigned char>(input[i + 2]) & 0x3F);
        if (cp >= 0x800 && (cp < 0xD800 || cp > 0xDFFF)) {
          output.push_back(input[i]);
          output.push_back(input[i + 1]);
          output.push_back(input[i + 2]);
        } else {
          output.append("\xEF\xBF\xBD");
        }
        i += 3;
      } else {
        output.append("\xEF\xBF\xBD");
        i++;
      }
    } else if ((c & 0xF8) == 0xF0) {
      // 4-byte sequence
      if (i + 3 < input.size() && (static_cast<unsigned char>(input[i + 1]) & 0xC0) == 0x80 &&
          (static_cast<unsigned char>(input[i + 2]) & 0xC0) == 0x80 && (static_cast<unsigned char>(input[i + 3]) & 0xC0) == 0x80) {
        uint32_t cp = ((c & 0x07) << 18) | ((static_cast<unsigned char>(input[i + 1]) & 0x3F) << 12) |
                      ((static_cast<unsigned char>(input[i + 2]) & 0x3F) << 6) | (static_cast<unsigned char>(input[i + 3]) & 0x3F);
        if (cp >= 0x10000 && cp <= 0x10FFFF) {
          output.push_back(input[i]);
          output.push_back(input[i + 1]);
          output.push_back(input[i + 2]);
          output.push_back(input[i + 3]);
        } else {
          output.append("\xEF\xBF\xBD");
        }
        i += 4;
      } else {
        output.append("\xEF\xBF\xBD");
        i++;
      }
    } else {
      // Invalid leading byte
      output.append("\xEF\xBF\xBD");
      i++;
    }
  }

  return output;
}

}  // namespace agent
