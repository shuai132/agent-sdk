#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <regex>
#include <sstream>

#include "session/session.hpp"
#include "tool/builtin/builtins.hpp"

#ifdef _WIN32
#include <windows.h>
#else

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#endif

namespace agent::tools {

namespace fs = std::filesystem;

// ============================================================================
// BashTool
// ============================================================================

BashTool::BashTool() : SimpleTool("bash", "Executes a given bash command in a persistent shell session with optional timeout.") {}

std::vector<ParameterSchema> BashTool::parameters() const {
  return {{"command", "string", "The command to execute", true, std::nullopt, std::nullopt},
          {"description", "string", "Clear, concise description of what this command does", false, std::nullopt, std::nullopt},
          {"timeout", "number", "Optional timeout in milliseconds", false, json(DEFAULT_TIMEOUT_MS), std::nullopt},
          {"workdir", "string", "The working directory to run the command in", false, std::nullopt, std::nullopt}};
}

std::future<ToolResult> BashTool::execute(const json &args, const ToolContext &ctx) {
  return std::async(std::launch::async, [args, ctx]() -> ToolResult {
    std::string command = args.value("command", "");
    std::string workdir = args.value("workdir", ctx.working_dir);
    int timeout_ms = args.value("timeout", DEFAULT_TIMEOUT_MS);

    if (command.empty()) {
      return ToolResult::error("Command is required");
    }

    // Check for abort
    if (ctx.abort_signal && ctx.abort_signal->load()) {
      return ToolResult::error("Cancelled");
    }

    std::string output;
    int exit_code = 0;

#ifndef _WIN32
    // Change to working directory
    std::string full_command;
    if (!workdir.empty() && workdir != ".") {
      full_command = "cd " + workdir + " && " + command;
    } else {
      full_command = command;
    }

    // Execute using popen
    std::array<char, 4096> buffer;
    std::string result;

    FILE *pipe = popen((full_command + " 2>&1").c_str(), "r");
    if (!pipe) {
      return ToolResult::error("Failed to execute command");
    }

    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
      result += buffer.data();

      // Check for abort
      if (ctx.abort_signal && ctx.abort_signal->load()) {
        pclose(pipe);
        return ToolResult::error("Cancelled");
      }
    }

    exit_code = pclose(pipe);
    exit_code = WEXITSTATUS(exit_code);
    output = result;
#else
            // Windows implementation would go here
            output = "Windows shell execution not implemented";
            exit_code = 1;
#endif

    // Truncate if needed
    auto truncated = Truncate::save_and_truncate(output, "bash");

    if (exit_code != 0) {
      return ToolResult{truncated.content + "\n[Exit code: " + std::to_string(exit_code) + "]", "Command failed", {{"exit_code", exit_code}}, true};
    }

    return ToolResult::with_title(truncated.content, "Executed: " + command.substr(0, 50));
  });
}

// ============================================================================
// ReadTool
// ============================================================================

ReadTool::ReadTool() : SimpleTool("read", "Reads a file from the local filesystem. Returns the file content with line numbers.") {}

std::vector<ParameterSchema> ReadTool::parameters() const {
  return {{"filePath", "string", "The absolute path to the file to read", true, std::nullopt, std::nullopt},
          {"offset", "number", "The line number to start reading from (0-based)", false, json(0), std::nullopt},
          {"limit", "number", "The number of lines to read (defaults to 2000)", false, json(2000), std::nullopt}};
}

std::future<ToolResult> ReadTool::execute(const json &args, const ToolContext &ctx) {
  return std::async(std::launch::async, [args, ctx]() -> ToolResult {
    std::string file_path = args.value("filePath", "");
    int offset = args.value("offset", 0);
    int limit = args.value("limit", 2000);

    if (file_path.empty()) {
      return ToolResult::error("filePath is required");
    }

    // Resolve relative paths
    fs::path path = file_path;
    if (!path.is_absolute()) {
      path = fs::path(ctx.working_dir) / path;
    }

    if (!fs::exists(path)) {
      return ToolResult::error("File not found: " + path.string());
    }

    if (fs::is_directory(path)) {
      return ToolResult::error("Path is a directory, not a file: " + path.string());
    }

    std::ifstream file(path);
    if (!file.is_open()) {
      return ToolResult::error("Failed to open file: " + path.string());
    }

    std::ostringstream output;
    std::string line;
    int line_num = 0;
    int lines_read = 0;

    while (std::getline(file, line)) {
      line_num++;

      if (line_num <= offset) continue;
      if (lines_read >= limit) break;

      // Format with line numbers (similar to cat -n)
      output << std::setw(5) << line_num << "\t" << line << "\n";
      lines_read++;
    }

    std::string content = output.str();

    // Check if file was truncated
    bool has_more = false;
    if (std::getline(file, line)) {
      has_more = true;
    }

    if (has_more) {
      content += "\n(File has more lines. Use 'offset' parameter to read beyond line " + std::to_string(offset + limit) + ")";
    }

    return ToolResult::with_title(content, path.filename().string());
  });
}

