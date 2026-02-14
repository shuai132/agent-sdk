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
