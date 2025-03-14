#pragma once

#include "envoy/registry/registry.h"
#include "envoy/server/filter_config.h"
#include "envoy/stream_info/filter_state.h"

#include "source/common/router/string_accessor_impl.h"
#include "source/common/stream_info/uint32_accessor_impl.h"

#include "quiche/quic/core/quic_packets.h"

namespace Envoy {
/**
 * Test listener filter which sets the ALPN to a manually configured string.
 */
class TestListenerFilter : public Network::ListenerFilter {
public:
  TestListenerFilter() = default;

  // Network::ListenerFilter
  Network::FilterStatus onAccept(Network::ListenerFilterCallbacks& cb) override {
    absl::MutexLock m(&alpn_lock_);
    ASSERT(!alpn_.empty());
    cb.socket().setRequestedApplicationProtocols({alpn_});
    alpn_.clear();
    return Network::FilterStatus::Continue;
  }
  Network::FilterStatus onData(Network::ListenerFilterBuffer&) override {
    return Network::FilterStatus::Continue;
  }
  size_t maxReadBytes() const override { return 0; }

  static void setAlpn(std::string alpn) {
    absl::MutexLock m(&alpn_lock_);
    alpn_ = alpn;
  }

private:
  static absl::Mutex alpn_lock_;
  static std::string alpn_;
};

/**
 * Test TCP listener filter.
 */
class TestTcpListenerFilter : public Network::ListenerFilter {
public:
  TestTcpListenerFilter(const uint32_t drain_bytes) : drain_bytes_(drain_bytes) {}

  Network::FilterStatus onAccept(Network::ListenerFilterCallbacks&) override {
    return Network::FilterStatus::StopIteration;
  }

  Network::FilterStatus onData(Network::ListenerFilterBuffer& buffer) override {
    // Drain some bytes when onData.
    if (drain_bytes_ && drain_bytes_ <= buffer.rawSlice().len_) {
      buffer.drain(drain_bytes_);
    }
    return Network::FilterStatus::Continue;
  }

  // Returning a non-zero number.
  size_t maxReadBytes() const override { return 1024; }

private:
  const uint32_t drain_bytes_;
};

/**
 * Test UDP listener filter.
 */
class TestUdpListenerFilter : public Network::UdpListenerReadFilter {
public:
  TestUdpListenerFilter(Network::UdpReadFilterCallbacks& callbacks)
      : UdpListenerReadFilter(callbacks) {}

  // Network::UdpListenerReadFilter callbacks
  Network::FilterStatus onData(Network::UdpRecvData&) override {
    return Network::FilterStatus::Continue;
  }
  Network::FilterStatus onReceiveError(Api::IoError::IoErrorCode) override {
    return Network::FilterStatus::Continue;
  }
};

#ifdef ENVOY_ENABLE_QUIC

class TestFirstPacketReceivedFilterState : public StreamInfo::FilterState::Object {
public:
  explicit TestFirstPacketReceivedFilterState() = default;
  static const absl::string_view key() { return "test.filter_state.quic_first_packet_received"; }
  void incrementPacketCount() { packet_count_++; }
  void setPacketLength(size_t packet_length) { packet_length_ = packet_length; }
  void setPacketHeadersLength(size_t packet_headers_length) {
    packet_headers_length_ = packet_headers_length;
  }

  // FilterState::Object
  absl::optional<std::string> serializeAsString() const override {
    return absl::StrCat(packet_count_, ",", packet_length_, ",", packet_headers_length_);
  }

private:
  uint32_t packet_count_ = 0;
  size_t packet_length_ = 0;
  size_t packet_headers_length_ = 0;
};

/**
 * Test QUIC listener filter which add a new filter state.
 */
class TestQuicListenerFilter : public Network::QuicListenerFilter {
public:
  class TestStringFilterState : public Router::StringAccessorImpl {
  public:
    TestStringFilterState(std::string value) : Router::StringAccessorImpl(value) {}
    static const absl::string_view key() { return "test.filter_state.string"; }
  };

  explicit TestQuicListenerFilter(std::string added_value, bool allow_server_migration,
                                  bool allow_client_migration)
      : added_value_(added_value), allow_server_migration_(allow_server_migration),
        allow_client_migration_(allow_client_migration) {
    test_first_packet_received_filter_state_ =
        std::make_shared<TestFirstPacketReceivedFilterState>();
  }

  // Network::QuicListenerFilter
  Network::FilterStatus onAccept(Network::ListenerFilterCallbacks& cb) override {
    cb.filterState().setData(TestStringFilterState::key(),
                             std::make_unique<TestStringFilterState>(added_value_),
                             StreamInfo::FilterState::StateType::ReadOnly,
                             StreamInfo::FilterState::LifeSpan::Connection);
    cb.filterState().setData(
        TestFirstPacketReceivedFilterState::key(), test_first_packet_received_filter_state_,
        StreamInfo::FilterState::StateType::Mutable, StreamInfo::FilterState::LifeSpan::Connection);
    dispatcher_ = &cb.dispatcher();
    return Network::FilterStatus::Continue;
  }
  bool isCompatibleWithServerPreferredAddress(
      const quic::QuicSocketAddress& /*server_preferred_address*/) const override {
    return allow_server_migration_;
  }
  Network::FilterStatus onPeerAddressChanged(const quic::QuicSocketAddress& /*new_address*/,
                                             Network::Connection& connection) override {
    if (allow_client_migration_) {
      return Network::FilterStatus::Continue;
    }
    connection.close(Network::ConnectionCloseType::NoFlush,
                     "Migration to a new address which is not compatible with this filter.");
    return Network::FilterStatus::StopIteration;
  }
  Network::FilterStatus onFirstPacketReceived(const quic::QuicReceivedPacket& packet) override {
    test_first_packet_received_filter_state_->incrementPacketCount();
    test_first_packet_received_filter_state_->setPacketLength(packet.length());
    test_first_packet_received_filter_state_->setPacketHeadersLength(packet.headers_length());
    if (packet.headers_length() > 0 && packet.packet_headers() != nullptr) {
      char* headers_buffer = new char[packet.headers_length()];
      memcpy(headers_buffer, packet.packet_headers(), packet.headers_length());
    }
    return Network::FilterStatus::Continue;
  }

private:
  const std::string added_value_;
  const bool allow_server_migration_;
  Event::Dispatcher* dispatcher_{nullptr};
  const bool allow_client_migration_;
  std::shared_ptr<TestFirstPacketReceivedFilterState> test_first_packet_received_filter_state_;
};

#endif

} // namespace Envoy
