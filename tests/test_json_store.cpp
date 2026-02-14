#include <gtest/gtest.h>

#include <filesystem>

#include "core/json_store.hpp"
#include "session/session.hpp"

using namespace agent;
namespace fs = std::filesystem;

class JsonStoreTest : public ::testing::Test {
 protected:
  void SetUp() override {
    test_dir_ = fs::temp_directory_path() / ("agent_test_" + UUID::generate());
    store_ = std::make_shared<JsonMessageStore>(test_dir_);
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(test_dir_, ec);
  }

  fs::path test_dir_;
  std::shared_ptr<JsonMessageStore> store_;
};

TEST_F(JsonStoreTest, SaveAndGetMessage) {
  auto msg = Message::user("Hello, world!");
  msg.set_session_id("session-1");

  store_->save(msg);

  auto loaded = store_->get(msg.id());
  ASSERT_TRUE(loaded.has_value());
  EXPECT_EQ(loaded->id(), msg.id());
  EXPECT_EQ(loaded->role(), Role::User);
  EXPECT_EQ(loaded->text(), "Hello, world!");
  EXPECT_EQ(loaded->session_id(), "session-1");
}

TEST_F(JsonStoreTest, ListBySession) {
  auto msg1 = Message::user("First");
  msg1.set_session_id("session-1");

  auto msg2 = Message::assistant("Second");
  msg2.set_session_id("session-1");

  auto msg3 = Message::user("Other session");
  msg3.set_session_id("session-2");

  // Need session index entries for get() to work across sessions,
  // but list() works directly by session_id
  store_->save(msg1);
  store_->save(msg2);
  store_->save(msg3);

  auto messages = store_->list("session-1");
  ASSERT_EQ(messages.size(), 2);
  EXPECT_EQ(messages[0].text(), "First");
  EXPECT_EQ(messages[1].text(), "Second");

  auto messages2 = store_->list("session-2");
  ASSERT_EQ(messages2.size(), 1);
  EXPECT_EQ(messages2[0].text(), "Other session");
}

TEST_F(JsonStoreTest, UpdateMessage) {
  auto msg = Message::user("Original");
  msg.set_session_id("session-1");
  store_->save(msg);

  // Modify and update
  msg.add_text(" updated");
  store_->update(msg);

  auto loaded = store_->list("session-1");
  ASSERT_EQ(loaded.size(), 1);
  EXPECT_EQ(loaded[0].text(), "Original\n updated");
}

TEST_F(JsonStoreTest, RemoveMessage) {
  // Need a session index entry for remove() to scan across sessions
  SessionMeta meta;
  meta.id = "session-1";
  meta.title = "test";
  store_->save_session(meta);

  auto msg = Message::user("To be removed");
  msg.set_session_id("session-1");
  store_->save(msg);

  auto before = store_->list("session-1");
  ASSERT_EQ(before.size(), 1);

  store_->remove(msg.id());

  auto after = store_->list("session-1");
  EXPECT_EQ(after.size(), 0);
}

TEST_F(JsonStoreTest, SessionMetaCRUD) {
  // Create
  SessionMeta meta;
  meta.id = "sess-abc";
  meta.title = "Test Session";
  meta.agent_type = AgentType::Build;
  meta.parent_id = "parent-123";

  store_->save_session(meta);

  // Read
  auto loaded = store_->get_session("sess-abc");
  ASSERT_TRUE(loaded.has_value());
  EXPECT_EQ(loaded->id, "sess-abc");
  EXPECT_EQ(loaded->title, "Test Session");
  EXPECT_TRUE(loaded->parent_id.has_value());
  EXPECT_EQ(*loaded->parent_id, "parent-123");

  // List
  auto sessions = store_->list_sessions();
  ASSERT_EQ(sessions.size(), 1);
  EXPECT_EQ(sessions[0].id, "sess-abc");

  // Update
  meta.title = "Updated Title";
  store_->save_session(meta);

  auto updated = store_->get_session("sess-abc");
  ASSERT_TRUE(updated.has_value());
  EXPECT_EQ(updated->title, "Updated Title");

  // Still only one session
  EXPECT_EQ(store_->list_sessions().size(), 1);

  // Delete
  store_->remove_session("sess-abc");
  EXPECT_FALSE(store_->get_session("sess-abc").has_value());
  EXPECT_EQ(store_->list_sessions().size(), 0);
}

