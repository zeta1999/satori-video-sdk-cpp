#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/filesystem.hpp>
#include <boost/optional.hpp>
#include <boost/program_options.hpp>
#include <json.hpp>
#include <list>
#include <memory>
#include <string>

#include "cli_streams.h"
#include "data.h"
#include "logging_impl.h"
#include "pool_controller.h"
#include "rtm_client.h"
#include "streams/signal_breaker.h"
#include "streams/threaded_worker.h"
#include "tcmalloc.h"
#include "video_streams.h"
#include "vp9_encoder.h"

namespace asio = boost::asio;
namespace fs = boost::filesystem;
namespace po = boost::program_options;

namespace satori {
namespace video {
namespace recorder {

namespace {

constexpr int max_streams_capacity = 5;

cli_streams::cli_options cli_configuration() {
  cli_streams::cli_options result;
  result.enable_file_output = true;
  result.enable_camera_input = true;
  result.enable_url_input = true;
  result.enable_rtm_input = true;
  result.enable_generic_input_options = true;
  result.enable_generic_output_options = true;
  result.enable_pool_mode = true;

  return result;
}

po::options_description cli_options() {
  po::options_description cli_generic("Generic options");
  cli_generic.add_options()("help", "produce help message");
  cli_generic.add_options()(
      ",v", po::value<std::string>(),
      "log verbosity level (INFO, WARNING, ERROR, FATAL, OFF, 1-9)");

  return cli_generic;
}

std::string escape_slashes(const std::string &s) {
  const std::string f{"/"};
  const std::string r{"{slash}"};

  std::string result{s};
  std::string::size_type i{0};
  while ((i = result.find(f, i)) != std::string::npos) {
    result.replace(i, f.size(), r);
    i += r.size();
  }

  return result;
}

struct recorder_configuration : cli_streams::configuration {
  recorder_configuration(int argc, char *argv[])
      : configuration(argc, argv, cli_configuration(), cli_options()) {}

  boost::optional<std::string> pool() const {
    return _vm.count("pool") > 0 ? _vm["pool"].as<std::string>()
                                 : boost::optional<std::string>{};
  }

  std::string pool_job_type() const {
    return _vm.count("pool-job-type") > 0 ? _vm["pool-job-type"].as<std::string>()
                                          : "recorder";
  }

  cli_streams::input_video_config as_input_config() const {
    return cli_streams::input_video_config{_vm};
  }

  cli_streams::output_video_config as_output_config() const {
    return cli_streams::output_video_config{_vm};
  }
};

using stream_done_callback_t = std::function<void(std::error_condition)>;

class video_stream : private streams::subscriber<encoded_packet> {
 public:
  video_stream(asio::io_service &io, std::shared_ptr<rtm::client> &client,
               cli_streams::input_video_config &&input_config,
               cli_streams::output_video_config &&output_config,
               const nlohmann::json &job, stream_done_callback_t &&done_callback)
      : _io{io},
        _client{client},
        _input_config{std::move(input_config)},
        _output_config{std::move(output_config)},
        _job{job},
        _done_callback{done_callback} {
    connect();
  }

  video_stream(const video_stream &) = delete;

  ~video_stream() override { stop(); }

  const nlohmann::json &job() const { return _job; }

  void stop() {
    LOG(INFO) << "stopping video stream for " << _input_config.input_channel.get();

    if (_subscription.is_initialized()) {
      LOG(INFO) << "canceling subscription for " << _input_config.input_channel.get();
      _subscription->cancel();
      _subscription.reset();
    }

    if (_sink.is_initialized()) {
      LOG(INFO) << "stopping sink for " << _input_config.input_channel.get();
      _sink->on_complete();
      _sink.reset();
    }
  }

 private:
  streams::publisher<encoded_packet> original_encoded_stream(const std::string &channel) {
    LOG(INFO) << "using original encoded stream";
    return cli_streams::encoded_publisher(_io, _client, _input_config)
           >> streams::threaded_worker("in_" + channel) >> streams::flatten();
  }

