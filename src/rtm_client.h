// RTM client interface definition.
#pragma once

#include <cbor.h>
#include <boost/asio.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/optional.hpp>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

#include "logging.h"

namespace satori {

namespace video {

namespace rtm {
enum class client_error : unsigned char {
  // 0 - not used, success.

  Unknown = 1,
  NotConnected = 2,
  ResponseParsingError = 3,
  InvalidResponse = 4,
  SubscriptionError = 5,
  SubscribeError = 6,
  UnsubscribeError = 7,
  AsioError = 8,
};

std::error_condition make_error_condition(client_error e);

struct error_callbacks {
  virtual ~error_callbacks() = default;
  virtual void on_error(std::error_condition ec) = 0;
};

struct channel_position {
  uint32_t gen{0};
  uint64_t pos{0};

  std::string str() const { return std::to_string(gen) + ":" + std::to_string(pos); }

  static channel_position parse(const std::string &str) {
    char *str_pos = nullptr;
    auto gen = strtoll(str.c_str(), &str_pos, 10);
    CHECK_LE(gen, std::numeric_limits<uint32_t>::max());
    if ((str_pos == nullptr) || str_pos == str.c_str() || *str_pos != ':') {
      return {0, 0};
    }
    auto pos = strtoull(str_pos + 1, &str_pos, 10);
    CHECK_LE(pos, std::numeric_limits<uint64_t>::max());
    if ((str_pos == nullptr) || (*str_pos != 0)) {
      return {0, 0};
    }
    return {static_cast<uint32_t>(gen), static_cast<uint64_t>(pos)};
  }
};

struct publish_callbacks : public error_callbacks {
  ~publish_callbacks() override = default;

  virtual void on_ok(const channel_position & /*position*/) {}
};

struct publisher {
  virtual ~publisher() = default;

  virtual void publish(const std::string &channel, const cbor_item_t *message,
                       publish_callbacks *callbacks = nullptr) = 0;
};

// Subscription interface of RTM.
struct subscription {};

struct subscription_callbacks : public error_callbacks {
  virtual void on_data(const subscription & /*subscription*/, cbor_item_t * /*unused*/) {}
};

struct history_options {
  boost::optional<uint64_t> count;
  boost::optional<uint64_t> age;
};

struct subscription_options {
  bool force{false};
  bool fast_forward{true};
  boost::optional<channel_position> position;
  history_options history;
};

struct subscriber {
  virtual ~subscriber() = default;

  virtual void subscribe_channel(const std::string &channel, const subscription &sub,
                                 subscription_callbacks &callbacks,
                                 const subscription_options *options = nullptr) = 0;

  virtual void subscribe_filter(const std::string &filter, const subscription &sub,
                                subscription_callbacks &callbacks,
                                const subscription_options *options = nullptr) = 0;

  virtual void unsubscribe(const subscription &sub) = 0;

  virtual channel_position position(const subscription &sub) = 0;

  virtual bool is_up(const subscription &sub) = 0;
};

class client : public publisher, public subscriber {
 public:
  virtual std::error_condition start() __attribute__((warn_unused_result)) = 0;
  virtual std::error_condition stop() __attribute__((warn_unused_result))  = 0;
};

std::unique_ptr<client> new_client(const std::string &endpoint, const std::string &port,
                                   const std::string &appkey,
                                   boost::asio::io_service &io_service,
                                   boost::asio::ssl::context &ssl_ctx, size_t id,
                                   error_callbacks &callbacks);

// reconnects on any error.
class resilient_client : public client, error_callbacks {
 public:
  using client_factory_t =
      std::function<std::unique_ptr<client>(error_callbacks &callbacks)>;

  explicit resilient_client(client_factory_t &&factory, error_callbacks &callbacks);

  void publish(const std::string &channel, const cbor_item_t *message,
               publish_callbacks *callbacks) override;

  void subscribe_channel(const std::string &channel, const subscription &sub,
                         subscription_callbacks &callbacks,
                         const subscription_options *options) override;

  void subscribe_filter(const std::string &filter, const subscription &sub,
                        subscription_callbacks &callbacks,
                        const subscription_options *options) override;

  void unsubscribe(const subscription &sub) override;

  channel_position position(const subscription &sub) override;

  bool is_up(const subscription &sub) override;

  std::error_condition start() override;

  std::error_condition stop() override;

 private:
  void on_error(std::error_condition ec) override;
  void restart();

  struct subscription_info {
    std::string channel;
    const subscription *sub;
    subscription_callbacks *callbacks;
    const subscription_options *options;
  };

  client_factory_t _factory;
  error_callbacks &_error_callbacks;
  std::unique_ptr<client> _client;
  std::mutex _client_mutex;
  bool _started{false};

  std::vector<subscription_info> _subscriptions;
};

}  // namespace rtm
}  // namespace video
}  // namespace satori

namespace std {
template <>
struct is_error_condition_enum<satori::video::rtm::client_error> : std::true_type {};
}  // namespace std