TEST_F(JsonStoreTest, EmptyStore) {
  // Operations on empty store should not crash
  auto messages = store_->list("nonexistent");
  EXPECT_TRUE(messages.empty());

  auto msg = store_->get("nonexistent-id");
  EXPECT_FALSE(msg.has_value());

  auto session = store_->get_session("nonexistent");
  EXPECT_FALSE(session.has_value());

  auto sessions = store_->list_sessions();
  EXPECT_TRUE(sessions.empty());
}

TEST_F(JsonStoreTest, PersistenceAcrossInstances) {
  // Write with first instance
  auto msg = Message::user("Persisted message");
  msg.set_session_id("session-persist");

  SessionMeta meta;
  meta.id = "session-persist";
  meta.title = "Persistent";
  store_->save_session(meta);
  store_->save(msg);

  // Create a new store instance pointing to the same directory
  auto store2 = std::make_shared<JsonMessageStore>(test_dir_);

  auto loaded_sessions = store2->list_sessions();
  ASSERT_EQ(loaded_sessions.size(), 1);
  EXPECT_EQ(loaded_sessions[0].title, "Persistent");

  auto loaded_messages = store2->list("session-persist");
  ASSERT_EQ(loaded_messages.size(), 1);
  EXPECT_EQ(loaded_messages[0].text(), "Persisted message");
}

TEST_F(JsonStoreTest, ToolCallMessageRoundTrip) {
  auto msg = Message::assistant("");
  msg.set_session_id("session-tools");
  msg.add_tool_call("tc_1", "bash", {{"command", "ls"}});
  msg.set_finished(true);
  msg.set_finish_reason(FinishReason::ToolCalls);
  store_->save(msg);

  auto loaded = store_->list("session-tools");
  ASSERT_EQ(loaded.size(), 1);

  auto tool_calls = loaded[0].tool_calls();
  ASSERT_EQ(tool_calls.size(), 1);
  EXPECT_EQ(tool_calls[0]->id, "tc_1");
  EXPECT_EQ(tool_calls[0]->name, "bash");
  EXPECT_TRUE(loaded[0].is_finished());
  EXPECT_EQ(loaded[0].finish_reason(), FinishReason::ToolCalls);
}

// Integration test: Session with store
class SessionResumeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    test_dir_ = fs::temp_directory_path() / ("agent_resume_test_" + UUID::generate());
    store_ = std::make_shared<JsonMessageStore>(test_dir_);
    config_ = Config::load_default();
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(test_dir_, ec);
  }

  fs::path test_dir_;
  std::shared_ptr<JsonMessageStore> store_;
  Config config_;
};

TEST_F(SessionResumeTest, ResumeFromStore) {
  asio::io_context io_ctx;

  // Create a session with store
  auto session = Session::create(io_ctx, config_, AgentType::Build, store_);
  auto session_id = session->id();

  // Add messages
  session->add_message(Message::user("Hello there"));
  session->add_message(Message::assistant("Hi! How can I help?"));

  // Verify title was auto-generated
  EXPECT_FALSE(session->title().empty());
  EXPECT_EQ(session->title(), "Hello there");

  // Destroy original session
  session.reset();

  // Resume from store
  auto resumed = Session::resume(io_ctx, config_, session_id, store_);
  ASSERT_NE(resumed, nullptr);

  EXPECT_EQ(resumed->id(), session_id);
  EXPECT_EQ(resumed->title(), "Hello there");
  EXPECT_EQ(resumed->messages().size(), 2);
  EXPECT_EQ(resumed->messages()[0].text(), "Hello there");
  EXPECT_EQ(resumed->messages()[1].text(), "Hi! How can I help?");
}

TEST_F(SessionResumeTest, ResumeNonexistent) {
  asio::io_context io_ctx;

  auto resumed = Session::resume(io_ctx, config_, "nonexistent-id", store_);
  EXPECT_EQ(resumed, nullptr);
}

TEST_F(SessionResumeTest, ListAllSessions) {
  asio::io_context io_ctx;

  auto s1 = Session::create(io_ctx, config_, AgentType::Build, store_);
  s1->add_message(Message::user("First session"));

  auto s2 = Session::create(io_ctx, config_, AgentType::Explore, store_);
  s2->add_message(Message::user("Second session"));

  auto sessions = store_->list_sessions();
  ASSERT_EQ(sessions.size(), 2);
}
