/*
 *  Copyright (c) 2014-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include <boost/make_shared.hpp>

#include <gtest/gtest.h>

#include "osquery/dispatcher/dispatcher.h"

namespace osquery {

class DispatcherTests : public testing::Test {
  void TearDown() override {}
};

TEST_F(DispatcherTests, test_singleton) {
  auto& one = Dispatcher::instance();
  auto& two = Dispatcher::instance();
  EXPECT_EQ(&one, &two);
}

class TestRunnable : public InternalRunnable {
 public:
  int* i;
  explicit TestRunnable(int* i) : i(i) {}
  virtual void start() { ++*i; }
};
}
