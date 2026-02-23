#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "core/config.hpp"
#include "skill/skill.hpp"

using namespace agent;
using namespace agent::skill;

namespace fs = std::filesystem;

// ============================================================================
// Skill name validation tests
// ============================================================================

TEST(SkillNameTest, ValidNames) {
  EXPECT_TRUE(validate_skill_name("git-release"));
  EXPECT_TRUE(validate_skill_name("a"));
  EXPECT_TRUE(validate_skill_name("abc123"));
  EXPECT_TRUE(validate_skill_name("my-cool-skill"));
  EXPECT_TRUE(validate_skill_name("a1-b2-c3"));
  EXPECT_TRUE(validate_skill_name("skill"));
}

TEST(SkillNameTest, InvalidNames) {
  EXPECT_FALSE(validate_skill_name(""));
  EXPECT_FALSE(validate_skill_name("-start"));
  EXPECT_FALSE(validate_skill_name("end-"));
  EXPECT_FALSE(validate_skill_name("double--dash"));
  EXPECT_FALSE(validate_skill_name("UPPER"));
  EXPECT_FALSE(validate_skill_name("has space"));
  EXPECT_FALSE(validate_skill_name("has_underscore"));
  EXPECT_FALSE(validate_skill_name("has.dot"));
  EXPECT_FALSE(validate_skill_name("has/slash"));

  // Too long (65 chars)
  std::string long_name(65, 'a');
  EXPECT_FALSE(validate_skill_name(long_name));
}

TEST(SkillNameTest, MaxLength) {
  // Exactly 64 chars should be valid
  std::string max_name(64, 'a');
  EXPECT_TRUE(validate_skill_name(max_name));
}

// ============================================================================
// SKILL.md parsing tests
// ============================================================================

class SkillParserTest : public ::testing::Test {
 protected:
  fs::path test_dir_;

  void SetUp() override {
    test_dir_ = fs::temp_directory_path() / "agent_sdk_skill_test";
    fs::create_directories(test_dir_);
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(test_dir_, ec);
  }

  fs::path create_skill(const std::string& name, const std::string& content) {
    auto skill_dir = test_dir_ / name;
    fs::create_directories(skill_dir);
    auto skill_file = skill_dir / "SKILL.md";
    std::ofstream(skill_file) << content;
    return skill_file;
  }
};

TEST_F(SkillParserTest, ParseValidSkill) {
  auto path = create_skill("git-release",
                           "---\n"
                           "name: git-release\n"
                           "description: Create consistent releases\n"
                           "license: MIT\n"
                           "---\n"
                           "## What I do\n"
                           "- Draft release notes\n");

  auto result = parse_skill_file(path);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.skill->name, "git-release");
  EXPECT_EQ(result.skill->description, "Create consistent releases");
  EXPECT_EQ(result.skill->license.value_or(""), "MIT");
  EXPECT_TRUE(result.skill->body.find("What I do") != std::string::npos);
}

TEST_F(SkillParserTest, ParseSkillWithMetadata) {
  auto path = create_skill("my-skill",
                           "---\n"
                           "name: my-skill\n"
                           "description: A test skill\n"
                           "metadata:\n"
                           "  audience: developers\n"
                           "  workflow: github\n"
                           "---\n"
                           "Body content\n");

  auto result = parse_skill_file(path);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.skill->metadata["audience"], "developers");
  EXPECT_EQ(result.skill->metadata["workflow"], "github");
}

TEST_F(SkillParserTest, ParseMultilineDescription) {
  // Test multiline description (YAML indented continuation)
  auto path = create_skill("git-commit",
                           "---\n"
                           "name: git-commit\n"
                           "description: 智能Git提交助手。触发场景：\n"
                           "  - 用户说\"提交\"、\"commit\"等\n"
                           "  - 必须用户主动触发\n"
                           "---\n"
                           "Body content\n");

  auto result = parse_skill_file(path);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.skill->name, "git-commit");
  // Should contain all lines joined with space
  EXPECT_TRUE(result.skill->description.find("智能Git提交助手") != std::string::npos);
  EXPECT_TRUE(result.skill->description.find("用户说") != std::string::npos);
  EXPECT_TRUE(result.skill->description.find("必须用户主动触发") != std::string::npos);
}

TEST_F(SkillParserTest, ParseLiteralBlockDescription) {
  // Test YAML literal block style (|)
  auto path = create_skill("literal-skill",
                           "---\n"
                           "name: literal-skill\n"
                           "description: |\n"
                           "  This is a literal block.\n"
                           "  Multiple lines are preserved.\n"
                           "---\n"
                           "Body content\n");

  auto result = parse_skill_file(path);
  ASSERT_TRUE(result.ok());
  EXPECT_TRUE(result.skill->description.find("literal block") != std::string::npos);
  EXPECT_TRUE(result.skill->description.find("Multiple lines") != std::string::npos);
}

TEST_F(SkillParserTest, MissingFrontmatter) {
  auto path = create_skill("bad-skill", "No frontmatter here\n");

  auto result = parse_skill_file(path);
  EXPECT_FALSE(result.ok());
  EXPECT_TRUE(result.error->find("Missing YAML frontmatter") != std::string::npos);
}

