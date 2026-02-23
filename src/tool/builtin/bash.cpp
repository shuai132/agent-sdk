#include <spdlog/spdlog.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <sstream>
#include <thread>

#include "builtins.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace agent::tools {

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

std::future<ToolResult> BashTool::execute(const json& args, const ToolContext& ctx) {
  return std::async(std::launch::async, [args, ctx]() -> ToolResult {
    std::string command = args.value("command", "");
    std::string workdir = args.value("workdir", ctx.working_dir);
    int timeout_ms = args.value("timeout", DEFAULT_TIMEOUT_MS);
    std::string description = args.value("description", "");

    if (command.empty()) {
      return ToolResult::error("Command is required");
    }

    spdlog::debug("[BashTool] Executing: command=\"{}\", workdir=\"{}\", timeout={}ms, description=\"{}\"", command, workdir, timeout_ms,
                  description);

    // Check for abort
    if (ctx.abort_signal && ctx.abort_signal->load()) {
      spdlog::warn("[BashTool] Execution cancelled");
      return ToolResult::error("Cancelled");
    }

    std::string output;
    int exit_code = 0;

#ifndef _WIN32
    // Create a pipe for capturing stdout+stderr
    int pipe_fd[2];
    if (pipe(pipe_fd) == -1) {
      return ToolResult::error("Failed to create pipe: " + std::string(strerror(errno)));
    }

    pid_t pid = fork();
    if (pid == -1) {
      close(pipe_fd[0]);
      close(pipe_fd[1]);
      return ToolResult::error("Failed to fork process: " + std::string(strerror(errno)));
    }

    if (pid == 0) {
      // ---- Child process ----
      close(pipe_fd[0]);  // Close read end

      // Redirect stdout and stderr to the pipe
      dup2(pipe_fd[1], STDOUT_FILENO);
      dup2(pipe_fd[1], STDERR_FILENO);
      close(pipe_fd[1]);

      // Change working directory
      if (!workdir.empty() && workdir != ".") {
        if (chdir(workdir.c_str()) != 0) {
          _exit(127);
        }
      }

      // Execute the command via /bin/sh
      execl("/bin/sh", "sh", "-c", command.c_str(), nullptr);
      _exit(127);  // exec failed
    }

    // ---- Parent process ----
    close(pipe_fd[1]);  // Close write end

    // Set the read end to non-blocking
    int flags = fcntl(pipe_fd[0], F_GETFL, 0);
    fcntl(pipe_fd[0], F_SETFL, flags | O_NONBLOCK);

    std::string result;
    std::array<char, 4096> buffer;
    bool timed_out = false;
    bool child_exited = false;

    auto start_time = std::chrono::steady_clock::now();

    // Read loop: poll pipe and check timeout / abort
    while (!child_exited) {
      // Check for abort
      if (ctx.abort_signal && ctx.abort_signal->load()) {
        kill(pid, SIGTERM);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (waitpid(pid, nullptr, WNOHANG) == 0) {
          kill(pid, SIGKILL);
          waitpid(pid, nullptr, 0);
        }
        close(pipe_fd[0]);
        return ToolResult::error("Cancelled");
      }

      // Check timeout
      auto elapsed = std::chrono::steady_clock::now() - start_time;
      if (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() >= timeout_ms) {
        timed_out = true;
        // Graceful termination: SIGTERM first
        kill(pid, SIGTERM);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        // Force kill if still running
        if (waitpid(pid, nullptr, WNOHANG) == 0) {
          kill(pid, SIGKILL);
          waitpid(pid, nullptr, 0);
        }
        break;
      }

      // Try to read available data from pipe
      ssize_t bytes_read = read(pipe_fd[0], buffer.data(), buffer.size());
      if (bytes_read > 0) {
        result.append(buffer.data(), bytes_read);
      } else if (bytes_read == 0) {
        // EOF — pipe closed, child has closed stdout/stderr
        break;
      }
      // bytes_read == -1 && errno == EAGAIN means no data available yet

      // Check if child has exited
      int status = 0;
      pid_t ret = waitpid(pid, &status, WNOHANG);
      if (ret == pid) {
        child_exited = true;
        if (WIFEXITED(status)) {
          exit_code = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
          exit_code = 128 + WTERMSIG(status);
        }
        // Drain remaining data from pipe after child exits
        while (true) {
          ssize_t n = read(pipe_fd[0], buffer.data(), buffer.size());
          if (n > 0) {
            result.append(buffer.data(), n);
          } else {
            break;
          }
        }
      } else if (ret == -1) {
        // waitpid error
        break;
      }

      // If no data and child hasn't exited, sleep briefly to avoid busy-waiting
      if (bytes_read <= 0 && !child_exited) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    }

    // If child hasn't been reaped yet (broke out of loop on EOF), reap it now
    if (!child_exited && !timed_out) {
      int status = 0;
      waitpid(pid, &status, 0);
      if (WIFEXITED(status)) {
        exit_code = WEXITSTATUS(status);
      } else if (WIFSIGNALED(status)) {
        exit_code = 128 + WTERMSIG(status);
      }
    }

    close(pipe_fd[0]);

    if (timed_out) {
      exit_code = 124;  // Convention: 124 indicates timeout (same as GNU timeout)
      output = result + "\n[Timed out after " + std::to_string(timeout_ms / 1000) + "s]";
      spdlog::warn("[BashTool] Command timed out after {}s", timeout_ms / 1000);
    } else {
      output = result;
      spdlog::debug("[BashTool] Command completed with exit code {}", exit_code);
    }
#else
            // Windows implementation would go here
            output = "Windows shell execution not implemented";
            exit_code = 1;
#endif

    // Truncate if needed
    auto truncated = Truncate::save_and_truncate(output, "bash");

    if (exit_code != 0) {
      spdlog::debug("[BashTool] Command failed with exit code {}: {}", exit_code,
                    truncated.content.substr(0, std::min(truncated.content.size(), size_t(200))));
      return ToolResult{truncated.content + "\n[Exit code: " + std::to_string(exit_code) + "]", "Command failed", {{"exit_code", exit_code}}, true};
    }

    spdlog::debug("[BashTool] Command succeeded, output length: {} bytes", truncated.content.size());
    return ToolResult::with_title(truncated.content, "Executed: " + command.substr(0, 50));
  });
}

// ============================================================================
// Registration — registers all builtin tools
// ============================================================================

void register_builtins() {
  auto& registry = ToolRegistry::instance();

  registry.register_tool(std::make_shared<BashTool>());
  registry.register_tool(std::make_shared<ReadTool>());
  registry.register_tool(std::make_shared<WriteTool>());
  registry.register_tool(std::make_shared<EditTool>());
  registry.register_tool(std::make_shared<GlobTool>());
  registry.register_tool(std::make_shared<GrepTool>());
  registry.register_tool(std::make_shared<QuestionTool>());
  registry.register_tool(std::make_shared<TaskTool>());
  registry.register_tool(std::make_shared<SkillTool>());
}

}  // namespace agent::tools
