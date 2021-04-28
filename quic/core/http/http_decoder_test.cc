// Copyright (c) 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "quic/core/http/http_decoder.h"

#include <memory>
#include <utility>

#include "absl/base/macros.h"
#include "absl/strings/escaping.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "quic/core/http/http_encoder.h"
#include "quic/core/http/http_frames.h"
#include "quic/core/quic_data_writer.h"
#include "quic/core/quic_versions.h"
#include "quic/platform/api/quic_expect_bug.h"
#include "quic/platform/api/quic_flags.h"
#include "quic/platform/api/quic_test.h"
#include "quic/test_tools/quic_test_utils.h"

using ::testing::_;
using ::testing::AnyNumber;
using ::testing::Eq;
using ::testing::InSequence;
using ::testing::Return;

namespace quic {

namespace test {

class HttpDecoderPeer {
 public:
  static uint64_t current_frame_type(HttpDecoder* decoder) {
    return decoder->current_frame_type_;
  }
};

class MockVisitor : public HttpDecoder::Visitor {
 public:
  ~MockVisitor() override = default;

  // Called if an error is detected.
  MOCK_METHOD(void, OnError, (HttpDecoder*), (override));

  MOCK_METHOD(bool,
              OnCancelPushFrame,
              (const CancelPushFrame& frame),
              (override));
  MOCK_METHOD(bool,
              OnMaxPushIdFrame,
              (const MaxPushIdFrame& frame),
              (override));
  MOCK_METHOD(bool, OnGoAwayFrame, (const GoAwayFrame& frame), (override));
  MOCK_METHOD(bool,
              OnSettingsFrameStart,
              (QuicByteCount header_length),
              (override));
  MOCK_METHOD(bool, OnSettingsFrame, (const SettingsFrame& frame), (override));

  MOCK_METHOD(bool,
              OnDataFrameStart,
              (QuicByteCount header_length, QuicByteCount payload_length),
              (override));
  MOCK_METHOD(bool,
              OnDataFramePayload,
              (absl::string_view payload),
              (override));
  MOCK_METHOD(bool, OnDataFrameEnd, (), (override));

  MOCK_METHOD(bool,
              OnHeadersFrameStart,
              (QuicByteCount header_length, QuicByteCount payload_length),
              (override));
  MOCK_METHOD(bool,
              OnHeadersFramePayload,
              (absl::string_view payload),
              (override));
  MOCK_METHOD(bool, OnHeadersFrameEnd, (), (override));

  MOCK_METHOD(bool,
              OnPushPromiseFrameStart,
              (QuicByteCount header_length),
              (override));
  MOCK_METHOD(bool,
              OnPushPromiseFramePushId,
              (PushId push_id,
               QuicByteCount push_id_length,
               QuicByteCount header_block_length),
              (override));
  MOCK_METHOD(bool,
              OnPushPromiseFramePayload,
              (absl::string_view payload),
              (override));
  MOCK_METHOD(bool, OnPushPromiseFrameEnd, (), (override));

  MOCK_METHOD(bool,
              OnPriorityUpdateFrameStart,
              (QuicByteCount header_length),
              (override));
  MOCK_METHOD(bool,
              OnPriorityUpdateFrame,
              (const PriorityUpdateFrame& frame),
              (override));

  MOCK_METHOD(bool,
              OnAcceptChFrameStart,
              (QuicByteCount header_length),
              (override));
  MOCK_METHOD(bool, OnAcceptChFrame, (const AcceptChFrame& frame), (override));
  MOCK_METHOD(void,
              OnWebTransportStreamFrameType,
              (QuicByteCount header_length, WebTransportSessionId session_id),
              (override));

  MOCK_METHOD(bool,
              OnUnknownFrameStart,
              (uint64_t frame_type,
               QuicByteCount header_length,
               QuicByteCount payload_length),
              (override));
  MOCK_METHOD(bool,
              OnUnknownFramePayload,
              (absl::string_view payload),
              (override));
  MOCK_METHOD(bool, OnUnknownFrameEnd, (), (override));
};

class HttpDecoderTest : public QuicTest {
 public:
  HttpDecoderTest() : decoder_(&visitor_) {
    ON_CALL(visitor_, OnCancelPushFrame(_)).WillByDefault(Return(true));
    ON_CALL(visitor_, OnMaxPushIdFrame(_)).WillByDefault(Return(true));
    ON_CALL(visitor_, OnGoAwayFrame(_)).WillByDefault(Return(true));
    ON_CALL(visitor_, OnSettingsFrameStart(_)).WillByDefault(Return(true));
    ON_CALL(visitor_, OnSettingsFrame(_)).WillByDefault(Return(true));
    ON_CALL(visitor_, OnDataFrameStart(_, _)).WillByDefault(Return(true));
    ON_CALL(visitor_, OnDataFramePayload(_)).WillByDefault(Return(true));
    ON_CALL(visitor_, OnDataFrameEnd()).WillByDefault(Return(true));
    ON_CALL(visitor_, OnHeadersFrameStart(_, _)).WillByDefault(Return(true));
    ON_CALL(visitor_, OnHeadersFramePayload(_)).WillByDefault(Return(true));
    ON_CALL(visitor_, OnHeadersFrameEnd()).WillByDefault(Return(true));
    ON_CALL(visitor_, OnPushPromiseFrameStart(_)).WillByDefault(Return(true));
    ON_CALL(visitor_, OnPushPromiseFramePushId(_, _, _))
        .WillByDefault(Return(true));
    ON_CALL(visitor_, OnPushPromiseFramePayload(_)).WillByDefault(Return(true));
    ON_CALL(visitor_, OnPushPromiseFrameEnd()).WillByDefault(Return(true));
    ON_CALL(visitor_, OnPriorityUpdateFrameStart(_))
        .WillByDefault(Return(true));
    ON_CALL(visitor_, OnPriorityUpdateFrame(_)).WillByDefault(Return(true));
    ON_CALL(visitor_, OnAcceptChFrameStart(_)).WillByDefault(Return(true));
    ON_CALL(visitor_, OnAcceptChFrame(_)).WillByDefault(Return(true));
    ON_CALL(visitor_, OnUnknownFrameStart(_, _, _)).WillByDefault(Return(true));
    ON_CALL(visitor_, OnUnknownFramePayload(_)).WillByDefault(Return(true));
    ON_CALL(visitor_, OnUnknownFrameEnd()).WillByDefault(Return(true));
  }
  ~HttpDecoderTest() override = default;

  uint64_t current_frame_type() {
    return HttpDecoderPeer::current_frame_type(&decoder_);
  }

  // Process |input| in a single call to HttpDecoder::ProcessInput().
  QuicByteCount ProcessInput(absl::string_view input) {
    return decoder_.ProcessInput(input.data(), input.size());
  }

  // Feed |input| to |decoder_| one character at a time,
  // verifying that each character gets processed.
  void ProcessInputCharByChar(absl::string_view input) {
    for (char c : input) {
      EXPECT_EQ(1u, decoder_.ProcessInput(&c, 1));
    }
  }

  // Append garbage to |input|, then process it in a single call to
  // HttpDecoder::ProcessInput().  Verify that garbage is not read.
  QuicByteCount ProcessInputWithGarbageAppended(absl::string_view input) {
    std::string input_with_garbage_appended = absl::StrCat(input, "blahblah");
    QuicByteCount processed_bytes = ProcessInput(input_with_garbage_appended);

    // Guaranteed by HttpDecoder::ProcessInput() contract.
    QUICHE_DCHECK_LE(processed_bytes, input_with_garbage_appended.size());

    // Caller should set up visitor to pause decoding
    // before HttpDecoder would read garbage.
    EXPECT_LE(processed_bytes, input.size());

    return processed_bytes;
  }

