// Copyright 2023 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "quiche/quic/moqt/moqt_track.h"

#include <optional>

#include "absl/strings/string_view.h"
#include "quiche/quic/moqt/moqt_messages.h"
#include "quiche/quic/moqt/moqt_subscribe_windows.h"
#include "quiche/quic/moqt/tools/moqt_mock_visitor.h"
#include "quiche/quic/platform/api/quic_test.h"

namespace moqt {

namespace test {

class LocalTrackTest : public quic::test::QuicTest {
 public:
  LocalTrackTest()
      : track_(FullTrackName("foo", "bar"), &visitor_, FullSequence(4, 1)) {}
  LocalTrack track_;
  MockLocalTrackVisitor visitor_;
};

TEST_F(LocalTrackTest, Queries) {
  EXPECT_EQ(track_.full_track_name(), FullTrackName("foo", "bar"));
  EXPECT_EQ(track_.track_alias(), std::nullopt);
  EXPECT_EQ(track_.visitor(), &visitor_);
  EXPECT_EQ(track_.next_sequence(), FullSequence(4, 1));
  track_.SentSequence(FullSequence(4, 0));
  EXPECT_EQ(track_.next_sequence(), FullSequence(4, 1));  // no change
  track_.SentSequence(FullSequence(4, 1));
  EXPECT_EQ(track_.next_sequence(), FullSequence(4, 2));
  EXPECT_FALSE(track_.HasSubscriber());
}

TEST_F(LocalTrackTest, SetTrackAlias) {
  EXPECT_EQ(track_.track_alias(), std::nullopt);
  track_.set_track_alias(6);
  EXPECT_EQ(track_.track_alias(), 6);
}

TEST_F(LocalTrackTest, ShouldSend) {
  track_.AddWindow(SubscribeWindow(0, 4, 1));
  EXPECT_TRUE(track_.HasSubscriber());
  EXPECT_TRUE(track_.ShouldSend(FullSequence(3, 12)).empty());
  EXPECT_TRUE(track_.ShouldSend(FullSequence(4, 0)).empty());
  EXPECT_EQ(track_.ShouldSend(FullSequence(4, 1)).size(), 1);
  EXPECT_EQ(track_.ShouldSend(FullSequence(12, 0)).size(), 1);
}

class RemoteTrackTest : public quic::test::QuicTest {
 public:
  RemoteTrackTest()
      : track_(FullTrackName("foo", "bar"), /*track_alias=*/5, &visitor_) {}
  RemoteTrack track_;
  MockRemoteTrackVisitor visitor_;
};

TEST_F(RemoteTrackTest, Queries) {
  EXPECT_EQ(track_.full_track_name(), FullTrackName("foo", "bar"));
  EXPECT_EQ(track_.track_alias(), 5);
  EXPECT_EQ(track_.visitor(), &visitor_);
}

// TODO: Write test for GetStreamForSequence.

}  // namespace test

}  // namespace moqt
