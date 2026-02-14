#include <gtest/gtest.h>
#include "agent/core/message.hpp"

using namespace agent;

TEST(MessageTest, CreateUserMessage) {
    auto msg = Message::user("Hello, world!");
    
    EXPECT_EQ(msg.role(), Role::User);
    EXPECT_EQ(msg.text(), "Hello, world!");
    EXPECT_FALSE(msg.is_finished());
}

TEST(MessageTest, CreateAssistantMessage) {
    auto msg = Message::assistant("Hi there!");
    
    EXPECT_EQ(msg.role(), Role::Assistant);
    EXPECT_EQ(msg.text(), "Hi there!");
}

TEST(MessageTest, AddToolCall) {
    auto msg = Message::assistant("");
    msg.add_tool_call("tc_123", "bash", {{"command", "ls -la"}});
    
    auto tool_calls = msg.tool_calls();
    ASSERT_EQ(tool_calls.size(), 1);
    EXPECT_EQ(tool_calls[0]->id, "tc_123");
    EXPECT_EQ(tool_calls[0]->name, "bash");
}

TEST(MessageTest, AddToolResult) {
    auto msg = Message::user("");
    msg.add_tool_result("tc_123", "bash", "file1.txt\nfile2.txt");
    
    auto results = msg.tool_results();
    ASSERT_EQ(results.size(), 1);
    EXPECT_EQ(results[0]->tool_call_id, "tc_123");
    EXPECT_EQ(results[0]->output, "file1.txt\nfile2.txt");
    EXPECT_FALSE(results[0]->is_error);
}

TEST(MessageTest, JsonSerialization) {
    auto msg = Message::user("Test message");
    msg.add_tool_result("tc_1", "read", "content");
    
    auto j = msg.to_json();
    
    EXPECT_EQ(j["role"], "user");
    EXPECT_TRUE(j.contains("parts"));
}
