// RTM client interface definition.
#pragma once

#include <boost/asio.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/optional.hpp>
#include <functional>
#include <json.hpp>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "logging.h"

namespace satori {

namespace video {

namespace rtm {
enum class client_error : unsigned char {
  // 0 - not used, success.

  UNKNOWN = 1,
  NOT_CONNECTED = 2,
  RESPONSE_PARSING_ERROR = 3,
  INVALID_RESPONSE = 4,
  SUBSCRIPTION_ERROR = 5,
  SUBSCRIBE_ERROR = 6,
  UNSUBSCRIBE_ERROR = 7,
  ASIO_ERROR = 8,
  INVALID_MESSAGE = 9,
  // TODO: distinguish between network errors and RTM error replies
  PUBLISH_ERROR = 10
};

std::error_condition make_error_condition(client_error e);

struct error_callbacks {
  virtual ~error_callbacks() = default;
  virtual void on_error(std::error_condition ec) = 0;
};

struct request_callbacks : error_callbacks {
  ~request_callbacks() override = default;

  // publish, subscribe and unsubscribe support channel positions
  virtual void on_ok() = 0;
};

struct publisher {
  virtual ~publisher() = default;

  virtual void publish(const std::string &channel, nlohmann::json &&message,
                       request_callbacks *callbacks = nullptr) = 0;
};

// Subscription interface of RTM.
struct subscription {};

struct channel_data {
  nlohmann::json payload;
  std::chrono::system_clock::time_point arrival_time;
};

struct subscription_callbacks : error_callbacks {
  virtual void on_data(const subscription & /*subscription*/,
                       channel_data && /*unused*/) {}
};

struct history_options {
  boost::optional<uint64_t> count;
  boost::optional<uint64_t> age;
};

struct subscription_options {
  bool force{false};
  bool fast_forward{true};
  history_options history;
};

struct subscriber {
  virtual ~subscriber() = default;

  virtual void subscribe(const std::string &channel, const subscription &sub,
                         subscription_callbacks &data_callbacks,
                         request_callbacks *callbacks = nullptr,
                         const subscription_options *options = nullptr) = 0;

  virtual void unsubscribe(const subscription &sub,
                           request_callbacks *callbacks = nullptr) = 0;
};

class client : public publisher, public subscriber {
 public:
  // TODO: return deferred<std::error_condition>
  virtual std::error_condition start() __attribute__((warn_unused_result)) = 0;

  // TODO: return deferred<std::error_condition>
  virtual std::error_condition stop() __attribute__((warn_unused_result)) = 0;
};

std::unique_ptr<client> new_client(const std::string &endpoint, const std::string &port,
                                   const std::string &appkey,
                                   boost::asio::io_service &io_service,
                                   boost::asio::ssl::context &ssl_ctx, size_t id,
                                   error_callbacks &callbacks);

// Reconnects on any error.
// It is expected that methods of this client are invoked from ASIO loop thread.
class resilient_client : public client, error_callbacks {
 public:
  using client_factory_t =
      std::function<std::unique_ptr<client>(error_callbacks &callbacks)>;

  explicit resilient_client(boost::asio::io_service &io_service,
                            std::thread::id io_thread_id, client_factory_t &&factory,
                            error_callbacks &callbacks);

  void publish(const std::string &channel, nlohmann::json &&message,
               request_callbacks *callbacks) override;

  void subscribe(const std::string &channel, const subscription &sub,
                 subscription_callbacks &data_callbacks, request_callbacks *callbacks,
                 const subscription_options *options) override;

  void unsubscribe(const subscription &sub, request_callbacks *callbacks) override;

  std::error_condition start() override;

  std::error_condition stop() override;

 private:
  void on_error(std::error_condition ec) override;
  void restart();

  struct subscription_info {
    std::string channel;
    const subscription *sub;
    subscription_callbacks *data_callbacks;
    request_callbacks *callbacks;
    const subscription_options *options;
  };

  boost::asio::io_service &_io;
  const std::thread::id _io_thread_id;
  client_factory_t _factory;
  error_callbacks &_error_callbacks;
  std::unique_ptr<client> _client;
  bool _started{false};

  std::vector<subscription_info> _subscriptions;
};

// Forwards requests to ASIO loop thread if necessary.
class thread_checking_client : public client {
 public:
  explicit thread_checking_client(boost::asio::io_service &io,
                                  std::thread::id io_thread_id,
                                  std::unique_ptr<client> client);

  void publish(const std::string &channel, nlohmann::json &&message,
               request_callbacks *callbacks) override;

  void subscribe(const std::string &channel, const subscription &sub,
                 subscription_callbacks &data_callbacks, request_callbacks *callbacks,
                 const subscription_options *options) override;

  void unsubscribe(const subscription &sub, request_callbacks *callbacks) override;

  std::error_condition start() override;

  std::error_condition stop() override;

 private:
  boost::asio::io_service &_io;
  const std::thread::id _io_thread_id;
  std::unique_ptr<client> _client;
};

}  // namespace rtm
}  // namespace video
}  // namespace satori

namespace std {
template <>
struct is_error_condition_enum<satori::video::rtm::client_error> : std::true_type {};
}  // namespace std
