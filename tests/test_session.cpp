#include <gtest/gtest.h>

#include "session/session.hpp"

using namespace agent;

class SessionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    config_ = Config::load_default();
  }

  Config config_;
};

TEST_F(SessionTest, CreateSession) {
  asio::io_context io_ctx;
  auto session = Session::create(io_ctx, config_, AgentType::Build);

  EXPECT_FALSE(session->id().empty());
  EXPECT_EQ(session->state(), SessionState::Idle);
}

TEST_F(SessionTest, AddMessage) {
  asio::io_context io_ctx;
  auto session = Session::create(io_ctx, config_, AgentType::Build);

  session->add_message(Message::user("Hello"));

  auto messages = session->messages();
  ASSERT_EQ(messages.size(), 1);
  EXPECT_EQ(messages[0].text(), "Hello");
}

TEST_F(SessionTest, CreateChildSession) {
  asio::io_context io_ctx;
  auto parent = Session::create(io_ctx, config_, AgentType::Build);
  auto child = parent->create_child(AgentType::Explore);

  EXPECT_TRUE(child->parent_id().has_value());
  EXPECT_EQ(*child->parent_id(), parent->id());
}

TEST_F(SessionTest, WorkingDirectoryInjection) {
  asio::io_context io_ctx;

  // Set a specific working directory for testing
  config_.working_dir = "/tmp/test_project";

  auto session = Session::create(io_ctx, config_, AgentType::Build);

  // Get the agent config that was created during session creation
  // We need to check that the working directory was injected into system_prompt
  auto agent_config = session->agent_config();

  EXPECT_TRUE(agent_config.system_prompt.find("当前工作目录：/tmp/test_project") != std::string::npos);
  EXPECT_TRUE(agent_config.system_prompt.find("默认相对于此工作目录进行") != std::string::npos);
}

TEST_F(SessionTest, GetContextMessagesWithSummary) {
  asio::io_context io_ctx;
  auto session = Session::create(io_ctx, config_, AgentType::Build);

  // Add some messages
  session->add_message(Message::user("First question"));
  session->add_message(Message::assistant("First answer"));
  session->add_message(Message::user("Second question"));
  session->add_message(Message::assistant("Second answer"));

  // Add a summary message
  Message summary(Role::Assistant, "");
  summary.add_text("Summary of conversation so far");
  summary.set_summary(true);
  summary.set_finished(true);
  session->add_message(std::move(summary));

  // Add messages after summary
  session->add_message(Message::user("Third question"));
  session->add_message(Message::assistant("Third answer"));

  // get_context_messages should return only summary + messages after it
  auto context = session->get_context_messages();

  // Should have: summary + user("Third question") + assistant("Third answer")
  ASSERT_EQ(context.size(), 3);
  EXPECT_TRUE(context[0].is_summary());
  EXPECT_EQ(context[0].text(), "Summary of conversation so far");
  EXPECT_EQ(context[1].text(), "Third question");
  EXPECT_EQ(context[2].text(), "Third answer");

  // But all messages are still stored
  EXPECT_EQ(session->messages().size(), 7);
}

TEST_F(SessionTest, GetContextMessagesNoSummary) {
  asio::io_context io_ctx;
  auto session = Session::create(io_ctx, config_, AgentType::Build);

  session->add_message(Message::user("Hello"));
  session->add_message(Message::assistant("Hi"));

  // Without summary, get_context_messages returns all
  auto context = session->get_context_messages();
  ASSERT_EQ(context.size(), 2);
}
