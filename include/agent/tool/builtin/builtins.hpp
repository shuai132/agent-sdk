#pragma once

#include "agent/tool/tool.hpp"

namespace agent::tools {

// Bash tool - execute shell commands
class BashTool : public SimpleTool {
public:
    BashTool();
    
    std::vector<ParameterSchema> parameters() const override;
    std::future<ToolResult> execute(const json& args, const ToolContext& ctx) override;
    
private:
    static constexpr int DEFAULT_TIMEOUT_MS = 120000;
};

// Read tool - read file contents
class ReadTool : public SimpleTool {
public:
    ReadTool();
    
    std::vector<ParameterSchema> parameters() const override;
    std::future<ToolResult> execute(const json& args, const ToolContext& ctx) override;
};

// Write tool - write file contents
class WriteTool : public SimpleTool {
public:
    WriteTool();
    
    std::vector<ParameterSchema> parameters() const override;
    std::future<ToolResult> execute(const json& args, const ToolContext& ctx) override;
};

// Edit tool - edit file with search/replace
class EditTool : public SimpleTool {
public:
    EditTool();
    
    std::vector<ParameterSchema> parameters() const override;
    std::future<ToolResult> execute(const json& args, const ToolContext& ctx) override;
};

// Glob tool - find files by pattern
class GlobTool : public SimpleTool {
public:
    GlobTool();
    
    std::vector<ParameterSchema> parameters() const override;
    std::future<ToolResult> execute(const json& args, const ToolContext& ctx) override;
};

// Grep tool - search file contents
class GrepTool : public SimpleTool {
public:
    GrepTool();
    
    std::vector<ParameterSchema> parameters() const override;
    std::future<ToolResult> execute(const json& args, const ToolContext& ctx) override;
};

// Question tool - ask user a question
class QuestionTool : public SimpleTool {
public:
    QuestionTool();
    
    std::vector<ParameterSchema> parameters() const override;
    std::future<ToolResult> execute(const json& args, const ToolContext& ctx) override;
};

// Task tool - launch subagent
class TaskTool : public SimpleTool {
public:
    TaskTool();
    
    std::vector<ParameterSchema> parameters() const override;
    std::future<ToolResult> execute(const json& args, const ToolContext& ctx) override;
};

// Register all builtin tools
void register_builtins();

}  // namespace agent::tools