// ============================================================================
// WriteTool
// ============================================================================

WriteTool::WriteTool() : SimpleTool("write", "Writes content to a file. Creates the file if it doesn't exist, overwrites if it does.") {}

std::vector<ParameterSchema> WriteTool::parameters() const {
  return {{"filePath", "string", "The absolute path to the file to write", true, std::nullopt, std::nullopt},
          {"content", "string", "The content to write to the file", true, std::nullopt, std::nullopt}};
}

std::future<ToolResult> WriteTool::execute(const json &args, const ToolContext &ctx) {
  return std::async(std::launch::async, [args, ctx]() -> ToolResult {
    std::string file_path = args.value("filePath", "");
    std::string content = args.value("content", "");

    if (file_path.empty()) {
      return ToolResult::error("filePath is required");
    }

    fs::path path = file_path;
    if (!path.is_absolute()) {
      path = fs::path(ctx.working_dir) / path;
    }

    // Create parent directories if needed
    auto parent = path.parent_path();
    if (!parent.empty() && !fs::exists(parent)) {
      fs::create_directories(parent);
    }

    std::ofstream file(path);
    if (!file.is_open()) {
      return ToolResult::error("Failed to open file for writing: " + path.string());
    }

    file << content;
    file.close();

    return ToolResult::with_title("Successfully wrote " + std::to_string(content.size()) + " bytes to " + path.string(),
                                  "Wrote " + path.filename().string());
  });
}

// ============================================================================
// EditTool
// ============================================================================

EditTool::EditTool() : SimpleTool("edit", "Performs exact string replacements in files using search and replace.") {}

std::vector<ParameterSchema> EditTool::parameters() const {
  return {{"filePath", "string", "The absolute path to the file to modify", true, std::nullopt, std::nullopt},
          {"oldString", "string", "The text to replace", true, std::nullopt, std::nullopt},
          {"newString", "string", "The text to replace it with", true, std::nullopt, std::nullopt},
          {"replaceAll", "boolean", "Replace all occurrences (default false)", false, json(false), std::nullopt}};
}