TEST_F(SkillParserTest, MissingName) {
  auto path = create_skill("no-name",
                           "---\n"
                           "description: Missing name\n"
                           "---\n"
                           "Body\n");

  auto result = parse_skill_file(path);
  EXPECT_FALSE(result.ok());
  EXPECT_TRUE(result.error->find("name") != std::string::npos);
}

TEST_F(SkillParserTest, MissingDescription) {
  auto path = create_skill("no-desc",
                           "---\n"
                           "name: no-desc\n"
                           "---\n"
                           "Body\n");

  auto result = parse_skill_file(path);
  EXPECT_FALSE(result.ok());
  EXPECT_TRUE(result.error->find("description") != std::string::npos);
}

TEST_F(SkillParserTest, NameDirMismatch) {
  auto path = create_skill("actual-dir",
                           "---\n"
                           "name: wrong-name\n"
                           "description: Mismatched\n"
                           "---\n"
                           "Body\n");

  auto result = parse_skill_file(path);
  EXPECT_FALSE(result.ok());
  EXPECT_TRUE(result.error->find("does not match") != std::string::npos);
}

TEST_F(SkillParserTest, InvalidSkillName) {
  auto path = create_skill("INVALID",
                           "---\n"
                           "name: INVALID\n"
                           "description: Bad name\n"
                           "---\n"
                           "Body\n");

  auto result = parse_skill_file(path);
  EXPECT_FALSE(result.ok());
  EXPECT_TRUE(result.error->find("Invalid skill name") != std::string::npos);
}

// ============================================================================
// Skill discovery / registry tests
// ============================================================================

class SkillRegistryTest : public ::testing::Test {
 protected:
  fs::path test_dir_;

  void SetUp() override {
    test_dir_ = fs::temp_directory_path() / "agent_sdk_registry_test";
    fs::create_directories(test_dir_);
    SkillRegistry::instance().clear();
  }

  void TearDown() override {
    SkillRegistry::instance().clear();
    std::error_code ec;
    fs::remove_all(test_dir_, ec);
  }

  void create_skill_in(const fs::path& base_dir, const std::string& name, const std::string& desc) {
    auto skill_dir = base_dir / "skills" / name;
    fs::create_directories(skill_dir);
    std::ofstream(skill_dir / "SKILL.md") << "---\nname: " << name << "\ndescription: " << desc << "\n---\nBody for " << name << "\n";
  }
};

TEST_F(SkillRegistryTest, DiscoverFromAgentsSdkDir) {
  auto agent_sdk_dir = test_dir_ / ".agent-sdk";
  create_skill_in(agent_sdk_dir, "my-tool", "A test tool");

  SkillRegistry::instance().discover(test_dir_);

  // Should contain at least our skill (global skills may also be found)
  EXPECT_GE(SkillRegistry::instance().size(), 1);
  auto skill = SkillRegistry::instance().get("my-tool");
  ASSERT_TRUE(skill.has_value());
  EXPECT_EQ(skill->description, "A test tool");
}

TEST_F(SkillRegistryTest, DiscoverFromAgentsDir) {
  auto agents_dir = test_dir_ / ".agents";
  create_skill_in(agents_dir, "shared-skill", "A shared skill");

  SkillRegistry::instance().discover(test_dir_);

  auto skill = SkillRegistry::instance().get("shared-skill");
  ASSERT_TRUE(skill.has_value());
  EXPECT_EQ(skill->description, "A shared skill");
}

TEST_F(SkillRegistryTest, DiscoverFromClaudeDir) {
  auto claude_dir = test_dir_ / ".claude";
  create_skill_in(claude_dir, "claude-skill", "Claude compatible");

  SkillRegistry::instance().discover(test_dir_);

  auto skill = SkillRegistry::instance().get("claude-skill");
  ASSERT_TRUE(skill.has_value());
}

TEST_F(SkillRegistryTest, DiscoverFromOpenCodeDir) {
  auto opencode_dir = test_dir_ / ".opencode";
  create_skill_in(opencode_dir, "oc-skill", "OpenCode compatible");

  SkillRegistry::instance().discover(test_dir_);

  auto skill = SkillRegistry::instance().get("oc-skill");
  ASSERT_TRUE(skill.has_value());
}

TEST_F(SkillRegistryTest, FirstWinsDedup) {
  // Create same-named skill in two locations
  auto agent_sdk_dir = test_dir_ / ".agent-sdk";
  auto agents_dir = test_dir_ / ".agents";
  create_skill_in(agent_sdk_dir, "dup-skill", "From agent-sdk");
  create_skill_in(agents_dir, "dup-skill", "From agents");

  SkillRegistry::instance().discover(test_dir_);

  // .agent-sdk is searched first, so its version should win
  auto skill = SkillRegistry::instance().get("dup-skill");
  ASSERT_TRUE(skill.has_value());
  EXPECT_EQ(skill->description, "From agent-sdk");
}

