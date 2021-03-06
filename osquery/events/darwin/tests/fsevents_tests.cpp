/*
 *  Copyright (c) 2014-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

#include <gtest/gtest.h>

#include <osquery/events.h>
#include <osquery/filesystem.h>
#include <osquery/flags.h>
#include <osquery/tables.h>

#include "osquery/core/test_util.h"
#include "osquery/events/darwin/fsevents.h"

namespace fs = boost::filesystem;

namespace osquery {

int kMaxEventLatency = 3000;

DECLARE_bool(verbose);

class FSEventsTests : public testing::Test {
 protected:
  void SetUp() override {
    // FSEvents will use data from the config and config parsers.
    Registry::registry("config_parser")->setUp();

    FLAGS_verbose = true;
    real_test_path = kTestWorkingDirectory + "fsevents-triggers" +
                     std::to_string(rand() % 10000 + 10000);
    // Create a similar directory for embedded paths and directories.
    real_test_dir = kTestWorkingDirectory + "fsevents-triggers" +
                    std::to_string(rand() % 10000 + 10000);
  }

  void TearDown() override {
    remove(real_test_path);
    fs::remove_all(real_test_dir);
  }

  void StartEventLoop() {
    event_pub_ = std::make_shared<FSEventsEventPublisher>();
    event_pub_->no_defer_ = true;
    event_pub_->no_self_ = false;
    EventFactory::registerEventPublisher(event_pub_);
    FILE* fd = fopen(real_test_path.c_str(), "w");
    fclose(fd);

    temp_thread_ = std::thread(EventFactory::run, "fsevents");
    // Wait for the publisher thread and FSEvent run loop to start.
  }

  void EndEventLoop() {
    while (!event_pub_->hasStarted()) {
      ::usleep(20);
    }
    EventFactory::end(false);
    temp_thread_.join();
  }

  void WaitForStream(int max) {
    int delay = 0;
    while (delay < max * 1000) {
      if (event_pub_->isStreamRunning()) {
        return;
      }
      ::usleep(100);
      delay += 100;
    }
  }

  bool WaitForEvents(size_t max, size_t num_events = 0) {
    size_t delay = 0;
    while (delay <= max * 1000) {
      if (num_events > 0 && event_pub_->numEvents() >= num_events) {
        return true;
      } else if (num_events == 0 && event_pub_->numEvents() > 0) {
        return true;
      }
      delay += 100;
      ::usleep(100);
    }
    return false;
  }

  void CreateEvents(int num = 1) {
    WaitForStream(kMaxEventLatency);
    for (int i = 0; i < num; ++i) {
      FILE* fd = fopen(real_test_path.c_str(), "a");
      fputs("fsevents", fd);
      fclose(fd);
    }
  }

  std::shared_ptr<FSEventsEventPublisher> event_pub_{nullptr};
  std::thread temp_thread_;

 public:
  /// Trigger path is the current test's eventing sink (accessed anywhere).
  std::string real_test_path;
  std::string real_test_dir;
};

TEST_F(FSEventsTests, test_register_event_pub) {
  auto pub = std::make_shared<FSEventsEventPublisher>();
  auto status = EventFactory::registerEventPublisher(pub);
  EXPECT_TRUE(status.ok());

  // Make sure only one event type exists
  EXPECT_EQ(EventFactory::numEventPublishers(), 1U);
  status = EventFactory::deregisterEventPublisher("fsevents");
  EXPECT_TRUE(status.ok());
}

TEST_F(FSEventsTests, test_fsevents_add_subscription_missing_path) {
  auto pub = std::make_shared<FSEventsEventPublisher>();
  EventFactory::registerEventPublisher(pub);

  // This subscription path is fake, and will succeed!
  auto mc = std::make_shared<FSEventsSubscriptionContext>();
  mc->path = "/this/path/is/fake";

  auto subscription = Subscription::create("TestSubscriber", mc);
  auto status = EventFactory::addSubscription("fsevents", subscription);
  EXPECT_TRUE(status.ok());
  EventFactory::deregisterEventPublisher("fsevents");
}

TEST_F(FSEventsTests, test_fsevents_add_subscription_success) {
  auto event_pub = std::make_shared<FSEventsEventPublisher>();
  EventFactory::registerEventPublisher(event_pub);

  // This subscription path *should* be real.
  auto mc = std::make_shared<FSEventsSubscriptionContext>();
  mc->path = "/";

  auto subscription = Subscription::create("TestSubscriber", mc);
  auto status = EventFactory::addSubscription("fsevents", subscription);
  EXPECT_TRUE(status.ok());
  event_pub->configure();

  // Make sure configure was called.
  size_t num_paths = event_pub->numSubscriptionedPaths();
  EXPECT_EQ(num_paths, 1U);

  // A duplicate subscription will work.
  auto mc_dup = std::make_shared<FSEventsSubscriptionContext>();
  mc_dup->path = "/";
  auto subscription_dup = Subscription::create("TestSubscriber", mc_dup);
  status = EventFactory::addSubscription("fsevents", subscription_dup);
  EXPECT_TRUE(status.ok());
  event_pub->configure();

  // But the paths with be deduped when the event type reconfigures.
  num_paths = event_pub->numSubscriptionedPaths();
  EXPECT_EQ(num_paths, 1U);
  EventFactory::deregisterEventPublisher("fsevents");
}

class TestFSEventsEventSubscriber
    : public EventSubscriber<FSEventsEventPublisher> {
 public:
  TestFSEventsEventSubscriber() : callback_count_(0) {
    setName("TestFSEventsEventSubscriber");
  }

  Status init() override {
    callback_count_ = 0;
    return Status(0, "OK");
  }

  SCRef GetSubscription(const std::string& path, uint32_t mask = 0) {
    auto sc = createSubscriptionContext();
    sc->path = path;
    sc->mask = mask;
    return sc;
  }

  Status SimpleCallback(const ECRef& ec, const SCRef& sc) {
    WriteLock lock(mutex_);
    callback_count_ += 1;
    return Status(0, "OK");
  }

  Status Callback(const ECRef& ec, const SCRef& sc) {
    // The following comments are an example Callback routine.
    // Row r;
    // r["action"] = ec->action;
    // r["path"] = ec->path;

    // Normally would call Add here.
    WriteLock lock(mutex_);
    actions_.push_back(ec->action);
    callback_count_ += 1;
    return Status(0, "OK");
  }

  void WaitForEvents(int max, int initial = 0) {
    int delay = 0;
    while (delay < max * 1000) {
      {
        WriteLock lock(mutex_);
        if (callback_count_ >= initial) {
          return;
        }
      }
      delay += 100;
      ::usleep(100);
    }
  }

 public:
  int callback_count_{0};
  std::vector<std::string> actions_;

 public:
  mutable Mutex mutex_;

 private:
  FRIEND_TEST(FSEventsTests, test_fsevents_fire_event);
  FRIEND_TEST(FSEventsTests, test_fsevents_event_action);
  FRIEND_TEST(FSEventsTests, test_fsevents_embedded_wildcards);
};

TEST_F(FSEventsTests, test_fsevents_run) {
  // Assume event type is registered.
  event_pub_ = std::make_shared<FSEventsEventPublisher>();
  EventFactory::registerEventPublisher(event_pub_);

  // Create a subscriber.
  auto sub = std::make_shared<TestFSEventsEventSubscriber>();
  EventFactory::registerEventSubscriber(sub);

  // Create a subscriptioning context
  auto mc = std::make_shared<FSEventsSubscriptionContext>();
  mc->path = real_test_path;
  EventFactory::addSubscription(
      "fsevents", Subscription::create("TestFSEventsEventSubscriber", mc));
  event_pub_->configure();

  // Create an event loop thread (similar to main)
  temp_thread_ = std::thread(EventFactory::run, "fsevents");
  EXPECT_TRUE(event_pub_->numEvents() == 0);

  // Wait for the thread to start and the FSEvents stream to turn on.
  WaitForStream(kMaxEventLatency);

  // Cause an fsevents event(s) by writing to the watched path.
  CreateEvents();

  // Wait for the thread's run loop to select.
  WaitForEvents(kMaxEventLatency);

  EXPECT_TRUE(event_pub_->numEvents() > 0);
  // We are managing the thread ourselves, so no join needed.
  EventFactory::end(false);
  temp_thread_.join();
}

TEST_F(FSEventsTests, test_fsevents_fire_event) {
  // Assume event type is registered.
  StartEventLoop();

  // Simulate registering an event subscriber.
  auto sub = std::make_shared<TestFSEventsEventSubscriber>();
  EventFactory::registerEventSubscriber(sub);

  // Create a subscriptioning context, note the added Event to the symbol
  auto sc = sub->GetSubscription(real_test_path, 0);
  sub->subscribe(&TestFSEventsEventSubscriber::SimpleCallback, sc);
  event_pub_->configure();
  CreateEvents();

  // This time wait for the callback.
  sub->WaitForEvents(kMaxEventLatency, 1);

  // Make sure our expected event fired (aka subscription callback was called).
  EXPECT_TRUE(sub->callback_count_ > 0);
  EndEventLoop();
}

TEST_F(FSEventsTests, test_fsevents_event_action) {
  // Assume event type is registered.
  StartEventLoop();

  // Simulate registering an event subscriber.
  auto sub = std::make_shared<TestFSEventsEventSubscriber>();
  auto status = sub->init();

  auto sc = sub->GetSubscription(real_test_path, 0);
  EventFactory::registerEventSubscriber(sub);

  sub->subscribe(&TestFSEventsEventSubscriber::Callback, sc);
  event_pub_->configure();

  CreateEvents();
  sub->WaitForEvents(kMaxEventLatency, 1);

  // Make sure the fsevents action was expected.
  ASSERT_TRUE(sub->actions_.size() > 0);
  bool has_created = false;
  bool has_unknown = false;
  {
    WriteLock lock(sub->mutex_);
    for (const auto& action : sub->actions_) {
      // Expect either a created event or attributes modified event.
      if (action == "CREATED" || action == "ATTRIBUTES_MODIFIED") {
        has_created = true;
      } else if (action == "UNKNOWN" || action == "") {
        // Allow an undetermined but existing FSevent on our target to pass.
        has_unknown = true;
      }
    }
  }
  EXPECT_TRUE(has_created || has_unknown);

  CreateEvents();
  sub->WaitForEvents(kMaxEventLatency, 2);
  bool has_updated = false;
  {
    WriteLock lock(sub->mutex_);
    // We may have triggered several updated events.
    for (const auto& action : sub->actions_) {
      if (action == "UPDATED") {
        has_updated = true;
      }
    }
  }
  EXPECT_TRUE(has_updated);

  EndEventLoop();
}

TEST_F(FSEventsTests, test_fsevents_embedded_wildcards) {
  // Assume event type is not registered.
  event_pub_ = std::make_shared<FSEventsEventPublisher>();
  EventFactory::registerEventPublisher(event_pub_);

  auto sub = std::make_shared<TestFSEventsEventSubscriber>();
  EventFactory::registerEventSubscriber(sub);

  // Create ./fsevents/2/1/
  fs::create_directories(real_test_dir + "/2/1");

  auto sc = sub->createSubscriptionContext();
  sc->path = real_test_dir + "/*/1";
  sub->subscribe(&TestFSEventsEventSubscriber::SimpleCallback, sc);

  // Now the publisher must be configured.
  event_pub_->configure();

  // The existing wild-card matched directory should be included.
  // This is populated via filesystem wildcard globbing.
  ASSERT_EQ(event_pub_->numSubscriptionedPaths(), 1U);
  std::set<std::string> expected = {real_test_dir + "/2/1/"};
  EXPECT_EQ(event_pub_->paths_, expected);
}
}