  testing::StrictMock<MockVisitor> visitor_;
  HttpDecoder decoder_;
};

TEST_F(HttpDecoderTest, InitialState) {
  EXPECT_THAT(decoder_.error(), IsQuicNoError());
  EXPECT_EQ("", decoder_.error_detail());
}

TEST_F(HttpDecoderTest, UnknownFrame) {
  std::unique_ptr<char[]> input;

  const QuicByteCount payload_lengths[] = {0, 14, 100};
  const uint64_t frame_types[] = {
      0x21, 0x40, 0x5f, 0x7e, 0x9d,  // some reserved frame types
      0x6f, 0x14                     // some unknown, not reserved frame types
  };

  for (auto payload_length : payload_lengths) {
    std::string data(payload_length, 'a');

    for (auto frame_type : frame_types) {
      const QuicByteCount total_length =
          QuicDataWriter::GetVarInt62Len(frame_type) +
          QuicDataWriter::GetVarInt62Len(payload_length) + payload_length;
      input = std::make_unique<char[]>(total_length);

      QuicDataWriter writer(total_length, input.get());
      writer.WriteVarInt62(frame_type);
      writer.WriteVarInt62(payload_length);
      const QuicByteCount header_length = writer.length();
      if (payload_length > 0) {
        writer.WriteStringPiece(data);
      }

      EXPECT_CALL(visitor_, OnUnknownFrameStart(frame_type, header_length,
                                                payload_length));
      if (payload_length > 0) {
        EXPECT_CALL(visitor_, OnUnknownFramePayload(Eq(data)));
      }
      EXPECT_CALL(visitor_, OnUnknownFrameEnd());

      EXPECT_EQ(total_length, decoder_.ProcessInput(input.get(), total_length));

      EXPECT_THAT(decoder_.error(), IsQuicNoError());
      ASSERT_EQ("", decoder_.error_detail());
      EXPECT_EQ(frame_type, current_frame_type());
    }
  }
}

TEST_F(HttpDecoderTest, CancelPush) {
  InSequence s;
  std::string input = absl::HexStringToBytes(
      "03"    // type (CANCEL_PUSH)
      "01"    // length
      "01");  // Push Id

  if (GetQuicReloadableFlag(quic_error_on_http3_push)) {
    EXPECT_CALL(visitor_, OnError(&decoder_));
    EXPECT_EQ(1u, ProcessInput(input));
    EXPECT_THAT(decoder_.error(), IsError(QUIC_HTTP_FRAME_ERROR));
    EXPECT_EQ("CANCEL_PUSH frame received.", decoder_.error_detail());
    return;
  }

  // Visitor pauses processing.
  EXPECT_CALL(visitor_, OnCancelPushFrame(CancelPushFrame({1})))
      .WillOnce(Return(false));
  EXPECT_EQ(input.size(), ProcessInputWithGarbageAppended(input));
  EXPECT_THAT(decoder_.error(), IsQuicNoError());
  EXPECT_EQ("", decoder_.error_detail());

  // Process the full frame.
  EXPECT_CALL(visitor_, OnCancelPushFrame(CancelPushFrame({1})));
  EXPECT_EQ(input.size(), ProcessInput(input));
  EXPECT_THAT(decoder_.error(), IsQuicNoError());
  EXPECT_EQ("", decoder_.error_detail());

  // Process the frame incrementally.
  EXPECT_CALL(visitor_, OnCancelPushFrame(CancelPushFrame({1})));
  ProcessInputCharByChar(input);
  EXPECT_THAT(decoder_.error(), IsQuicNoError());
  EXPECT_EQ("", decoder_.error_detail());
}

TEST_F(HttpDecoderTest, PushPromiseFrame) {
  InSequence s;
  std::string input =
      absl::StrCat(absl::HexStringToBytes("05"  // type (PUSH PROMISE)
                                          "0f"  // length
                                          "C000000000000101"),  // push id 257
                   "Headers");                                  // headers

  if (GetQuicReloadableFlag(quic_error_on_http3_push)) {
    EXPECT_CALL(visitor_, OnError(&decoder_));
    EXPECT_EQ(1u, ProcessInput(input));
    EXPECT_THAT(decoder_.error(), IsError(QUIC_HTTP_FRAME_ERROR));
    EXPECT_EQ("PUSH_PROMISE frame received.", decoder_.error_detail());
    return;
  }

  // Visitor pauses processing.
  EXPECT_CALL(visitor_, OnPushPromiseFrameStart(2)).WillOnce(Return(false));
  EXPECT_CALL(visitor_, OnPushPromiseFramePushId(257, 8, 7))
      .WillOnce(Return(false));
  absl::string_view remaining_input(input);
  QuicByteCount processed_bytes =
      ProcessInputWithGarbageAppended(remaining_input);
  EXPECT_EQ(2u, processed_bytes);
  remaining_input = remaining_input.substr(processed_bytes);
  processed_bytes = ProcessInputWithGarbageAppended(remaining_input);
  EXPECT_EQ(8u, processed_bytes);
  remaining_input = remaining_input.substr(processed_bytes);

  EXPECT_CALL(visitor_, OnPushPromiseFramePayload(absl::string_view("Headers")))
      .WillOnce(Return(false));
  processed_bytes = ProcessInputWithGarbageAppended(remaining_input);
  EXPECT_EQ(remaining_input.size(), processed_bytes);

  EXPECT_CALL(visitor_, OnPushPromiseFrameEnd()).WillOnce(Return(false));
  EXPECT_EQ(0u, ProcessInputWithGarbageAppended(""));
  EXPECT_THAT(decoder_.error(), IsQuicNoError());
  EXPECT_EQ("", decoder_.error_detail());

  // Process the full frame.
  EXPECT_CALL(visitor_, OnPushPromiseFrameStart(2));
  EXPECT_CALL(visitor_, OnPushPromiseFramePushId(257, 8, 7));
  EXPECT_CALL(visitor_,
              OnPushPromiseFramePayload(absl::string_view("Headers")));
  EXPECT_CALL(visitor_, OnPushPromiseFrameEnd());
  EXPECT_EQ(input.size(), ProcessInput(input));
  EXPECT_THAT(decoder_.error(), IsQuicNoError());
  EXPECT_EQ("", decoder_.error_detail());

  // Process the frame incrementally.
  EXPECT_CALL(visitor_, OnPushPromiseFrameStart(2));
  EXPECT_CALL(visitor_, OnPushPromiseFramePushId(257, 8, 7));
  EXPECT_CALL(visitor_, OnPushPromiseFramePayload(absl::string_view("H")));
  EXPECT_CALL(visitor_, OnPushPromiseFramePayload(absl::string_view("e")));
  EXPECT_CALL(visitor_, OnPushPromiseFramePayload(absl::string_view("a")));
  EXPECT_CALL(visitor_, OnPushPromiseFramePayload(absl::string_view("d")));
  EXPECT_CALL(visitor_, OnPushPromiseFramePayload(absl::string_view("e")));
  EXPECT_CALL(visitor_, OnPushPromiseFramePayload(absl::string_view("r")));
  EXPECT_CALL(visitor_, OnPushPromiseFramePayload(absl::string_view("s")));
  EXPECT_CALL(visitor_, OnPushPromiseFrameEnd());
  ProcessInputCharByChar(input);
  EXPECT_THAT(decoder_.error(), IsQuicNoError());
  EXPECT_EQ("", decoder_.error_detail());

  // Process push id incrementally and append headers with last byte of push id.
  EXPECT_CALL(visitor_, OnPushPromiseFrameStart(2));
  EXPECT_CALL(visitor_, OnPushPromiseFramePushId(257, 8, 7));
  EXPECT_CALL(visitor_,
              OnPushPromiseFramePayload(absl::string_view("Headers")));
  EXPECT_CALL(visitor_, OnPushPromiseFrameEnd());
  ProcessInputCharByChar(input.substr(0, 9));
  EXPECT_EQ(8u, ProcessInput(input.substr(9)));
  EXPECT_THAT(decoder_.error(), IsQuicNoError());
  EXPECT_EQ("", decoder_.error_detail());
}

TEST_F(HttpDecoderTest, CorruptPushPromiseFrame) {
  if (GetQuicReloadableFlag(quic_error_on_http3_push)) {
    return;
  }

  InSequence s;

  std::string input = absl::HexStringToBytes(
      "05"    // type (PUSH_PROMISE)
      "01"    // length
      "40");  // first byte of two-byte varint push id

  {
    HttpDecoder decoder(&visitor_);
    EXPECT_CALL(visitor_, OnPushPromiseFrameStart(2));
    EXPECT_CALL(visitor_, OnError(&decoder));

    decoder.ProcessInput(input.data(), input.size());

    EXPECT_THAT(decoder.error(), IsError(QUIC_HTTP_FRAME_ERROR));
    EXPECT_EQ("Unable to read PUSH_PROMISE push_id.", decoder.error_detail());
  }
  {
    HttpDecoder decoder(&visitor_);
    EXPECT_CALL(visitor_, OnPushPromiseFrameStart(2));
    EXPECT_CALL(visitor_, OnError(&decoder));

    for (auto c : input) {
      decoder.ProcessInput(&c, 1);
    }

    EXPECT_THAT(decoder.error(), IsError(QUIC_HTTP_FRAME_ERROR));
    EXPECT_EQ("Unable to read PUSH_PROMISE push_id.", decoder.error_detail());
  }
}

TEST_F(HttpDecoderTest, MaxPushId) {
  InSequence s;
  std::string input = absl::HexStringToBytes(
      "0D"    // type (MAX_PUSH_ID)
      "01"    // length
      "01");  // Push Id

  // Visitor pauses processing.
  EXPECT_CALL(visitor_, OnMaxPushIdFrame(MaxPushIdFrame({1})))
      .WillOnce(Return(false));
  EXPECT_EQ(input.size(), ProcessInputWithGarbageAppended(input));
  EXPECT_THAT(decoder_.error(), IsQuicNoError());
  EXPECT_EQ("", decoder_.error_detail());

  // Process the full frame.
  EXPECT_CALL(visitor_, OnMaxPushIdFrame(MaxPushIdFrame({1})));
  EXPECT_EQ(input.size(), ProcessInput(input));
  EXPECT_THAT(decoder_.error(), IsQuicNoError());
  EXPECT_EQ("", decoder_.error_detail());

  // Process the frame incrementally.
  EXPECT_CALL(visitor_, OnMaxPushIdFrame(MaxPushIdFrame({1})));
  ProcessInputCharByChar(input);
  EXPECT_THAT(decoder_.error(), IsQuicNoError());
  EXPECT_EQ("", decoder_.error_detail());
}

TEST_F(HttpDecoderTest, SettingsFrame) {
  InSequence s;
  std::string input = absl::HexStringToBytes(
      "04"    // type (SETTINGS)
      "07"    // length
      "01"    // identifier (SETTINGS_QPACK_MAX_TABLE_CAPACITY)
      "02"    // content
      "06"    // identifier (SETTINGS_MAX_HEADER_LIST_SIZE)
      "05"    // content
      "4100"  // identifier, encoded on 2 bytes (0x40), value is 256 (0x100)
      "04");  // content

  SettingsFrame frame;
  frame.values[1] = 2;
  frame.values[6] = 5;
  frame.values[256] = 4;

  // Visitor pauses processing.
  absl::string_view remaining_input(input);
  EXPECT_CALL(visitor_, OnSettingsFrameStart(2)).WillOnce(Return(false));
  QuicByteCount processed_bytes =
      ProcessInputWithGarbageAppended(remaining_input);
  EXPECT_EQ(2u, processed_bytes);
  remaining_input = remaining_input.substr(processed_bytes);

  EXPECT_CALL(visitor_, OnSettingsFrame(frame)).WillOnce(Return(false));
  processed_bytes = ProcessInputWithGarbageAppended(remaining_input);
  EXPECT_EQ(remaining_input.size(), processed_bytes);
  EXPECT_THAT(decoder_.error(), IsQuicNoError());
  EXPECT_EQ("", decoder_.error_detail());

  // Process the full frame.
  EXPECT_CALL(visitor_, OnSettingsFrameStart(2));
  EXPECT_CALL(visitor_, OnSettingsFrame(frame));
  EXPECT_EQ(input.size(), ProcessInput(input));
  EXPECT_THAT(decoder_.error(), IsQuicNoError());
  EXPECT_EQ("", decoder_.error_detail());

  // Process the frame incrementally.
  EXPECT_CALL(visitor_, OnSettingsFrameStart(2));
  EXPECT_CALL(visitor_, OnSettingsFrame(frame));
  ProcessInputCharByChar(input);
  EXPECT_THAT(decoder_.error(), IsQuicNoError());
  EXPECT_EQ("", decoder_.error_detail());
}

TEST_F(HttpDecoderTest, CorruptSettingsFrame) {
  const char* const kPayload =
      "\x42\x11"                           // two-byte id
      "\x80\x22\x33\x44"                   // four-byte value
      "\x58\x39"                           // two-byte id
      "\xf0\x22\x33\x44\x55\x66\x77\x88";  // eight-byte value
  struct {
    size_t payload_length;
    const char* const error_message;
  } kTestData[] = {
      {1, "Unable to read setting identifier."},
      {5, "Unable to read setting value."},
      {7, "Unable to read setting identifier."},
      {12, "Unable to read setting value."},
  };

  for (const auto& test_data : kTestData) {
    std::string input;
    input.push_back(4u);  // type SETTINGS
    input.push_back(test_data.payload_length);
    const size_t header_length = input.size();
    input.append(kPayload, test_data.payload_length);

    HttpDecoder decoder(&visitor_);
    EXPECT_CALL(visitor_, OnSettingsFrameStart(header_length));
    EXPECT_CALL(visitor_, OnError(&decoder));

    QuicByteCount processed_bytes =
        decoder.ProcessInput(input.data(), input.size());
    EXPECT_EQ(input.size(), processed_bytes);
    EXPECT_THAT(decoder.error(), IsError(QUIC_HTTP_FRAME_ERROR));
    EXPECT_EQ(test_data.error_message, decoder.error_detail());
  }
}

TEST_F(HttpDecoderTest, DuplicateSettingsIdentifier) {
  std::string input = absl::HexStringToBytes(
      "04"    // type (SETTINGS)
      "04"    // length
      "01"    // identifier
      "01"    // content
      "01"    // identifier
      "02");  // content

  EXPECT_CALL(visitor_, OnSettingsFrameStart(2));
  EXPECT_CALL(visitor_, OnError(&decoder_));

  EXPECT_EQ(input.size(), ProcessInput(input));

  EXPECT_THAT(decoder_.error(),
              IsError(QUIC_HTTP_DUPLICATE_SETTING_IDENTIFIER));
  EXPECT_EQ("Duplicate setting identifier.", decoder_.error_detail());
}

TEST_F(HttpDecoderTest, DataFrame) {
  InSequence s;
  std::string input = absl::StrCat(absl::HexStringToBytes("00"    // type (DATA)
                                                          "05"),  // length
                                   "Data!");                      // data

  // Visitor pauses processing.
  EXPECT_CALL(visitor_, OnDataFrameStart(2, 5)).WillOnce(Return(false));
  absl::string_view remaining_input(input);
  QuicByteCount processed_bytes =
      ProcessInputWithGarbageAppended(remaining_input);
  EXPECT_EQ(2u, processed_bytes);
  remaining_input = remaining_input.substr(processed_bytes);

  EXPECT_CALL(visitor_, OnDataFramePayload(absl::string_view("Data!")))
      .WillOnce(Return(false));
  processed_bytes = ProcessInputWithGarbageAppended(remaining_input);
  EXPECT_EQ(remaining_input.size(), processed_bytes);

  EXPECT_CALL(visitor_, OnDataFrameEnd()).WillOnce(Return(false));
  EXPECT_EQ(0u, ProcessInputWithGarbageAppended(""));
  EXPECT_THAT(decoder_.error(), IsQuicNoError());
  EXPECT_EQ("", decoder_.error_detail());

  // Process the full frame.
  EXPECT_CALL(visitor_, OnDataFrameStart(2, 5));
  EXPECT_CALL(visitor_, OnDataFramePayload(absl::string_view("Data!")));
  EXPECT_CALL(visitor_, OnDataFrameEnd());
  EXPECT_EQ(input.size(), ProcessInput(input));
  EXPECT_THAT(decoder_.error(), IsQuicNoError());
  EXPECT_EQ("", decoder_.error_detail());

  // Process the frame incrementally.
  EXPECT_CALL(visitor_, OnDataFrameStart(2, 5));
  EXPECT_CALL(visitor_, OnDataFramePayload(absl::string_view("D")));
  EXPECT_CALL(visitor_, OnDataFramePayload(absl::string_view("a")));
  EXPECT_CALL(visitor_, OnDataFramePayload(absl::string_view("t")));
  EXPECT_CALL(visitor_, OnDataFramePayload(absl::string_view("a")));
  EXPECT_CALL(visitor_, OnDataFramePayload(absl::string_view("!")));
  EXPECT_CALL(visitor_, OnDataFrameEnd());
  ProcessInputCharByChar(input);
  EXPECT_THAT(decoder_.error(), IsQuicNoError());
  EXPECT_EQ("", decoder_.error_detail());
}

TEST_F(HttpDecoderTest, FrameHeaderPartialDelivery) {
  InSequence s;
  // A large input that will occupy more than 1 byte in the length field.
  std::string input(2048, 'x');
  std::unique_ptr<char[]> buffer;
  QuicByteCount header_length =
      HttpEncoder::SerializeDataFrameHeader(input.length(), &buffer);
  std::string header = std::string(buffer.get(), header_length);
  // Partially send only 1 byte of the header to process.
  EXPECT_EQ(1u, decoder_.ProcessInput(header.data(), 1));
  EXPECT_THAT(decoder_.error(), IsQuicNoError());
  EXPECT_EQ("", decoder_.error_detail());

  // Send the rest of the header.
  EXPECT_CALL(visitor_, OnDataFrameStart(3, input.length()));
  EXPECT_EQ(header_length - 1,
            decoder_.ProcessInput(header.data() + 1, header_length - 1));
  EXPECT_THAT(decoder_.error(), IsQuicNoError());
  EXPECT_EQ("", decoder_.error_detail());

  // Send data.
  EXPECT_CALL(visitor_, OnDataFramePayload(absl::string_view(input)));
  EXPECT_CALL(visitor_, OnDataFrameEnd());
  EXPECT_EQ(2048u, decoder_.ProcessInput(input.data(), 2048));
  EXPECT_THAT(decoder_.error(), IsQuicNoError());
  EXPECT_EQ("", decoder_.error_detail());
}

TEST_F(HttpDecoderTest, PartialDeliveryOfLargeFrameType) {
  // Use a reserved type that takes four bytes as a varint.
  const uint64_t frame_type = 0x1f * 0x222 + 0x21;
  const QuicByteCount payload_length = 0;
  const QuicByteCount header_length =
      QuicDataWriter::GetVarInt62Len(frame_type) +
      QuicDataWriter::GetVarInt62Len(payload_length);

  auto input = std::make_unique<char[]>(header_length);
  QuicDataWriter writer(header_length, input.get());
  writer.WriteVarInt62(frame_type);
  writer.WriteVarInt62(payload_length);

  EXPECT_CALL(visitor_,
              OnUnknownFrameStart(frame_type, header_length, payload_length));
  EXPECT_CALL(visitor_, OnUnknownFrameEnd());

  auto raw_input = input.get();
  for (uint64_t i = 0; i < header_length; ++i) {
    char c = raw_input[i];
    EXPECT_EQ(1u, decoder_.ProcessInput(&c, 1));
  }

  EXPECT_THAT(decoder_.error(), IsQuicNoError());
  EXPECT_EQ("", decoder_.error_detail());
  EXPECT_EQ(frame_type, current_frame_type());
}

TEST_F(HttpDecoderTest, GoAway) {
  InSequence s;
  std::string input = absl::HexStringToBytes(
      "07"    // type (GOAWAY)
      "01"    // length
      "01");  // ID

  // Visitor pauses processing.
  EXPECT_CALL(visitor_, OnGoAwayFrame(GoAwayFrame({1})))
      .WillOnce(Return(false));
  EXPECT_EQ(input.size(), ProcessInputWithGarbageAppended(input));
  EXPECT_THAT(decoder_.error(), IsQuicNoError());
  EXPECT_EQ("", decoder_.error_detail());

  // Process the full frame.
  EXPECT_CALL(visitor_, OnGoAwayFrame(GoAwayFrame({1})));
  EXPECT_EQ(input.size(), ProcessInput(input));
  EXPECT_THAT(decoder_.error(), IsQuicNoError());
  EXPECT_EQ("", decoder_.error_detail());

  // Process the frame incrementally.
  EXPECT_CALL(visitor_, OnGoAwayFrame(GoAwayFrame({1})));
  ProcessInputCharByChar(input);
  EXPECT_THAT(decoder_.error(), IsQuicNoError());
  EXPECT_EQ("", decoder_.error_detail());
}

TEST_F(HttpDecoderTest, HeadersFrame) {
  InSequence s;
  std::string input =
      absl::StrCat(absl::HexStringToBytes("01"    // type (HEADERS)
                                          "07"),  // length
                   "Headers");                    // headers

  // Visitor pauses processing.
  EXPECT_CALL(visitor_, OnHeadersFrameStart(2, 7)).WillOnce(Return(false));
  absl::string_view remaining_input(input);
  QuicByteCount processed_bytes =
      ProcessInputWithGarbageAppended(remaining_input);
  EXPECT_EQ(2u, processed_bytes);
  remaining_input = remaining_input.substr(processed_bytes);

  EXPECT_CALL(visitor_, OnHeadersFramePayload(absl::string_view("Headers")))
      .WillOnce(Return(false));
  processed_bytes = ProcessInputWithGarbageAppended(remaining_input);
  EXPECT_EQ(remaining_input.size(), processed_bytes);

  EXPECT_CALL(visitor_, OnHeadersFrameEnd()).WillOnce(Return(false));
  EXPECT_EQ(0u, ProcessInputWithGarbageAppended(""));
  EXPECT_THAT(decoder_.error(), IsQuicNoError());
  EXPECT_EQ("", decoder_.error_detail());

  // Process the full frame.
  EXPECT_CALL(visitor_, OnHeadersFrameStart(2, 7));
  EXPECT_CALL(visitor_, OnHeadersFramePayload(absl::string_view("Headers")));
  EXPECT_CALL(visitor_, OnHeadersFrameEnd());
  EXPECT_EQ(input.size(), ProcessInput(input));
  EXPECT_THAT(decoder_.error(), IsQuicNoError());
  EXPECT_EQ("", decoder_.error_detail());

  // Process the frame incrementally.
  EXPECT_CALL(visitor_, OnHeadersFrameStart(2, 7));
  EXPECT_CALL(visitor_, OnHeadersFramePayload(absl::string_view("H")));
  EXPECT_CALL(visitor_, OnHeadersFramePayload(absl::string_view("e")));
  EXPECT_CALL(visitor_, OnHeadersFramePayload(absl::string_view("a")));
  EXPECT_CALL(visitor_, OnHeadersFramePayload(absl::string_view("d")));
  EXPECT_CALL(visitor_, OnHeadersFramePayload(absl::string_view("e")));
  EXPECT_CALL(visitor_, OnHeadersFramePayload(absl::string_view("r")));
  EXPECT_CALL(visitor_, OnHeadersFramePayload(absl::string_view("s")));
  EXPECT_CALL(visitor_, OnHeadersFrameEnd());
  ProcessInputCharByChar(input);
  EXPECT_THAT(decoder_.error(), IsQuicNoError());
  EXPECT_EQ("", decoder_.error_detail());
}

TEST_F(HttpDecoderTest, EmptyDataFrame) {
  InSequence s;
  std::string input = absl::HexStringToBytes(
      "00"    // type (DATA)
      "00");  // length

  // Visitor pauses processing.
  EXPECT_CALL(visitor_, OnDataFrameStart(2, 0)).WillOnce(Return(false));
  EXPECT_EQ(input.size(), ProcessInputWithGarbageAppended(input));

  EXPECT_CALL(visitor_, OnDataFrameEnd()).WillOnce(Return(false));
  EXPECT_EQ(0u, ProcessInputWithGarbageAppended(""));
  EXPECT_THAT(decoder_.error(), IsQuicNoError());
  EXPECT_EQ("", decoder_.error_detail());

  // Process the full frame.
  EXPECT_CALL(visitor_, OnDataFrameStart(2, 0));
  EXPECT_CALL(visitor_, OnDataFrameEnd());
  EXPECT_EQ(input.size(), ProcessInput(input));
  EXPECT_THAT(decoder_.error(), IsQuicNoError());
  EXPECT_EQ("", decoder_.error_detail());

  // Process the frame incrementally.
  EXPECT_CALL(visitor_, OnDataFrameStart(2, 0));
  EXPECT_CALL(visitor_, OnDataFrameEnd());
  ProcessInputCharByChar(input);
  EXPECT_THAT(decoder_.error(), IsQuicNoError());
  EXPECT_EQ("", decoder_.error_detail());
}

TEST_F(HttpDecoderTest, EmptyHeadersFrame) {
  InSequence s;
  std::string input = absl::HexStringToBytes(
      "01"    // type (HEADERS)
      "00");  // length

  // Visitor pauses processing.
  EXPECT_CALL(visitor_, OnHeadersFrameStart(2, 0)).WillOnce(Return(false));
  EXPECT_EQ(input.size(), ProcessInputWithGarbageAppended(input));

  EXPECT_CALL(visitor_, OnHeadersFrameEnd()).WillOnce(Return(false));
  EXPECT_EQ(0u, ProcessInputWithGarbageAppended(""));
  EXPECT_THAT(decoder_.error(), IsQuicNoError());
  EXPECT_EQ("", decoder_.error_detail());

  // Process the full frame.
  EXPECT_CALL(visitor_, OnHeadersFrameStart(2, 0));
  EXPECT_CALL(visitor_, OnHeadersFrameEnd());
  EXPECT_EQ(input.size(), ProcessInput(input));
  EXPECT_THAT(decoder_.error(), IsQuicNoError());
  EXPECT_EQ("", decoder_.error_detail());

  // Process the frame incrementally.
  EXPECT_CALL(visitor_, OnHeadersFrameStart(2, 0));
  EXPECT_CALL(visitor_, OnHeadersFrameEnd());
  ProcessInputCharByChar(input);
  EXPECT_THAT(decoder_.error(), IsQuicNoError());
  EXPECT_EQ("", decoder_.error_detail());
}

TEST_F(HttpDecoderTest, PushPromiseFrameNoHeaders) {
  if (GetQuicReloadableFlag(quic_error_on_http3_push)) {
    return;
  }

  InSequence s;
  std::string input = absl::HexStringToBytes(
      "05"    // type (PUSH_PROMISE)
      "01"    // length
      "01");  // Push Id

  // Visitor pauses processing.
  EXPECT_CALL(visitor_, OnPushPromiseFrameStart(2));
  EXPECT_CALL(visitor_, OnPushPromiseFramePushId(1, 1, 0))
      .WillOnce(Return(false));
  EXPECT_EQ(input.size(), ProcessInputWithGarbageAppended(input));

  EXPECT_CALL(visitor_, OnPushPromiseFrameEnd()).WillOnce(Return(false));
  EXPECT_EQ(0u, ProcessInputWithGarbageAppended(""));
  EXPECT_THAT(decoder_.error(), IsQuicNoError());
  EXPECT_EQ("", decoder_.error_detail());

  // Process the full frame.
  EXPECT_CALL(visitor_, OnPushPromiseFrameStart(2));
  EXPECT_CALL(visitor_, OnPushPromiseFramePushId(1, 1, 0));
  EXPECT_CALL(visitor_, OnPushPromiseFrameEnd());
  EXPECT_EQ(input.size(), ProcessInput(input));
  EXPECT_THAT(decoder_.error(), IsQuicNoError());
  EXPECT_EQ("", decoder_.error_detail());

  // Process the frame incrementally.
  EXPECT_CALL(visitor_, OnPushPromiseFrameStart(2));
  EXPECT_CALL(visitor_, OnPushPromiseFramePushId(1, 1, 0));
  EXPECT_CALL(visitor_, OnPushPromiseFrameEnd());
  ProcessInputCharByChar(input);
  EXPECT_THAT(decoder_.error(), IsQuicNoError());
  EXPECT_EQ("", decoder_.error_detail());
}

TEST_F(HttpDecoderTest, MalformedFrameWithOverlyLargePayload) {
  if (GetQuicReloadableFlag(quic_error_on_http3_push)) {
    return;
  }

  std::string input = absl::HexStringToBytes(
      "03"    // type (CANCEL_PUSH)
      "10"    // length
      "15");  // malformed payload
  // Process the full frame.
  EXPECT_CALL(visitor_, OnError(&decoder_));
  EXPECT_EQ(2u, ProcessInput(input));
  EXPECT_THAT(decoder_.error(), IsError(QUIC_HTTP_FRAME_TOO_LARGE));
  EXPECT_EQ("Frame is too large.", decoder_.error_detail());
}

TEST_F(HttpDecoderTest, MalformedSettingsFrame) {
  char input[30];
  QuicDataWriter writer(30, input);
  // Write type SETTINGS.
  writer.WriteUInt8(0x04);
  // Write length.
  writer.WriteVarInt62(2048 * 1024);

  writer.WriteStringPiece("Malformed payload");
  EXPECT_CALL(visitor_, OnError(&decoder_));
  EXPECT_EQ(5u, decoder_.ProcessInput(input, ABSL_ARRAYSIZE(input)));
  EXPECT_THAT(decoder_.error(), IsError(QUIC_HTTP_FRAME_TOO_LARGE));
  EXPECT_EQ("Frame is too large.", decoder_.error_detail());
}

TEST_F(HttpDecoderTest, Http2Frame) {
  std::string input = absl::HexStringToBytes(
      "06"    // PING in HTTP/2 but not supported in HTTP/3.
      "05"    // length
      "15");  // random payload

  // Process the full frame.
  EXPECT_CALL(visitor_, OnError(&decoder_));
  EXPECT_EQ(1u, ProcessInput(input));
  EXPECT_THAT(decoder_.error(), IsError(QUIC_HTTP_RECEIVE_SPDY_FRAME));
  EXPECT_EQ("HTTP/2 frame received in a HTTP/3 connection: 6",
            decoder_.error_detail());
}

TEST_F(HttpDecoderTest, HeadersPausedThenData) {
  InSequence s;
  std::string input =
      absl::StrCat(absl::HexStringToBytes("01"    // type (HEADERS)
                                          "07"),  // length
                   "Headers",                     // headers
                   absl::HexStringToBytes("00"    // type (DATA)
                                          "05"),  // length
                   "Data!");                      // data

  // Visitor pauses processing, maybe because header decompression is blocked.
  EXPECT_CALL(visitor_, OnHeadersFrameStart(2, 7));
  EXPECT_CALL(visitor_, OnHeadersFramePayload(absl::string_view("Headers")));
  EXPECT_CALL(visitor_, OnHeadersFrameEnd()).WillOnce(Return(false));
  absl::string_view remaining_input(input);
  QuicByteCount processed_bytes =
      ProcessInputWithGarbageAppended(remaining_input);
  EXPECT_EQ(9u, processed_bytes);
  remaining_input = remaining_input.substr(processed_bytes);

  // Process DATA frame.
  EXPECT_CALL(visitor_, OnDataFrameStart(2, 5));
  EXPECT_CALL(visitor_, OnDataFramePayload(absl::string_view("Data!")));
  EXPECT_CALL(visitor_, OnDataFrameEnd());

  processed_bytes = ProcessInput(remaining_input);
  EXPECT_EQ(remaining_input.size(), processed_bytes);

  EXPECT_THAT(decoder_.error(), IsQuicNoError());
  EXPECT_EQ("", decoder_.error_detail());
}

TEST_F(HttpDecoderTest, CorruptFrame) {
  if (GetQuicReloadableFlag(quic_error_on_http3_push)) {
    InSequence s;

    struct {
      const char* const input;
      const char* const error_message;
    } kTestData[] = {{"\x0D"   // type (MAX_PUSH_ID)
                      "\x01"   // length
                      "\x40",  // first byte of two-byte varint push id
                      "Unable to read MAX_PUSH_ID push_id."},
                     {"\x0D"  // type (MAX_PUSH_ID)
                      "\x04"  // length
                      "\x05"  // valid push id
                      "foo",  // superfluous data
                      "Superfluous data in MAX_PUSH_ID frame."},
                     {"\x07"   // type (GOAWAY)
                      "\x01"   // length
                      "\x40",  // first byte of two-byte varint stream id
                      "Unable to read GOAWAY ID."},
                     {"\x07"  // type (GOAWAY)
                      "\x04"  // length
                      "\x05"  // valid stream id
                      "foo",  // superfluous data
                      "Superfluous data in GOAWAY frame."},
                     {"\x40\x89"  // type (ACCEPT_CH)
                      "\x01"      // length
                      "\x40",     // first byte of two-byte varint origin length
                      "Unable to read ACCEPT_CH origin."},
                     {"\x40\x89"  // type (ACCEPT_CH)
                      "\x01"      // length
                      "\x05",     // valid origin length but no origin string
                      "Unable to read ACCEPT_CH origin."},
                     {"\x40\x89"  // type (ACCEPT_CH)
                      "\x04"      // length
                      "\x05"      // valid origin length
                      "foo",      // payload ends before origin ends
                      "Unable to read ACCEPT_CH origin."},
                     {"\x40\x89"  // type (ACCEPT_CH)
                      "\x04"      // length
                      "\x03"      // valid origin length
                      "foo",      // payload ends at end of origin: no value
                      "Unable to read ACCEPT_CH value."},
                     {"\x40\x89"  // type (ACCEPT_CH)
                      "\x05"      // length
                      "\x03"      // valid origin length
                      "foo"       // payload ends at end of origin: no value
                      "\x40",     // first byte of two-byte varint value length
                      "Unable to read ACCEPT_CH value."},
                     {"\x40\x89"  // type (ACCEPT_CH)
                      "\x08"      // length
                      "\x03"      // valid origin length
                      "foo"       // origin
                      "\x05"      // valid value length
                      "bar",      // payload ends before value ends
                      "Unable to read ACCEPT_CH value."}};

    for (const auto& test_data : kTestData) {
      {
        HttpDecoder decoder(&visitor_);
        EXPECT_CALL(visitor_, OnAcceptChFrameStart(_)).Times(AnyNumber());
        EXPECT_CALL(visitor_, OnError(&decoder));

        absl::string_view input(test_data.input);
        decoder.ProcessInput(input.data(), input.size());
        EXPECT_THAT(decoder.error(), IsError(QUIC_HTTP_FRAME_ERROR));
        EXPECT_EQ(test_data.error_message, decoder.error_detail());
      }
      {
        HttpDecoder decoder(&visitor_);
        EXPECT_CALL(visitor_, OnAcceptChFrameStart(_)).Times(AnyNumber());
        EXPECT_CALL(visitor_, OnError(&decoder));

        absl::string_view input(test_data.input);
        for (auto c : input) {
          decoder.ProcessInput(&c, 1);
        }
        EXPECT_THAT(decoder.error(), IsError(QUIC_HTTP_FRAME_ERROR));
        EXPECT_EQ(test_data.error_message, decoder.error_detail());
      }
    }
  } else {
    InSequence s;

    struct {
      const char* const input;
      const char* const error_message;
    } kTestData[] = {{"\x03"   // type (CANCEL_PUSH)
                      "\x01"   // length
                      "\x40",  // first byte of two-byte varint push id
                      "Unable to read CANCEL_PUSH push_id."},
                     {"\x03"  // type (CANCEL_PUSH)
                      "\x04"  // length
                      "\x05"  // valid push id
                      "foo",  // superfluous data
                      "Superfluous data in CANCEL_PUSH frame."},
                     {"\x0D"   // type (MAX_PUSH_ID)
                      "\x01"   // length
                      "\x40",  // first byte of two-byte varint push id
                      "Unable to read MAX_PUSH_ID push_id."},
                     {"\x0D"  // type (MAX_PUSH_ID)
                      "\x04"  // length
                      "\x05"  // valid push id
                      "foo",  // superfluous data
                      "Superfluous data in MAX_PUSH_ID frame."},
                     {"\x07"   // type (GOAWAY)
                      "\x01"   // length
                      "\x40",  // first byte of two-byte varint stream id
                      "Unable to read GOAWAY ID."},
                     {"\x07"  // type (GOAWAY)
                      "\x04"  // length
                      "\x05"  // valid stream id
                      "foo",  // superfluous data
                      "Superfluous data in GOAWAY frame."},
                     {"\x40\x89"  // type (ACCEPT_CH)
                      "\x01"      // length
                      "\x40",     // first byte of two-byte varint origin length
                      "Unable to read ACCEPT_CH origin."},
                     {"\x40\x89"  // type (ACCEPT_CH)
                      "\x01"      // length
                      "\x05",     // valid origin length but no origin string
                      "Unable to read ACCEPT_CH origin."},
                     {"\x40\x89"  // type (ACCEPT_CH)
                      "\x04"      // length
                      "\x05"      // valid origin length
                      "foo",      // payload ends before origin ends
                      "Unable to read ACCEPT_CH origin."},
                     {"\x40\x89"  // type (ACCEPT_CH)
                      "\x04"      // length
                      "\x03"      // valid origin length
                      "foo",      // payload ends at end of origin: no value
                      "Unable to read ACCEPT_CH value."},
                     {"\x40\x89"  // type (ACCEPT_CH)
                      "\x05"      // length
                      "\x03"      // valid origin length
                      "foo"       // payload ends at end of origin: no value
                      "\x40",     // first byte of two-byte varint value length
                      "Unable to read ACCEPT_CH value."},
                     {"\x40\x89"  // type (ACCEPT_CH)
                      "\x08"      // length
                      "\x03"      // valid origin length
                      "foo"       // origin
                      "\x05"      // valid value length
                      "bar",      // payload ends before value ends
                      "Unable to read ACCEPT_CH value."}};

    for (const auto& test_data : kTestData) {
      {
        HttpDecoder decoder(&visitor_);
        EXPECT_CALL(visitor_, OnAcceptChFrameStart(_)).Times(AnyNumber());
        EXPECT_CALL(visitor_, OnError(&decoder));

        absl::string_view input(test_data.input);
        decoder.ProcessInput(input.data(), input.size());
        EXPECT_THAT(decoder.error(), IsError(QUIC_HTTP_FRAME_ERROR));
        EXPECT_EQ(test_data.error_message, decoder.error_detail());
      }
      {
        HttpDecoder decoder(&visitor_);
        EXPECT_CALL(visitor_, OnAcceptChFrameStart(_)).Times(AnyNumber());
        EXPECT_CALL(visitor_, OnError(&decoder));

        absl::string_view input(test_data.input);
        for (auto c : input) {
          decoder.ProcessInput(&c, 1);
        }
        EXPECT_THAT(decoder.error(), IsError(QUIC_HTTP_FRAME_ERROR));
        EXPECT_EQ(test_data.error_message, decoder.error_detail());
      }
    }
  }
}

TEST_F(HttpDecoderTest, EmptyCancelPushFrame) {
  if (GetQuicReloadableFlag(quic_error_on_http3_push)) {
    return;
  }

  std::string input = absl::HexStringToBytes(
      "03"    // type (CANCEL_PUSH)
      "00");  // frame length

  EXPECT_CALL(visitor_, OnError(&decoder_));
  EXPECT_EQ(input.size(), ProcessInput(input));
  EXPECT_THAT(decoder_.error(), IsError(QUIC_HTTP_FRAME_ERROR));
  EXPECT_EQ("Unable to read CANCEL_PUSH push_id.", decoder_.error_detail());
}

TEST_F(HttpDecoderTest, EmptySettingsFrame) {
  std::string input = absl::HexStringToBytes(
      "04"    // type (SETTINGS)
      "00");  // frame length

  EXPECT_CALL(visitor_, OnSettingsFrameStart(2));

  SettingsFrame empty_frame;
  EXPECT_CALL(visitor_, OnSettingsFrame(empty_frame));

  EXPECT_EQ(input.size(), ProcessInput(input));
  EXPECT_THAT(decoder_.error(), IsQuicNoError());
  EXPECT_EQ("", decoder_.error_detail());
}

// Regression test for https://crbug.com/1001823.
TEST_F(HttpDecoderTest, EmptyPushPromiseFrame) {
  if (GetQuicReloadableFlag(quic_error_on_http3_push)) {
    return;
  }

  std::string input = absl::HexStringToBytes(
      "05"    // type (PUSH_PROMISE)
      "00");  // frame length

  EXPECT_CALL(visitor_, OnError(&decoder_));
  EXPECT_EQ(input.size(), ProcessInput(input));
  EXPECT_THAT(decoder_.error(), IsError(QUIC_HTTP_FRAME_ERROR));
  EXPECT_EQ("PUSH_PROMISE frame with empty payload.", decoder_.error_detail());
}

TEST_F(HttpDecoderTest, EmptyGoAwayFrame) {
  std::string input = absl::HexStringToBytes(
      "07"    // type (GOAWAY)
      "00");  // frame length

  EXPECT_CALL(visitor_, OnError(&decoder_));
  EXPECT_EQ(input.size(), ProcessInput(input));
  EXPECT_THAT(decoder_.error(), IsError(QUIC_HTTP_FRAME_ERROR));
  EXPECT_EQ("Unable to read GOAWAY ID.", decoder_.error_detail());
}

TEST_F(HttpDecoderTest, EmptyMaxPushIdFrame) {
  std::string input = absl::HexStringToBytes(
      "0d"    // type (MAX_PUSH_ID)
      "00");  // frame length

  EXPECT_CALL(visitor_, OnError(&decoder_));
  EXPECT_EQ(input.size(), ProcessInput(input));
  EXPECT_THAT(decoder_.error(), IsError(QUIC_HTTP_FRAME_ERROR));
  EXPECT_EQ("Unable to read MAX_PUSH_ID push_id.", decoder_.error_detail());
}

TEST_F(HttpDecoderTest, LargeStreamIdInGoAway) {
  GoAwayFrame frame;
  frame.id = 1ull << 60;
  std::unique_ptr<char[]> buffer;
  uint64_t length = HttpEncoder::SerializeGoAwayFrame(frame, &buffer);
  EXPECT_CALL(visitor_, OnGoAwayFrame(frame));
  EXPECT_GT(length, 0u);
  EXPECT_EQ(length, decoder_.ProcessInput(buffer.get(), length));
  EXPECT_THAT(decoder_.error(), IsQuicNoError());
  EXPECT_EQ("", decoder_.error_detail());
}

TEST_F(HttpDecoderTest, OldPriorityUpdateFrame) {
  if (GetQuicReloadableFlag(quic_ignore_old_priority_update_frame)) {
    return;
  }

  InSequence s;
  std::string input1 = absl::HexStringToBytes(
      "0f"    // type (PRIORITY_UPDATE)
      "02"    // length
      "00"    // prioritized element type: REQUEST_STREAM
      "03");  // prioritized element id

  PriorityUpdateFrame priority_update1;
  priority_update1.prioritized_element_type = REQUEST_STREAM;
  priority_update1.prioritized_element_id = 0x03;

  // Visitor pauses processing.
  EXPECT_CALL(visitor_, OnPriorityUpdateFrameStart(2)).WillOnce(Return(false));
  absl::string_view remaining_input(input1);
  QuicByteCount processed_bytes =
      ProcessInputWithGarbageAppended(remaining_input);
  EXPECT_EQ(2u, processed_bytes);
  remaining_input = remaining_input.substr(processed_bytes);

  EXPECT_CALL(visitor_, OnPriorityUpdateFrame(priority_update1))
      .WillOnce(Return(false));
  processed_bytes = ProcessInputWithGarbageAppended(remaining_input);
  EXPECT_EQ(remaining_input.size(), processed_bytes);
  EXPECT_THAT(decoder_.error(), IsQuicNoError());
  EXPECT_EQ("", decoder_.error_detail());

  // Process the full frame.
  EXPECT_CALL(visitor_, OnPriorityUpdateFrameStart(2));
  EXPECT_CALL(visitor_, OnPriorityUpdateFrame(priority_update1));
  EXPECT_EQ(input1.size(), ProcessInput(input1));
  EXPECT_THAT(decoder_.error(), IsQuicNoError());
  EXPECT_EQ("", decoder_.error_detail());

  // Process the frame incrementally.
  EXPECT_CALL(visitor_, OnPriorityUpdateFrameStart(2));
  EXPECT_CALL(visitor_, OnPriorityUpdateFrame(priority_update1));
  ProcessInputCharByChar(input1);
  EXPECT_THAT(decoder_.error(), IsQuicNoError());
  EXPECT_EQ("", decoder_.error_detail());

  std::string input2 = absl::HexStringToBytes(
      "0f"        // type (PRIORITY_UPDATE)
      "05"        // length
      "80"        // prioritized element type: PUSH_STREAM
      "05"        // prioritized element id
      "666f6f");  // priority field value: "foo"

  PriorityUpdateFrame priority_update2;
  priority_update2.prioritized_element_type = PUSH_STREAM;
  priority_update2.prioritized_element_id = 0x05;
  priority_update2.priority_field_value = "foo";

  // Visitor pauses processing.
  EXPECT_CALL(visitor_, OnPriorityUpdateFrameStart(2)).WillOnce(Return(false));
  remaining_input = input2;
  processed_bytes = ProcessInputWithGarbageAppended(remaining_input);
  EXPECT_EQ(2u, processed_bytes);
  remaining_input = remaining_input.substr(processed_bytes);

  EXPECT_CALL(visitor_, OnPriorityUpdateFrame(priority_update2))
      .WillOnce(Return(false));
  processed_bytes = ProcessInputWithGarbageAppended(remaining_input);
  EXPECT_EQ(remaining_input.size(), processed_bytes);
  EXPECT_THAT(decoder_.error(), IsQuicNoError());
  EXPECT_EQ("", decoder_.error_detail());

  // Process the full frame.
  EXPECT_CALL(visitor_, OnPriorityUpdateFrameStart(2));
  EXPECT_CALL(visitor_, OnPriorityUpdateFrame(priority_update2));
  EXPECT_EQ(input2.size(), ProcessInput(input2));
  EXPECT_THAT(decoder_.error(), IsQuicNoError());
  EXPECT_EQ("", decoder_.error_detail());

  // Process the frame incrementally.
  EXPECT_CALL(visitor_, OnPriorityUpdateFrameStart(2));
  EXPECT_CALL(visitor_, OnPriorityUpdateFrame(priority_update2));
  ProcessInputCharByChar(input2);
  EXPECT_THAT(decoder_.error(), IsQuicNoError());
  EXPECT_EQ("", decoder_.error_detail());
}

TEST_F(HttpDecoderTest, ObsoletePriorityUpdateFrame) {
  if (!GetQuicReloadableFlag(quic_ignore_old_priority_update_frame)) {
    return;
  }

  const QuicByteCount header_length = 2;
  const QuicByteCount payload_length = 3;
  InSequence s;
  std::string input = absl::HexStringToBytes(
      "0f"        // type (obsolete PRIORITY_UPDATE)
      "03"        // length
      "666f6f");  // payload "foo"

  // Process frame as a whole.
  EXPECT_CALL(visitor_,
              OnUnknownFrameStart(0x0f, header_length, payload_length));
  EXPECT_CALL(visitor_, OnUnknownFramePayload(Eq("foo")));
  EXPECT_CALL(visitor_, OnUnknownFrameEnd()).WillOnce(Return(false));

  EXPECT_EQ(header_length + payload_length,
            ProcessInputWithGarbageAppended(input));
  EXPECT_THAT(decoder_.error(), IsQuicNoError());
  EXPECT_EQ("", decoder_.error_detail());

  // Process frame byte by byte.
  EXPECT_CALL(visitor_,
              OnUnknownFrameStart(0x0f, header_length, payload_length));
  EXPECT_CALL(visitor_, OnUnknownFramePayload(Eq("f")));
  EXPECT_CALL(visitor_, OnUnknownFramePayload(Eq("o")));
  EXPECT_CALL(visitor_, OnUnknownFramePayload(Eq("o")));
  EXPECT_CALL(visitor_, OnUnknownFrameEnd());

  ProcessInputCharByChar(input);
  EXPECT_THAT(decoder_.error(), IsQuicNoError());
  EXPECT_EQ("", decoder_.error_detail());
}

TEST_F(HttpDecoderTest, PriorityUpdateFrame) {
  InSequence s;
  std::string input1 = absl::HexStringToBytes(
      "800f0700"  // type (PRIORITY_UPDATE)
      "01"        // length
      "03");      // prioritized element id

  PriorityUpdateFrame priority_update1;
  priority_update1.prioritized_element_type = REQUEST_STREAM;
  priority_update1.prioritized_element_id = 0x03;

  // Visitor pauses processing.
  EXPECT_CALL(visitor_, OnPriorityUpdateFrameStart(5)).WillOnce(Return(false));
  absl::string_view remaining_input(input1);
  QuicByteCount processed_bytes =
      ProcessInputWithGarbageAppended(remaining_input);
  EXPECT_EQ(5u, processed_bytes);
  remaining_input = remaining_input.substr(processed_bytes);

  EXPECT_CALL(visitor_, OnPriorityUpdateFrame(priority_update1))
      .WillOnce(Return(false));
  processed_bytes = ProcessInputWithGarbageAppended(remaining_input);
  EXPECT_EQ(remaining_input.size(), processed_bytes);
  EXPECT_THAT(decoder_.error(), IsQuicNoError());
  EXPECT_EQ("", decoder_.error_detail());

  // Process the full frame.
  EXPECT_CALL(visitor_, OnPriorityUpdateFrameStart(5));
  EXPECT_CALL(visitor_, OnPriorityUpdateFrame(priority_update1));
  EXPECT_EQ(input1.size(), ProcessInput(input1));
  EXPECT_THAT(decoder_.error(), IsQuicNoError());
  EXPECT_EQ("", decoder_.error_detail());

  // Process the frame incrementally.
  EXPECT_CALL(visitor_, OnPriorityUpdateFrameStart(5));
  EXPECT_CALL(visitor_, OnPriorityUpdateFrame(priority_update1));
  ProcessInputCharByChar(input1);
  EXPECT_THAT(decoder_.error(), IsQuicNoError());
  EXPECT_EQ("", decoder_.error_detail());

  std::string input2 = absl::HexStringToBytes(
      "800f0700"  // type (PRIORITY_UPDATE)
      "04"        // length
      "05"        // prioritized element id
      "666f6f");  // priority field value: "foo"

  PriorityUpdateFrame priority_update2;
  priority_update2.prioritized_element_type = REQUEST_STREAM;
  priority_update2.prioritized_element_id = 0x05;
  priority_update2.priority_field_value = "foo";

  // Visitor pauses processing.
  EXPECT_CALL(visitor_, OnPriorityUpdateFrameStart(5)).WillOnce(Return(false));
  remaining_input = input2;
  processed_bytes = ProcessInputWithGarbageAppended(remaining_input);
  EXPECT_EQ(5u, processed_bytes);
  remaining_input = remaining_input.substr(processed_bytes);

  EXPECT_CALL(visitor_, OnPriorityUpdateFrame(priority_update2))
      .WillOnce(Return(false));
  processed_bytes = ProcessInputWithGarbageAppended(remaining_input);
  EXPECT_EQ(remaining_input.size(), processed_bytes);
  EXPECT_THAT(decoder_.error(), IsQuicNoError());
  EXPECT_EQ("", decoder_.error_detail());

  // Process the full frame.
  EXPECT_CALL(visitor_, OnPriorityUpdateFrameStart(5));
  EXPECT_CALL(visitor_, OnPriorityUpdateFrame(priority_update2));
  EXPECT_EQ(input2.size(), ProcessInput(input2));
  EXPECT_THAT(decoder_.error(), IsQuicNoError());
  EXPECT_EQ("", decoder_.error_detail());

  // Process the frame incrementally.
  EXPECT_CALL(visitor_, OnPriorityUpdateFrameStart(5));
  EXPECT_CALL(visitor_, OnPriorityUpdateFrame(priority_update2));
  ProcessInputCharByChar(input2);
  EXPECT_THAT(decoder_.error(), IsQuicNoError());
  EXPECT_EQ("", decoder_.error_detail());
}

TEST_F(HttpDecoderTest, CorruptPriorityUpdateFrame) {
  if (GetQuicReloadableFlag(quic_ignore_old_priority_update_frame)) {
    return;
  }

  std::string payload1 = absl::HexStringToBytes(
      "80"      // prioritized element type: PUSH_STREAM
      "4005");  // prioritized element id
  std::string payload2 =
      absl::HexStringToBytes("42");  // invalid prioritized element type
  struct {
    const char* const payload;
    size_t payload_length;
    const char* const error_message;
  } kTestData[] = {
      {payload1.data(), 0, "Unable to read prioritized element type."},
      {payload1.data(), 1, "Unable to read prioritized element id."},
      {payload1.data(), 2, "Unable to read prioritized element id."},
      {payload2.data(), 1, "Invalid prioritized element type."},
  };

  for (const auto& test_data : kTestData) {
    std::string input;
    input.push_back(15u);  // type PRIORITY_UPDATE
    input.push_back(test_data.payload_length);
    size_t header_length = input.size();
    input.append(test_data.payload, test_data.payload_length);

    HttpDecoder decoder(&visitor_);
    EXPECT_CALL(visitor_, OnPriorityUpdateFrameStart(header_length));
    EXPECT_CALL(visitor_, OnError(&decoder));

    QuicByteCount processed_bytes =
        decoder.ProcessInput(input.data(), input.size());
    EXPECT_EQ(input.size(), processed_bytes);
    EXPECT_THAT(decoder.error(), IsError(QUIC_HTTP_FRAME_ERROR));
    EXPECT_EQ(test_data.error_message, decoder.error_detail());
  }
}

TEST_F(HttpDecoderTest, CorruptNewPriorityUpdateFrame) {
  std::string payload =
      absl::HexStringToBytes("4005");  // prioritized element id
  struct {
    size_t payload_length;
    const char* const error_message;
  } kTestData[] = {
      {0, "Unable to read prioritized element id."},
      {1, "Unable to read prioritized element id."},
  };

  for (const auto& test_data : kTestData) {
    std::string input =
        absl::HexStringToBytes("800f0700");  // type PRIORITY_UPDATE
    input.push_back(test_data.payload_length);
    size_t header_length = input.size();
    input.append(payload.data(), test_data.payload_length);

    HttpDecoder decoder(&visitor_);
    EXPECT_CALL(visitor_, OnPriorityUpdateFrameStart(header_length));
    EXPECT_CALL(visitor_, OnError(&decoder));

    QuicByteCount processed_bytes =
        decoder.ProcessInput(input.data(), input.size());
    EXPECT_EQ(input.size(), processed_bytes);
    EXPECT_THAT(decoder.error(), IsError(QUIC_HTTP_FRAME_ERROR));
    EXPECT_EQ(test_data.error_message, decoder.error_detail());
  }
}

TEST_F(HttpDecoderTest, AcceptChFrame) {
  InSequence s;
  std::string input1 = absl::HexStringToBytes(
      "4089"  // type (ACCEPT_CH)
      "00");  // length

  AcceptChFrame accept_ch1;

  // Visitor pauses processing.
  EXPECT_CALL(visitor_, OnAcceptChFrameStart(3)).WillOnce(Return(false));
  absl::string_view remaining_input(input1);
  QuicByteCount processed_bytes =
      ProcessInputWithGarbageAppended(remaining_input);
  EXPECT_EQ(3u, processed_bytes);
  remaining_input = remaining_input.substr(processed_bytes);

  EXPECT_CALL(visitor_, OnAcceptChFrame(accept_ch1)).WillOnce(Return(false));
  processed_bytes = ProcessInputWithGarbageAppended(remaining_input);
  EXPECT_EQ(remaining_input.size(), processed_bytes);
  EXPECT_THAT(decoder_.error(), IsQuicNoError());
  EXPECT_EQ("", decoder_.error_detail());

  // Process the full frame.
  EXPECT_CALL(visitor_, OnAcceptChFrameStart(3));
  EXPECT_CALL(visitor_, OnAcceptChFrame(accept_ch1));
  EXPECT_EQ(input1.size(), ProcessInput(input1));
  EXPECT_THAT(decoder_.error(), IsQuicNoError());
  EXPECT_EQ("", decoder_.error_detail());

  // Process the frame incrementally.
  EXPECT_CALL(visitor_, OnAcceptChFrameStart(3));
  EXPECT_CALL(visitor_, OnAcceptChFrame(accept_ch1));
  ProcessInputCharByChar(input1);
  EXPECT_THAT(decoder_.error(), IsQuicNoError());
  EXPECT_EQ("", decoder_.error_detail());

  std::string input2 = absl::HexStringToBytes(
      "4089"      // type (ACCEPT_CH)
      "08"        // length
      "03"        // length of origin
      "666f6f"    // origin "foo"
      "03"        // length of value
      "626172");  // value "bar"

  AcceptChFrame accept_ch2;
  accept_ch2.entries.push_back({"foo", "bar"});

  // Visitor pauses processing.
  EXPECT_CALL(visitor_, OnAcceptChFrameStart(3)).WillOnce(Return(false));
  remaining_input = input2;
  processed_bytes = ProcessInputWithGarbageAppended(remaining_input);
  EXPECT_EQ(3u, processed_bytes);
  remaining_input = remaining_input.substr(processed_bytes);

  EXPECT_CALL(visitor_, OnAcceptChFrame(accept_ch2)).WillOnce(Return(false));
  processed_bytes = ProcessInputWithGarbageAppended(remaining_input);
  EXPECT_EQ(remaining_input.size(), processed_bytes);
  EXPECT_THAT(decoder_.error(), IsQuicNoError());
  EXPECT_EQ("", decoder_.error_detail());

  // Process the full frame.
  EXPECT_CALL(visitor_, OnAcceptChFrameStart(3));
  EXPECT_CALL(visitor_, OnAcceptChFrame(accept_ch2));
  EXPECT_EQ(input2.size(), ProcessInput(input2));
  EXPECT_THAT(decoder_.error(), IsQuicNoError());
  EXPECT_EQ("", decoder_.error_detail());

  // Process the frame incrementally.
  EXPECT_CALL(visitor_, OnAcceptChFrameStart(3));
  EXPECT_CALL(visitor_, OnAcceptChFrame(accept_ch2));
  ProcessInputCharByChar(input2);
  EXPECT_THAT(decoder_.error(), IsQuicNoError());
  EXPECT_EQ("", decoder_.error_detail());
}

TEST_F(HttpDecoderTest, WebTransportStreamDisabled) {
  InSequence s;

  // Unknown frame of type 0x41 and length 0x104.
  std::string input = absl::HexStringToBytes("40414104");
  EXPECT_CALL(visitor_, OnUnknownFrameStart(0x41, input.size(), 0x104));
  EXPECT_EQ(ProcessInput(input), input.size());
}

TEST(HttpDecoderTestNoFixture, WebTransportStream) {
  HttpDecoder::Options options;
  options.allow_web_transport_stream = true;
  testing::StrictMock<MockVisitor> visitor;
  HttpDecoder decoder(&visitor, options);

  // WebTransport stream for session ID 0x104, with four bytes of extra data.
  std::string input = absl::HexStringToBytes("40414104ffffffff");
  EXPECT_CALL(visitor, OnWebTransportStreamFrameType(4, 0x104));
  QuicByteCount bytes = decoder.ProcessInput(input.data(), input.size());
  EXPECT_EQ(bytes, 4u);
}

TEST(HttpDecoderTestNoFixture, WebTransportStreamError) {
  HttpDecoder::Options options;
  options.allow_web_transport_stream = true;
  testing::StrictMock<MockVisitor> visitor;
  HttpDecoder decoder(&visitor, options);

  std::string input = absl::HexStringToBytes("404100");
  EXPECT_CALL(visitor, OnWebTransportStreamFrameType(_, _));
  decoder.ProcessInput(input.data(), input.size());

  EXPECT_CALL(visitor, OnError(_));
  EXPECT_QUIC_BUG(decoder.ProcessInput(input.data(), input.size()),
                  "HttpDecoder called after an indefinite-length frame");
}

TEST_F(HttpDecoderTest, DecodeSettings) {
  std::string input = absl::HexStringToBytes(
      "04"    // type (SETTINGS)
      "07"    // length
      "01"    // identifier (SETTINGS_QPACK_MAX_TABLE_CAPACITY)
      "02"    // content
      "06"    // identifier (SETTINGS_MAX_HEADER_LIST_SIZE)
      "05"    // content
      "4100"  // identifier, encoded on 2 bytes (0x40), value is 256 (0x100)
      "04");  // content

  SettingsFrame frame;
  frame.values[1] = 2;
  frame.values[6] = 5;
  frame.values[256] = 4;

  SettingsFrame out;
  EXPECT_TRUE(HttpDecoder::DecodeSettings(input.data(), input.size(), &out));
  EXPECT_EQ(frame, out);

  // non-settings frame.
  input = absl::HexStringToBytes(
      "0D"    // type (MAX_PUSH_ID)
      "01"    // length
      "01");  // Push Id

  EXPECT_FALSE(HttpDecoder::DecodeSettings(input.data(), input.size(), &out));

  // Corrupt SETTINGS.
  input = absl::HexStringToBytes(
      "04"    // type (SETTINGS)
      "01"    // length
      "42");  // First byte of setting identifier, indicating a 2-byte varint62.

  EXPECT_FALSE(HttpDecoder::DecodeSettings(input.data(), input.size(), &out));
}

}  // namespace test

}  // namespace quic