std::future<ToolResult> EditTool::execute(const json &args, const ToolContext &ctx) {
  return std::async(std::launch::async, [args, ctx]() -> ToolResult {
    std::string file_path = args.value("filePath", "");
    std::string old_str = args.value("oldString", "");
    std::string new_str = args.value("newString", "");
    bool replace_all = args.value("replaceAll", false);

    if (file_path.empty()) {
      return ToolResult::error("filePath is required");
    }
    if (old_str.empty()) {
      return ToolResult::error("oldString is required");
    }

    fs::path path = file_path;
    if (!path.is_absolute()) {
      path = fs::path(ctx.working_dir) / path;
    }

    if (!fs::exists(path)) {
      return ToolResult::error("File not found: " + path.string());
    }

    // Read file content
    std::ifstream file(path);
    if (!file.is_open()) {
      return ToolResult::error("Failed to open file: " + path.string());
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();
    file.close();

    // Find occurrences
    size_t count = 0;
    size_t pos = 0;
    while ((pos = content.find(old_str, pos)) != std::string::npos) {
      count++;
      pos += old_str.length();
    }

    if (count == 0) {
      return ToolResult::error("oldString not found in content");
    }

    if (count > 1 && !replace_all) {
      return ToolResult::error("oldString found " + std::to_string(count) + " times. " +
                               "Use replaceAll=true to replace all occurrences, or provide more context to make it unique.");
    }

    // Perform replacement
    std::string new_content;
    pos = 0;
    size_t replaced = 0;

    while (true) {
      size_t found = content.find(old_str, pos);
      if (found == std::string::npos) {
        new_content += content.substr(pos);
        break;
      }

      new_content += content.substr(pos, found - pos);
      new_content += new_str;
      pos = found + old_str.length();
      replaced++;

      if (!replace_all) break;
    }

    if (!replace_all && pos < content.length()) {
      new_content += content.substr(pos);
    }

    // Write back
    std::ofstream out_file(path);
    if (!out_file.is_open()) {
      return ToolResult::error("Failed to write file: " + path.string());
    }

    out_file << new_content;
    out_file.close();

    return ToolResult::with_title("Replaced " + std::to_string(replaced) + " occurrence(s) in " + path.string(),
                                  "Edited " + path.filename().string());
  });
}

// ============================================================================
// GlobTool
// ============================================================================

GlobTool::GlobTool() : SimpleTool("glob", "Fast file pattern matching tool. Supports glob patterns like \"**/*.js\".") {}

std::vector<ParameterSchema> GlobTool::parameters() const {
  return {{"pattern", "string", "The glob pattern to match files against", true, std::nullopt, std::nullopt},
          {"path", "string", "The directory to search in", false, std::nullopt, std::nullopt}};
}

std::future<ToolResult> GlobTool::execute(const json &args, const ToolContext &ctx) {
  return std::async(std::launch::async, [args, ctx]() -> ToolResult {
    std::string pattern = args.value("pattern", "");
    std::string search_path = args.value("path", ctx.working_dir);

    if (pattern.empty()) {
      return ToolResult::error("pattern is required");
    }

    fs::path base_path = search_path;
    if (!base_path.is_absolute()) {
      base_path = fs::path(ctx.working_dir) / base_path;
    }

    if (!fs::exists(base_path)) {
      return ToolResult::error("Path not found: " + base_path.string());
    }

    // Simple glob implementation (basic patterns only)
    // A full implementation would use a proper glob library
    std::vector<std::string> matches;

    try {
      for (const auto &entry : fs::recursive_directory_iterator(base_path)) {
        if (!entry.is_regular_file()) continue;

        std::string filename = entry.path().filename().string();
        std::string rel_path = fs::relative(entry.path(), base_path).string();

        // Simple pattern matching
        bool match = false;

        if (pattern.find("**") != std::string::npos) {
          // Recursive pattern - match filename extension
          std::string ext_pattern = pattern.substr(pattern.rfind("*.") + 1);
          if (filename.size() >= ext_pattern.size() && filename.compare(filename.size() - ext_pattern.size(), ext_pattern.size(), ext_pattern) == 0) {
            match = true;
          }
        } else if (pattern.find('*') != std::string::npos) {
          // Simple wildcard
          size_t star_pos = pattern.find('*');
          std::string prefix = pattern.substr(0, star_pos);
          std::string suffix = pattern.substr(star_pos + 1);

          if ((prefix.empty() || filename.find(prefix) == 0) && (suffix.empty() || filename.rfind(suffix) == filename.size() - suffix.size())) {
            match = true;
          }
        } else {
          // Exact match
          match = (filename == pattern || rel_path == pattern);
        }

        if (match) {
          matches.push_back(rel_path);
        }
      }
    } catch (const std::exception &e) {
      return ToolResult::error(std::string("Error searching: ") + e.what());
    }

    if (matches.empty()) {
      return ToolResult::success("No files found matching pattern: " + pattern);
    }

    std::ostringstream output;
    for (const auto &match : matches) {
      output << match << "\n";
    }

    return ToolResult::with_title(output.str(), "Found " + std::to_string(matches.size()) + " files");
  });
}

// ============================================================================
// GrepTool
// ============================================================================

GrepTool::GrepTool() : SimpleTool("grep", "Fast content search tool. Searches file contents using regular expressions.") {}

std::vector<ParameterSchema> GrepTool::parameters() const {
  return {{"pattern", "string", "The regex pattern to search for", true, std::nullopt, std::nullopt},
          {"path", "string", "The directory to search in", false, std::nullopt, std::nullopt},
          {"include", "string", "File pattern to include (e.g. \"*.js\")", false, std::nullopt, std::nullopt}};
}

std::future<ToolResult> GrepTool::execute(const json &args, const ToolContext &ctx) {
  return std::async(std::launch::async, [args, ctx]() -> ToolResult {
    std::string pattern = args.value("pattern", "");
    std::string search_path = args.value("path", ctx.working_dir);
    std::string include = args.value("include", "");

    if (pattern.empty()) {
      return ToolResult::error("pattern is required");
    }

    fs::path base_path = search_path;
    if (!base_path.is_absolute()) {
      base_path = fs::path(ctx.working_dir) / base_path;
    }

    std::regex search_regex;
    try {
      search_regex = std::regex(pattern);
    } catch (const std::regex_error &e) {
      return ToolResult::error("Invalid regex pattern: " + std::string(e.what()));
    }

    std::ostringstream output;
    size_t match_count = 0;
    const size_t max_matches = 100;

    try {
      for (const auto &entry : fs::recursive_directory_iterator(base_path)) {
        if (!entry.is_regular_file()) continue;
        if (match_count >= max_matches) break;

        std::string filename = entry.path().filename().string();

        // Check include pattern
        if (!include.empty()) {
          bool matches_include = false;
          if (include.find('*') != std::string::npos) {
            std::string ext = include.substr(include.rfind('.'));
            if (filename.size() >= ext.size() && filename.compare(filename.size() - ext.size(), ext.size(), ext) == 0) {
              matches_include = true;
            }
          } else {
            matches_include = (filename == include);
          }
          if (!matches_include) continue;
        }

        std::ifstream file(entry.path());
        if (!file.is_open()) continue;

        std::string line;
        int line_num = 0;
        std::string rel_path = fs::relative(entry.path(), base_path).string();

        while (std::getline(file, line) && match_count < max_matches) {
          line_num++;
          if (std::regex_search(line, search_regex)) {
            output << rel_path << ":" << line_num << ": " << line << "\n";
            match_count++;
          }
        }
      }
    } catch (const std::exception &e) {
      return ToolResult::error(std::string("Error searching: ") + e.what());
    }

    if (match_count == 0) {
      return ToolResult::success("No matches found for pattern: " + pattern);
    }

    std::string result = output.str();
    if (match_count >= max_matches) {
      result += "\n... (results truncated, showing first " + std::to_string(max_matches) + " matches)";
    }

    return ToolResult::with_title(result, std::to_string(match_count) + " matches");
  });
}

// ============================================================================
// QuestionTool
// ============================================================================

QuestionTool::QuestionTool() : SimpleTool("question", "Ask the user a question to gather information or clarify requirements.") {}

std::vector<ParameterSchema> QuestionTool::parameters() const {
  return {{"questions", "array", "Array of questions to ask", true, std::nullopt, std::nullopt}};
}

std::future<ToolResult> QuestionTool::execute(const json &args, const ToolContext &ctx) {
  // This would typically be handled by the session to interact with the user
  return std::async(std::launch::async, [args]() -> ToolResult {
    auto questions = args.value("questions", json::array());

    std::ostringstream output;
    output << "Questions for user:\n";

    for (size_t i = 0; i < questions.size(); i++) {
      auto q = questions[i];
      output << "\n" << (i + 1) << ". " << q.value("question", "") << "\n";

      if (q.contains("options")) {
        for (const auto &opt : q["options"]) {
          output << "   - " << opt.value("label", "") << ": " << opt.value("description", "") << "\n";
        }
      }
    }

    return ToolResult::with_title(output.str(), "Waiting for user response");
  });
}

// ============================================================================
// TaskTool
// ============================================================================

TaskTool::TaskTool() : SimpleTool("task", "Launch a new agent to handle complex, multistep tasks autonomously.") {}

std::vector<ParameterSchema> TaskTool::parameters() const {
  return {{"prompt", "string", "The task for the agent to perform", true, std::nullopt, std::nullopt},
          {"description", "string", "A short description of the task", true, std::nullopt, std::nullopt},
          {"subagent_type", "string", "The type of agent to use", true, std::nullopt, std::vector<std::string>{"general", "explore"}},
          {"task_id", "string", "Resume a previous task session", false, std::nullopt, std::nullopt}};
}

std::future<ToolResult> TaskTool::execute(const json &args, const ToolContext &ctx) {
  return std::async(std::launch::async, [args, ctx]() -> ToolResult {
    std::string prompt = args.value("prompt", "");
    std::string description = args.value("description", "");
    std::string agent_type_str = args.value("subagent_type", "general");

    // Check if we have the child session creation callback
    if (!ctx.create_child_session) {
      return ToolResult::error("Task tool requires a session context to create child sessions");
    }

    // Map agent type string to enum
    AgentType agent_type = AgentType::General;  // default
    if (agent_type_str == "explore") {
      agent_type = AgentType::Explore;
    } else if (agent_type_str == "general") {
      agent_type = AgentType::General;
    }

    // Create child session
    auto child_session = ctx.create_child_session(agent_type);
    if (!child_session) {
      return ToolResult::error("Failed to create child session");
    }

    // Collect the response
    std::string response_text;
    std::promise<void> completion_promise;
    auto completion_future = completion_promise.get_future();

    // Set up callbacks to capture the response
    child_session->on_stream([&response_text](const std::string &text) {
      response_text += text;
    });

    child_session->on_complete([&completion_promise](FinishReason reason) {
      completion_promise.set_value();
    });

    child_session->on_error([&response_text, &completion_promise](const std::string &error) {
      response_text = "Error: " + error;
      completion_promise.set_value();
    });

    // Send the prompt to the child session
    child_session->prompt(prompt);

    // Wait for completion
    completion_future.wait();

    // Return the result
    return ToolResult::with_title(response_text.empty() ? "Task completed with no output" : response_text, "Task: " + description);
  });
}

// ============================================================================
// Registration
// ============================================================================

void register_builtins() {
  auto &registry = ToolRegistry::instance();

  registry.register_tool(std::make_shared<BashTool>());
  registry.register_tool(std::make_shared<ReadTool>());
  registry.register_tool(std::make_shared<WriteTool>());
  registry.register_tool(std::make_shared<EditTool>());
  registry.register_tool(std::make_shared<GlobTool>());
  registry.register_tool(std::make_shared<GrepTool>());
  registry.register_tool(std::make_shared<QuestionTool>());
  registry.register_tool(std::make_shared<TaskTool>());
}

}  // namespace agent::tools
