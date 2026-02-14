#include <gtest/gtest.h>
#include "agent/tool/tool.hpp"
#include "agent/tool/builtin/builtins.hpp"

using namespace agent;

TEST(ToolTest, ToolRegistration) {
    auto& registry = ToolRegistry::instance();
    
    // Register builtins
    tools::register_builtins();
    
    // Check bash tool exists
    auto bash = registry.get("bash");
    ASSERT_NE(bash, nullptr);
    EXPECT_EQ(bash->id(), "bash");
}

TEST(ToolTest, ToolParameters) {
    tools::register_builtins();
    
    auto& registry = ToolRegistry::instance();
    auto read = registry.get("read");
    ASSERT_NE(read, nullptr);
    
    auto params = read->parameters();
    EXPECT_GT(params.size(), 0);
    
    // Check for required filePath parameter
    bool has_file_path = false;
    for (const auto& param : params) {
        if (param.name == "filePath") {
            has_file_path = true;
            EXPECT_TRUE(param.required);
        }
    }
    EXPECT_TRUE(has_file_path);
}

TEST(ToolTest, ToolJsonSchema) {
    tools::register_builtins();
    
    auto& registry = ToolRegistry::instance();
    auto glob = registry.get("glob");
    ASSERT_NE(glob, nullptr);
    
    auto schema = glob->to_json_schema();
    
    EXPECT_EQ(schema["name"], "glob");
    EXPECT_TRUE(schema.contains("description"));
    EXPECT_TRUE(schema.contains("input_schema"));
}

TEST(TruncateTest, NoTruncationNeeded) {
    std::string short_text = "Hello, world!";
    auto result = Truncate::output(short_text);
    
    EXPECT_FALSE(result.truncated);
    EXPECT_EQ(result.content, short_text);
}

TEST(TruncateTest, TruncateByLines) {
    std::string long_text;
    for (int i = 0; i < 3000; i++) {
        long_text += "Line " + std::to_string(i) + "\n";
    }
    
    auto result = Truncate::output(long_text, 100);
    
    EXPECT_TRUE(result.truncated);
    EXPECT_LT(result.content.size(), long_text.size());
}