  streams::publisher<encoded_packet> transcoded_stream(const std::string &channel) {
    LOG(INFO) << "using transcoded stream";
    return cli_streams::decoded_publisher(_io, _client, _input_config,
                                          image_pixel_format::RGB0)
           >> streams::threaded_worker("in_" + channel) >> streams::flatten()
           >> encode_vp9(25) >> streams::threaded_worker("vp9_" + channel)
           >> streams::flatten();
  }

  void connect() {
    CHECK(_input_config.input_channel.is_initialized());
    CHECK(!_subscription.is_initialized());
    CHECK(!_sink.is_initialized());

    const std::string channel = _input_config.input_channel.get();
    LOG(INFO) << "starting recorder: " << channel;

    auto publisher = (_input_config.resolution == "original")
                         ? original_encoded_stream(channel)
                         : transcoded_stream(channel);

    _sink = cli_streams::encoded_subscriber(_io, _client, _output_config);

    publisher->subscribe(*this);
  }

  void on_next(encoded_packet &&pkt) override {
    CHECK(_subscription.is_initialized());
    CHECK(_sink.is_initialized());
    _sink->on_next(std::move(pkt));  // TODO: add metric
  }

  void on_error(std::error_condition ec) override {  // TODO: add metric
    LOG(INFO) << "stream failed, stopping " << _input_config.input_channel.get();
    CHECK(_subscription.is_initialized());
    CHECK(_sink.is_initialized());
    stop();
    _done_callback(ec);
  }

  void on_complete() override {  // TODO: add metric
    LOG(INFO) << "stream is complete, reconnecting " << _input_config.input_channel.get();
    CHECK(_subscription.is_initialized());
    CHECK(_sink.is_initialized());
    stop();
    _done_callback({});
  }

  void on_subscribe(streams::subscription &s) override {
    LOG(INFO) << "got subscription for " << _input_config.input_channel.get();
    CHECK(!_subscription.is_initialized());
    CHECK(_sink.is_initialized());
    _subscription = s;
    _sink->on_subscribe(s);
  }

 private:
  asio::io_service &_io;
  const std::shared_ptr<rtm::client> _client;
  const cli_streams::input_video_config _input_config;
  const cli_streams::output_video_config _output_config;
  const nlohmann::json _job{nullptr};
  const stream_done_callback_t _done_callback;
  boost::optional<streams::subscription &> _subscription;
  boost::optional<streams::subscriber<encoded_packet> &> _sink;
};

class recorder_job_controller : public job_controller {
 public:
  recorder_job_controller(asio::io_service &io, std::shared_ptr<rtm::client> &client,
                          const recorder_configuration &config)
      : _io{io}, _client{client}, _config{config} {}

 private:
  /**
   * Expecting jobs of the following format
   * {
   *   "channel": <string>,
   *   "segment-duration": <number> [OPTIONAL],
   *   "resolution": <string> [OPTIONAL],
   *   "reserved-index-space": <number> [OPTIONAL]
   * }
   */
  void add_job(const nlohmann::json &job) override {
    LOG(INFO) << "got a job: " << job;
    CHECK(job.is_object()) << "job is not an object: " << job;

    cli_streams::input_video_config input_config{job};
    CHECK(input_config.input_channel);

    LOG(INFO) << "channel name: " << escape_slashes(*input_config.input_channel);
    // TODO: ugly hack to make output path to be channel name
    const fs::path output_path = *_config.as_output_config().output_path
                                 / (escape_slashes(*input_config.input_channel) + ".mkv");
    LOG(INFO) << "output path: " << output_path;
    nlohmann::json job_copy{job};
    job_copy["output-video-file"] = output_path.string();
    cli_streams::output_video_config output_config{job_copy};

    _streams.emplace_back(_io, _client, std::move(input_config), std::move(output_config),
                          job, [](std::error_condition) {});
  }

  void remove_job(const nlohmann::json &job) override {
    ABORT() << "job removal is not supported: " << job;
  }

