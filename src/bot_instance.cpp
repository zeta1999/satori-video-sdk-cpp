#include "bot_instance.h"

#include <gsl/gsl>

#include "metrics.h"
#include "stopwatch.h"

namespace satori {
namespace video {
namespace {
auto& processing_times_millis =
    prometheus::BuildHistogram()
        .Name("frame_batch_processing_times_millis")
        .Register(metrics_registry())
        .Add({}, std::vector<double>{0,    1,    2,    5,    10,   15,   20,   25,   30,
                                     40,   50,   60,   70,   80,   90,   100,  200,  300,
                                     400,  500,  600,  700,  800,  900,  1000, 2000, 3000,
                                     4000, 5000, 6000, 7000, 8000, 9000, 10000});
auto& frame_size =
    prometheus::BuildHistogram()
        .Name("frame_batch_size")
        .Register(metrics_registry())
        .Add({}, std::vector<double>{1,  2,  3,   4,   5,   6,   7,   8,  9,
                                     10, 15, 20,  25,  30,  40,  50,  60, 70,
                                     80, 90, 100, 200, 300, 400, 500, 750});

auto& frame_batch_processed_total = prometheus::BuildCounter()
                                        .Name("frame_batch_processed_total")
                                        .Register(metrics_registry())
                                        .Add({});
auto& messages_sent =
    prometheus::BuildCounter().Name("messages_sent").Register(metrics_registry());
auto& messages_received =
    prometheus::BuildCounter().Name("messages_received").Register(metrics_registry());

nlohmann::json build_configure_command(const nlohmann::json& config) {
  nlohmann::json cmd = nlohmann::json::object();
  cmd["action"] = "configure";
  cmd["body"] = config;
  return cmd;
}

nlohmann::json build_shutdown_command() {
  nlohmann::json cmd = {{"action", "shutdown"}};
  return cmd;
}

}  // namespace

bot_instance::bot_instance(const std::string& bot_id, const execution_mode execmode,
                           const multiframe_bot_descriptor& descriptor)
    : _bot_id(bot_id),
      _descriptor(descriptor),
      bot_context{nullptr,
                  &_image_metadata,
                  execmode,
                  {
                      satori::video::metrics_registry(),
                      prometheus::BuildCounter()
                          .Name("frames_processed_total")
                          .Register(satori::video::metrics_registry())
                          .Add({{"id", bot_id}}),
                      prometheus::BuildCounter()
                          .Name("frames_dropped_total")
                          .Register(satori::video::metrics_registry())
                          .Add({{"id", bot_id}}),
                      prometheus::BuildHistogram()
                          .Name("frame_processing_times_millis")
                          .Register(satori::video::metrics_registry())
                          .Add({{"id", bot_id}},
                               std::vector<double>{0,  1,   2,   5,   10,  15,  20,
                                                   25, 30,  40,  50,  60,  70,  80,
                                                   90, 100, 200, 300, 400, 500, 750}),
                  }} {}

streams::op<bot_input, bot_output> bot_instance::run_bot() {
  return [this](streams::publisher<bot_input>&& src) {
    auto main_stream =
        std::move(src)
        >> streams::map([this](bot_input&& p) { return boost::apply_visitor(*this, p); })
        >> streams::flatten();

    // TODO: maybe initial config and shutdown message should be sent from the same place?
    auto shutdown_stream = streams::generators<bot_output>::stateful(
        [this]() {
          LOG(INFO) << "shutting down bot";
          if (_descriptor.ctrl_callback) {
            nlohmann::json cmd = build_shutdown_command();
            nlohmann::json response = _descriptor.ctrl_callback(*this, std::move(cmd));
            if (!response.is_null()) {
              LOG(INFO) << "got shutdown response: " << response;
              queue_message(bot_message_kind::DEBUG, std::move(response), frame_id{0, 0});
            } else {
              LOG(INFO) << "shutdown response is null";
            }
          }

          prepare_message_buffer_for_downstream();

          return nullptr;
        },
        [this](void*, streams::observer<bot_output>& sink) {
          if (_message_buffer.empty()) {
            sink.on_complete();
            return;
          }

          LOG(INFO) << "sending shutdown";
          struct bot_message msg = std::move(_message_buffer.front());
          _message_buffer.pop_front();

          sink.on_next(std::move(msg));
        });

    return streams::publishers::concat(std::move(main_stream),
                                       std::move(shutdown_stream));
  };
}

void bot_instance::queue_message(const bot_message_kind kind, nlohmann::json&& message,
                                 const frame_id& id) {
  CHECK(message.is_object()) << "message is not an object: " << message;

  frame_id effective_frame_id =
      (id.i1 == 0 && id.i2 == 0 && _current_frame_id.i1 != 0 && _current_frame_id.i2 != 0)
          ? _current_frame_id
          : id;

  struct bot_message newmsg {
    std::move(message), kind, effective_frame_id
  };
  _message_buffer.push_back(std::move(newmsg));
}

void bot_instance::set_current_frame_id(const frame_id& id) { _current_frame_id = id; }

std::vector<image_frame> bot_instance::extract_frames(
    const std::list<bot_output>& packets) {
  std::vector<image_frame> result;

  for (const auto& p : packets) {
    auto* frame = boost::get<owned_image_frame>(&p);

    if (frame == nullptr) {
      continue;
    }

    if (frame->width != _image_metadata.width
        || frame->height != _image_metadata.height) {
      CHECK(_image_metadata.width == 0)
          << "frame resolution has been changed: " << _image_metadata.width << "x"
          << _image_metadata.height << " -> " << frame->width << "x" << frame->height;
      _image_metadata.width = frame->width;
      _image_metadata.height = frame->height;
      std::copy(frame->plane_strides, frame->plane_strides + max_image_planes,
                _image_metadata.plane_strides);
    }

    image_frame bframe;
    bframe.id = frame->id;
    for (int i = 0; i < max_image_planes; ++i) {
      if (frame->plane_data[i].empty()) {
        bframe.plane_data[i] = nullptr;
      } else {
        bframe.plane_data[i] = (const uint8_t*)frame->plane_data[i].data();
      }
    }
    result.push_back(std::move(bframe));
  }
  return result;
}

std::list<bot_output> bot_instance::operator()(std::queue<owned_image_packet>& pp) {
  stopwatch<> s;
  std::list<bot_output> result;

  frame_size.Observe(pp.size());

  while (!pp.empty()) {
    result.emplace_back(pp.front());
    pp.pop();
  }

  std::vector<image_frame> bframes = extract_frames(result);

  if (!bframes.empty()) {
    LOG(1) << "process " << bframes.size() << " frames " << _image_metadata.width << "x"
           << _image_metadata.height;

    _descriptor.img_callback(*this, gsl::span<image_frame>(bframes));
    frame_batch_processed_total.Increment();

    prepare_message_buffer_for_downstream();

    std::copy(_message_buffer.begin(), _message_buffer.end(), std::back_inserter(result));
    _message_buffer.clear();
  }

  processing_times_millis.Observe(s.millis());
  return result;
}

std::list<bot_output> bot_instance::operator()(nlohmann::json& msg) {
  messages_received.Add({{"message_type", "control"}}).Increment();
  if (msg.is_array()) {
    std::list<bot_output> aggregated;
    for (auto& el : msg) {
      aggregated.splice(aggregated.end(), this->operator()(el));
    }
    return aggregated;
  }

  if (!msg.is_object() || msg.find("to") == msg.end()) {
    LOG(ERROR) << "unsupported kind of message: " << msg;
    return std::list<bot_output>{};
  }

  if (_bot_id.empty() || msg["to"] != _bot_id) {
    LOG(INFO) << "message for a different bot: " << msg;
    return std::list<bot_output>{};
  }

  nlohmann::json response = _descriptor.ctrl_callback(*this, msg);

  if (!response.is_null()) {
    CHECK(response.is_object()) << "bot response is not an object: " << response;

    if (msg.find("request_id") != msg.end()) {
      response["request_id"] = msg["request_id"];
    }

    queue_message(bot_message_kind::CONTROL, std::move(response), frame_id{0, 0});
  }

  prepare_message_buffer_for_downstream();

  std::list<bot_output> result{_message_buffer.begin(), _message_buffer.end()};
  _message_buffer.clear();
  return result;
}

void bot_instance::prepare_message_buffer_for_downstream() {
  for (auto&& msg : _message_buffer) {
    switch (msg.kind) {
      case bot_message_kind::ANALYSIS:
        messages_sent.Add({{"message_type", "analysis"}}).Increment();
        break;
      case bot_message_kind::DEBUG:
        messages_sent.Add({{"message_type", "debug"}}).Increment();
        break;
      case bot_message_kind::CONTROL:
        messages_sent.Add({{"message_type", "control"}}).Increment();
        break;
    }

    CHECK(msg.data.is_object()) << "data is not an object: " << msg.data;

    if (msg.id.i1 >= 0) {
      msg.data["i"] = {msg.id.i1, msg.id.i2};
    }

    if (!_bot_id.empty()) {
      msg.data["from"] = _bot_id;
    }
  }
}

void bot_instance::configure(const nlohmann::json& config) {
  if (!_descriptor.ctrl_callback) {
    if (config.is_null()) {
      return;
    }
    ABORT() << "Bot control handler was not provided but config was";
  }

  nlohmann::json cmd =
      build_configure_command(!config.is_null() ? config : nlohmann::json::object());

  LOG(INFO) << "configuring bot: " << cmd;
  nlohmann::json response = _descriptor.ctrl_callback(*this, std::move(cmd));
  if (!response.is_null()) {
    queue_message(bot_message_kind::DEBUG, std::move(response), frame_id{0, 0});
  }
}

}  // namespace video
}  // namespace satori
