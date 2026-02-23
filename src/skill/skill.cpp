#include "skill.hpp"

#include <spdlog/spdlog.h>

#include <fstream>
#include <sstream>

#include "core/config.hpp"

namespace agent::skill {

namespace fs = std::filesystem;

// ============================================================================
// Name validation
// ============================================================================

bool validate_skill_name(const std::string& name) {
  if (name.empty() || name.size() > 64) return false;

  // Must match: ^[a-z0-9]+(-[a-z0-9]+)*$
  static const std::regex pattern("^[a-z0-9]+(-[a-z0-9]+)*$");
  return std::regex_match(name, pattern);
}

// ============================================================================
// SKILL.md parser
// ============================================================================

// Extract YAML frontmatter between --- delimiters
// Returns: {frontmatter_string, body_string}
static std::pair<std::string, std::string> split_frontmatter(const std::string& content) {
  // Must start with ---
  if (content.substr(0, 3) != "---") {
    return {"", content};
  }

  auto end_pos = content.find("\n---", 3);
  if (end_pos == std::string::npos) {
    return {"", content};
  }

  // Skip the first "---\n"
  size_t fm_start = (content.size() > 3 && content[3] == '\n') ? 4 : 3;
  std::string frontmatter = content.substr(fm_start, end_pos - fm_start);

  // Body starts after the closing "---\n"
  size_t body_start = end_pos + 4;  // skip "\n---"
  if (body_start < content.size() && content[body_start] == '\n') {
    body_start++;
  }
  std::string body = (body_start < content.size()) ? content.substr(body_start) : "";

  return {frontmatter, body};
}

// Simple YAML-like parser for frontmatter
// Handles multiline values (indented continuation lines joined with space)
static std::map<std::string, std::string> parse_flat_yaml(const std::string& yaml) {
  std::map<std::string, std::string> result;
  std::istringstream stream(yaml);
  std::string line;
  std::string current_key;
  std::string current_value;

  auto trim = [](const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    size_t end = s.find_last_not_of(" \t\r\n");
    return (start == std::string::npos) ? "" : s.substr(start, end - start + 1);
  };

  auto is_indented = [](const std::string& s) {
    return !s.empty() && (s[0] == ' ' || s[0] == '\t');
  };

  auto save_current = [&]() {
    if (!current_key.empty()) {
      // Strip quotes if present
      if (current_value.size() >= 2 &&
          ((current_value.front() == '"' && current_value.back() == '"') ||
           (current_value.front() == '\'' && current_value.back() == '\''))) {
        current_value = current_value.substr(1, current_value.size() - 2);
      }
      result[current_key] = current_value;
    }
  };

  while (std::getline(stream, line)) {
    if (line.empty() || trim(line).empty() || trim(line)[0] == '#') continue;

    if (is_indented(line)) {
      // Continuation of previous value
      if (!current_key.empty()) {
        std::string content = trim(line);
        if (!content.empty()) {
          if (!current_value.empty()) current_value += " ";
          current_value += content;
        }
      }
    } else {
      // New key-value pair
      save_current();

      auto colon_pos = line.find(':');
      if (colon_pos == std::string::npos) continue;

      current_key = trim(line.substr(0, colon_pos));
      current_value = trim(line.substr(colon_pos + 1));
    }
  }

  save_current();
  return result;
}

// Parse the metadata block (indented key-value pairs under "metadata:")
static std::map<std::string, std::string> parse_metadata_block(const std::string& yaml) {
  std::map<std::string, std::string> result;
  std::istringstream stream(yaml);
  std::string line;
  bool in_metadata = false;

  while (std::getline(stream, line)) {
    // Detect "metadata:" line
    auto trimmed = line;
    auto start_pos = trimmed.find_first_not_of(" \t");
    if (start_pos != std::string::npos) {
      trimmed = trimmed.substr(start_pos);
    }

    if (trimmed == "metadata:" || trimmed == "metadata: ") {
      in_metadata = true;
      continue;
    }

    if (in_metadata) {
      // Check indentation — metadata entries must be indented
      if (line.empty() || (line[0] != ' ' && line[0] != '\t')) {
        in_metadata = false;
        continue;
      }

      auto colon_pos = line.find(':');
      if (colon_pos == std::string::npos) continue;

      std::string key = line.substr(0, colon_pos);
      std::string value = line.substr(colon_pos + 1);

      auto trim = [](std::string& s) {
        size_t st = s.find_first_not_of(" \t\r\n");
        size_t en = s.find_last_not_of(" \t\r\n");
        s = (st == std::string::npos) ? "" : s.substr(st, en - st + 1);
      };

      trim(key);
      trim(value);

      if (!key.empty()) {
        result[key] = value;
      }
    }
  }

  return result;
}

ParseResult parse_skill_file(const fs::path& path) {
  // Read file
  std::ifstream file(path);
  if (!file.is_open()) {
    return {std::nullopt, "Cannot open file: " + path.string()};
  }

  std::ostringstream ss;
  ss << file.rdbuf();
  std::string content = ss.str();

  // Split frontmatter and body
  auto [frontmatter, body] = split_frontmatter(content);
  if (frontmatter.empty()) {
    return {std::nullopt, "Missing YAML frontmatter in: " + path.string()};
  }

  // Parse frontmatter
  auto fields = parse_flat_yaml(frontmatter);

  // Validate required fields
  auto name_it = fields.find("name");
  if (name_it == fields.end() || name_it->second.empty()) {
    return {std::nullopt, "Missing required 'name' field in: " + path.string()};
  }

  auto desc_it = fields.find("description");
  if (desc_it == fields.end() || desc_it->second.empty()) {
    return {std::nullopt, "Missing required 'description' field in: " + path.string()};
  }

  // Validate name format
  if (!validate_skill_name(name_it->second)) {
    return {std::nullopt, "Invalid skill name '" + name_it->second + "' in: " + path.string()};
  }

  // Validate name matches directory name
  auto parent_dir = path.parent_path().filename().string();
  if (parent_dir != name_it->second) {
    return {std::nullopt, "Skill name '" + name_it->second + "' does not match directory '" + parent_dir + "' in: " + path.string()};
  }

  // Validate description length
  if (desc_it->second.size() > 1024) {
    return {std::nullopt, "Description exceeds 1024 characters in: " + path.string()};
  }

  // Build SkillInfo
  SkillInfo skill;
  skill.name = name_it->second;
  skill.description = desc_it->second;
  skill.body = body;
  skill.source_path = fs::canonical(path);

  if (auto it = fields.find("license"); it != fields.end()) {
    skill.license = it->second;
  }
  if (auto it = fields.find("compatibility"); it != fields.end()) {
    skill.compatibility = it->second;
  }

  // Parse metadata block
  skill.metadata = parse_metadata_block(frontmatter);

  return {std::move(skill), std::nullopt};
}

// ============================================================================
// SkillRegistry
// ============================================================================

SkillRegistry& SkillRegistry::instance() {
  static SkillRegistry registry;
  return registry;
}

void SkillRegistry::register_skill(SkillInfo skill) {
  // First-wins dedup: if a skill with this name already exists, skip
  if (skills_.count(skill.name)) {
    spdlog::debug("Skill '{}' already registered (from {}), skipping duplicate from {}", skill.name, skills_[skill.name].source_path.string(),
                  skill.source_path.string());
    return;
  }

  spdlog::info("Registered skill '{}' from {}", skill.name, skill.source_path.string());
  skills_[skill.name] = std::move(skill);
}

void SkillRegistry::scan_skills_dir(const fs::path& skills_dir) {
  if (!fs::exists(skills_dir) || !fs::is_directory(skills_dir)) {
    return;
  }

  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(skills_dir, ec)) {
    if (!entry.is_directory()) continue;

    auto skill_md = entry.path() / "SKILL.md";
    if (!fs::exists(skill_md)) continue;

    auto result = parse_skill_file(skill_md);
    if (result.ok()) {
      register_skill(std::move(*result.skill));
    } else {
      spdlog::warn("Failed to load skill from {}: {}", skill_md.string(), result.error.value_or("unknown error"));
    }
  }
}

