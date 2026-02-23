#include "skill/skill.hpp"

#include "builtins.hpp"

namespace agent::tools {

// ============================================================================
// SkillTool — load a skill's instructions on demand
// ============================================================================

SkillTool::SkillTool() : SimpleTool("skill", "Load a specialized skill that provides domain-specific instructions and workflows.") {}

std::string SkillTool::description() const {
  // Build dynamic description that lists available skills
  auto skills = skill::SkillRegistry::instance().all();
  if (skills.empty()) {
    return "Load a specialized skill. No skills are currently available.";
  }

  std::string desc =
      "Load a specialized skill that provides domain-specific instructions and workflows.\n"
      "When you recognize that a task matches one of the available skills listed below, "
      "use this tool to load the full skill instructions.\n\n"
      "<available_skills>\n";

  for (const auto& s : skills) {
    desc += "  <skill>\n";
    desc += "    <name>" + s.name + "</name>\n";
    desc += "    <description>" + s.description + "</description>\n";
    if (s.source_path.string().find("/.") != std::string::npos) {
      // Include location hint for debugging
      desc += "    <location>" + s.source_path.string() + "</location>\n";
    }
    desc += "  </skill>\n";
  }

  desc += "</available_skills>";
  return desc;
}

std::vector<ParameterSchema> SkillTool::parameters() const {
  return {{"name", "string", "The name of the skill to load (from available_skills)", true, std::nullopt, std::nullopt}};
}

std::future<ToolResult> SkillTool::execute(const json& args, const ToolContext& ctx) {
  return std::async(std::launch::async, [args]() -> ToolResult {
    std::string name = args.value("name", "");
    if (name.empty()) {
      return ToolResult::error("Skill name is required");
    }

    auto skill = skill::SkillRegistry::instance().get(name);
    if (!skill) {
      // List available skills in error message
      auto all = skill::SkillRegistry::instance().all();
      std::string available;
      for (const auto& s : all) {
        if (!available.empty()) available += ", ";
        available += s.name;
      }
      return ToolResult::error("Skill '" + name + "' not found. Available skills: " + (available.empty() ? "(none)" : available));
    }

    // Skill directory is the parent of SKILL.md
    auto skill_dir = skill->source_path.parent_path();
    auto scripts_dir = skill_dir / "scripts";

    // Return skill content wrapped in a skill_content tag, including path information
    std::string output = "<skill_content name=\"" + skill->name + "\">\n";
    output += "<skill_path>" + skill_dir.string() + "</skill_path>\n";

    // Check if scripts directory exists
    if (std::filesystem::exists(scripts_dir) && std::filesystem::is_directory(scripts_dir)) {
      output += "<scripts_path>" + scripts_dir.string() + "</scripts_path>\n";
    }

    output += "\n" + skill->body + "\n</skill_content>";

    return ToolResult::with_title(output, "Loaded skill: " + skill->name);
  });
}

}  // namespace agent::tools