TEST_F(SkillRegistryTest, ExtraPaths) {
  auto extra_dir = test_dir_ / "custom-skills";
  create_skill_in(extra_dir, "extra-skill", "From extra path");

  // Pass extra_dir/skills via extra_paths won't work because discover scans skills_dir directly
  // But we pass just the parent. Actually, scan_skills_dir expects a dir containing */SKILL.md
  auto extra_skills_dir = extra_dir / "skills";
  SkillRegistry::instance().discover(test_dir_, {extra_skills_dir});

  auto skill = SkillRegistry::instance().get("extra-skill");
  ASSERT_TRUE(skill.has_value());
}

TEST_F(SkillRegistryTest, SkipInvalidSkills) {
  auto agent_sdk_dir = test_dir_ / ".agent-sdk";

  // Create an invalid skill (no frontmatter)
  auto bad_dir = agent_sdk_dir / "skills" / "bad-skill";
  fs::create_directories(bad_dir);
  std::ofstream(bad_dir / "SKILL.md") << "No frontmatter\n";

  // Create a valid skill
  create_skill_in(agent_sdk_dir, "good-skill", "Valid skill");

  SkillRegistry::instance().discover(test_dir_);

  // The valid skill should be registered, the invalid one should not
  EXPECT_TRUE(SkillRegistry::instance().get("good-skill").has_value());
  EXPECT_FALSE(SkillRegistry::instance().get("bad-skill").has_value());
}

// ============================================================================
// find_agent_instructions tests (enhanced multi-convention)
// ============================================================================

class AgentInstructionsTest : public ::testing::Test {
 protected:
  fs::path test_dir_;

  void SetUp() override {
    test_dir_ = fs::temp_directory_path() / "agent_sdk_instructions_test";
    fs::create_directories(test_dir_);
    // Create a .git directory to act as git root
    fs::create_directories(test_dir_ / ".git");
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(test_dir_, ec);
  }
};

TEST_F(AgentInstructionsTest, FindAgentsMd) {
  std::ofstream(test_dir_ / "AGENTS.md") << "# Project Rules\n";

  auto results = config_paths::find_agent_instructions(test_dir_);

  // Should find AGENTS.md (may also find global files)
  bool found = false;
  for (const auto& p : results) {
    if (p.filename() == "AGENTS.md" && p.parent_path() == test_dir_) {
      found = true;
    }
  }
  EXPECT_TRUE(found);
}

TEST_F(AgentInstructionsTest, FindClaudeMd) {
  std::ofstream(test_dir_ / "CLAUDE.md") << "# Claude Rules\n";

  auto results = config_paths::find_agent_instructions(test_dir_);

  bool found = false;
  for (const auto& p : results) {
    if (p.filename() == "CLAUDE.md" && p.parent_path() == test_dir_) {
      found = true;
    }
  }
  EXPECT_TRUE(found);
}

TEST_F(AgentInstructionsTest, FindInAgentsSdkDir) {
  auto dir = test_dir_ / ".agent-sdk";
  fs::create_directories(dir);
  std::ofstream(dir / "AGENTS.md") << "# Agent-sdk Rules\n";

  auto results = config_paths::find_agent_instructions(test_dir_);

  bool found = false;
  for (const auto& p : results) {
    if (p.parent_path() == dir) {
      found = true;
    }
  }
  EXPECT_TRUE(found);
}

TEST_F(AgentInstructionsTest, FindInAgentsDir) {
  auto dir = test_dir_ / ".agents";
  fs::create_directories(dir);
  std::ofstream(dir / "AGENTS.md") << "# Agents Rules\n";

  auto results = config_paths::find_agent_instructions(test_dir_);

  bool found = false;
  for (const auto& p : results) {
    if (p.parent_path() == dir) {
      found = true;
    }
  }
  EXPECT_TRUE(found);
}

TEST_F(AgentInstructionsTest, StopsAtGitRoot) {
  // Create a subdirectory
  auto sub = test_dir_ / "src" / "foo";
  fs::create_directories(sub);
  std::ofstream(test_dir_ / "AGENTS.md") << "# Root\n";
  std::ofstream(sub / "AGENTS.md") << "# Sub\n";

  auto results = config_paths::find_agent_instructions(sub);

  // Should find both: parent root's AGENTS.md + sub's AGENTS.md
  int project_count = 0;
  for (const auto& p : results) {
    if (p.string().find(test_dir_.string()) != std::string::npos) {
      project_count++;
    }
  }
  EXPECT_GE(project_count, 2);
}

TEST_F(AgentInstructionsTest, FindGitRoot) {
  auto root = config_paths::find_git_root(test_dir_);
  ASSERT_TRUE(root.has_value());
  EXPECT_EQ(*root, test_dir_);

  // Subdirectory should also find the same root
  auto sub = test_dir_ / "deep" / "nested";
  fs::create_directories(sub);
  auto root2 = config_paths::find_git_root(sub);
  ASSERT_TRUE(root2.has_value());
  EXPECT_EQ(*root2, test_dir_);
}