void SkillRegistry::discover(const fs::path& start_dir, const std::vector<fs::path>& extra_paths) {
  std::lock_guard<std::mutex> lock(mutex_);

  // Skill search directories within each traversed directory
  static constexpr const char* PROJECT_SKILL_DIRS[] = {
      ".agent-sdk/skills",
      ".agents/skills",
      ".claude/skills",
      ".opencode/skills",
  };

  // 1. Scan project-local paths (traversing up to git root)
  auto git_root = config_paths::find_git_root(start_dir);

  fs::path current = start_dir;
  while (true) {
    for (const auto& dir : PROJECT_SKILL_DIRS) {
      scan_skills_dir(current / dir);
    }

    // Stop at git root
    if (git_root && current == *git_root) break;

    auto parent = current.parent_path();
    if (parent == current) break;  // Filesystem root
    current = parent;
  }

  // 2. Scan global paths
  auto home = config_paths::home_dir();
  std::vector<fs::path> global_skill_dirs = {
      config_paths::config_dir() / "skills",     // ~/.config/agent-sdk/skills/
      home / ".agents" / "skills",               // ~/.agents/skills/
      home / ".claude" / "skills",               // ~/.claude/skills/
      home / ".config" / "opencode" / "skills",  // ~/.config/opencode/skills/
  };

  for (const auto& dir : global_skill_dirs) {
    scan_skills_dir(dir);
  }

  // 3. Scan additional paths from config
  for (const auto& path : extra_paths) {
    scan_skills_dir(path);
  }

  spdlog::info("Skill discovery complete: {} skills registered", skills_.size());
}

std::optional<SkillInfo> SkillRegistry::get(const std::string& name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = skills_.find(name);
  if (it != skills_.end()) {
    return it->second;
  }
  return std::nullopt;
}

std::vector<SkillInfo> SkillRegistry::all() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<SkillInfo> result;
  result.reserve(skills_.size());
  for (const auto& [name, skill] : skills_) {
    result.push_back(skill);
  }
  return result;
}

size_t SkillRegistry::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return skills_.size();
}

void SkillRegistry::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  skills_.clear();
}

}  // namespace agent::skill