  nlohmann::json list_jobs() const override {
    nlohmann::json result = nlohmann::json::array();

    for (const auto &s : _streams) {
      result.emplace_back(s.job());
    }

    return result;
  }

 private:
  const recorder_configuration &_config;
  asio::io_service &_io;
  std::shared_ptr<rtm::client> _client;
  std::list<video_stream> _streams;
};

void request_rtm_client_stop(asio::io_service &io, std::shared_ptr<rtm::client> &client) {
  io.post([client]() {
    if (client) {
      LOG(INFO) << "stopping rtm client";
      if (auto ec = client->stop()) {
        LOG(ERROR) << "error stopping rtm client: " << ec.message();
      } else {
        LOG(INFO) << "rtm client was stopped";
      }
    }
  });
}

void run_standalone(asio::io_service &io, std::shared_ptr<rtm::client> &client,
                    const recorder_configuration &config) {
  video_stream recorded_stream{io,
                               client,
                               config.as_input_config(),
                               config.as_output_config(),
                               nullptr,
                               [&io, &client](std::error_condition stream_error) {
                                 if (stream_error.value() != 0) {
                                   LOG(ERROR) << "stream completed with failure: "
                                              << stream_error.message();
                                 } else {
                                   LOG(INFO) << "stream completed successfully";
                                 }
                                 request_rtm_client_stop(io, client);
                               }};

  signal::register_handler({SIGINT, SIGTERM, SIGQUIT},
                           [&recorded_stream, &io, &client](int /*signal*/) {
                             LOG(INFO) << "stopping the stream...";
                             recorded_stream.stop();
                             request_rtm_client_stop(io, client);
                           });

  LOG(INFO) << "starting recorder...";
  const auto number_of_handlers = io.run();
  LOG(INFO) << "recorder is stopped, executed " << number_of_handlers << " handlers";
}

void run_pool(asio::io_service &io, std::shared_ptr<rtm::client> &client,
              const recorder_configuration &config) {
  recorder_job_controller recorder_controller{io, client, config};
  pool_job_controller job_controller{
      io,     config.pool().get(), config.pool_job_type(), max_streams_capacity,
      client, recorder_controller};

  // Kubernetes sends SIGTERM, and then SIGKILL after 30 seconds
  // https://kubernetes.io/docs/concepts/workloads/pods/pod/#termination-of-pods
  signal::register_handler({SIGINT, SIGTERM, SIGQUIT},
                           [&job_controller, &io, &client](int /*signal*/) {
                             job_controller.shutdown();
                             request_rtm_client_stop(io, client);
                           });

  job_controller.start();
  LOG(INFO) << "starting recorder pool...";
  const auto number_of_handlers = io.run();
  LOG(INFO) << "recorder pool is stopped, executed " << number_of_handlers << " handlers";
}

}  // namespace

void recorder_main(int argc, char *argv[]) {
  recorder_configuration config{argc, argv};

  asio::io_service io;
  asio::ssl::context ssl_context{asio::ssl::context::sslv23};

  struct rtm_error_callbacks : rtm::error_callbacks {
    void on_error(std::error_condition ec) override { LOG(ERROR) << ec.message(); }
  } rtm_error_callbacks;

  std::shared_ptr<rtm::client> client =
      config.rtm_client(io, std::this_thread::get_id(), ssl_context, rtm_error_callbacks);
  if (client) {
    if (auto ec = client->start()) {
      ABORT() << "error starting rtm client: " << ec.message();
    }
  }

  if (config.pool()) {
    LOG(INFO) << "running recorder in pool mode";
    run_pool(io, client, config);
  } else {
    LOG(INFO) << "running standalone recorder";
    run_standalone(io, client, config);
  }
}

}  // namespace recorder
}  // namespace video
}  // namespace satori

int main(int argc, char *argv[]) {
  satori::video::init_tcmalloc();
  satori::video::init_logging(argc, argv);
  satori::video::recorder::recorder_main(argc, argv);
  return 0;
}
