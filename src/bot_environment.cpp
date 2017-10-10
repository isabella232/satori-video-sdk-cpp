#include "bot_environment.h"

#include <rapidjson/document.h>
#include <rapidjson/filereadstream.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <boost/asio.hpp>
#include <boost/program_options.hpp>
#include <fstream>

#include "avutils.h"
#include "bot_instance.h"
#include "cbor_json.h"
#include "cbor_tools.h"
#include "cli_streams.h"
#include "logging_impl.h"
#include "rtm_streams.h"
#include "streams/asio_streams.h"
#include "streams/buffered_worker.h"
#include "streams/signal_breaker.h"

namespace satori {
namespace video {
namespace {

constexpr size_t image_buffer_size = 2;
using variables_map = boost::program_options::variables_map;

variables_map parse_command_line(int argc, char* argv[],
                                 const cli_streams::configuration& cli_cfg) {
  namespace po = boost::program_options;

  po::options_description generic("Generic options");
  generic.add_options()("help", "produce help message");
  generic.add_options()(",v", po::value<std::string>(),
                        "log verbosity level (INFO, WARNING, ERROR, FATAL, OFF, 1-9)");

  po::options_description bot_configuration_options("Bot configuration options");
  bot_configuration_options.add_options()(
      "id", po::value<std::string>()->default_value(""), "bot id");
  bot_configuration_options.add_options()("config", po::value<std::string>(),
                                          "bot config file");

  po::options_description bot_execution_options("Bot execution options");
  bot_execution_options.add_options()(
      "analysis_file", po::value<std::string>(),
      "saves analysis messages to a file instead of sending to a channel");
  bot_execution_options.add_options()(
      "debug_file", po::value<std::string>(),
      "saves debug messages to a file instead of sending to a channel");

  po::options_description cli_options = cli_cfg.to_boost();
  cli_options.add(bot_configuration_options).add(bot_execution_options).add(generic);

  po::variables_map vm;

  try {
    po::store(po::parse_command_line(argc, argv, cli_options), vm);
    po::notify(vm);
  } catch (const std::exception& e) {
    std::cerr << e.what() << std::endl;
    std::cerr << cli_options << std::endl;
    exit(1);
  }

  if (argc == 1 || vm.count("help")) {
    std::cerr << cli_options << std::endl;
    exit(1);
  }

  if (!cli_cfg.validate(vm)) exit(1);

  return vm;
}

// todo: move this reusable class out.
struct file_cbor_dump_observer : public streams::observer<cbor_item_t*> {
  explicit file_cbor_dump_observer(std::ostream& out) : _out(out) {}

  void on_next(cbor_item_t*&& t) override {
    _out << t << std::endl;
    cbor_decref(&t);
  }
  void on_error(std::error_condition ec) override {
    LOG(ERROR) << "ERROR: " << ec.message();
    delete this;
  }

  void on_complete() override { delete this; }

  std::ostream& _out;
};

cbor_item_t* configure_command(cbor_item_t* config) {
  cbor_item_t* cmd = cbor_new_definite_map(2);
  cbor_map_add(cmd, {cbor_move(cbor_build_string("action")),
                     cbor_move(cbor_build_string("configure"))});
  cbor_map_add(cmd, {cbor_move(cbor_build_string("body")), config});
  return cmd;
}

void log_important_counters() {
  LOG(INFO) << "  input.network_buffer.delivered = " << std::setw(5) << std::left
            << tele::counter_get("input.network_buffer.delivered")
            << "  input.network_buffer.dropped = " << std::setw(5) << std::left
            << tele::counter_get("input.network_buffer.dropped")
            << "  input.network_buffer.size = " << std::setw(2) << std::left
            << tele::gauge_get("input.network_buffer.size");

  LOG(INFO) << "  input.encoded_buffer.delivered = " << std::setw(5) << std::left
            << tele::counter_get("input.encoded_buffer.delivered")
            << "  input.encoded_buffer.dropped = " << std::setw(5) << std::left
            << tele::counter_get("input.encoded_buffer.dropped")
            << "  input.encoded_buffer.size = " << std::setw(2) << std::left
            << tele::gauge_get("input.encoded_buffer.size");

  LOG(INFO) << "    input.image_buffer.delivered = " << std::setw(5) << std::left
            << tele::counter_get("input.image_buffer.delivered")
            << "    input.image_buffer.dropped = " << std::setw(5) << std::left
            << tele::counter_get("input.image_buffer.dropped")
            << "    input.image_buffer.size = " << std::setw(2) << std::left
            << tele::gauge_get("input.image_buffer.size");
}

}  // namespace

void bot_environment::parse_config(boost::optional<std::string> config_file) {
  if (!_bot_descriptor->ctrl_callback && config_file.is_initialized()) {
    std::cerr << "Config specified but there is no control method set\n";
    exit(1);
  }

  if (!_bot_descriptor->ctrl_callback) {
    return;
  }

  cbor_item_t* config;

  if (config_file.is_initialized()) {
    FILE* fp = fopen(config_file.get().c_str(), "r");
    if (!fp) {
      std::cerr << "Can't read config file " << config_file.get() << ": "
                << strerror(errno) << "\n";
      exit(1);
    }
    auto file_closer = gsl::finally([&fp]() { fclose(fp); });

    char readBuffer[65536];
    rapidjson::FileReadStream is(fp, readBuffer, sizeof(readBuffer));
    rapidjson::Document d;
    d.ParseStream(is);

    config = json_to_cbor(d);
  } else {
    config = cbor_new_definite_map(0);
  }
  cbor_item_t* cmd = configure_command(config);
  auto cbor_deleter = gsl::finally([&config, &cmd]() {
    cbor_decref(&config);
    cbor_decref(&cmd);
  });

  cbor_item_t* response = _bot_descriptor->ctrl_callback(*_bot_instance, cmd);
  if (response != nullptr) {
    _bot_instance->queue_message(bot_message_kind::DEBUG, cbor_move(response),
                                 frame_id{0, 0});
  }
}

bot_environment& bot_environment::instance() {
  static bot_environment env;
  return env;
}

void bot_environment::register_bot(const bot_descriptor* bot) {
  CHECK(!_bot_descriptor);
  _bot_descriptor = bot;
}

void bot_environment::send_messages(std::list<struct bot_message>&& messages) {
  for (auto&& msg : messages) {
    switch (msg.kind) {
      case bot_message_kind::ANALYSIS:
        _analysis_sink->on_next(std::move(msg.data));
        break;
      case bot_message_kind::CONTROL:
        _control_sink->on_next(std::move(msg.data));
        break;
      case bot_message_kind::DEBUG:
        _debug_sink->on_next(std::move(msg.data));
        break;
    }
  }

  messages.clear();
}

int bot_environment::main(int argc, char* argv[]) {
  cli_streams::configuration cli_cfg;
  cli_cfg.enable_rtm_input = true;
  cli_cfg.enable_file_input = true;
  cli_cfg.enable_generic_input_options = true;
  cli_cfg.enable_file_batch_mode = true;

  auto cmd_args = parse_command_line(argc, argv, cli_cfg);
  init_logging(argc, argv);

  const std::string id = cmd_args["id"].as<std::string>();
  const bool batch = cmd_args.count("batch");
  _bot_instance.reset(new bot_instance(
      id, batch ? execution_mode::BATCH : execution_mode::LIVE, *_bot_descriptor, *this));
  parse_config(cmd_args.count("config")
                   ? boost::optional<std::string>{cmd_args["config"].as<std::string>()}
                   : boost::optional<std::string>{});

  boost::asio::io_service io_service;
  boost::asio::ssl::context ssl_context{boost::asio::ssl::context::sslv23};

  _rtm_client = cli_cfg.rtm_client(cmd_args, io_service, ssl_context, *this);
  if (_rtm_client) {
    if (auto ec = _rtm_client->start()) {
      ABORT() << "error starting rtm client: " << ec.message();
    }
    _tele_publisher.reset(new tele::publisher(*_rtm_client, io_service));
  }

  const std::string channel = cli_cfg.rtm_channel(cmd_args);
  const bool batch_mode = cli_cfg.is_batch_mode(cmd_args);
  _source = cli_cfg.decoded_publisher(cmd_args, io_service, _rtm_client, channel, true,
                                      _bot_descriptor->pixel_format);

  if (!batch_mode) {
    _source = std::move(_source)
              >> streams::buffered_worker("input.image_buffer", image_buffer_size);
  }

  if (cmd_args.count("analysis_file")) {
    std::string analysis_file = cmd_args["analysis_file"].as<std::string>();
    LOG(INFO) << "saving analysis output to " << analysis_file;
    _analysis_file.reset(new std::ofstream(analysis_file.c_str()));
    _analysis_sink = new file_cbor_dump_observer(*_analysis_file);
  } else if (_rtm_client) {
    _analysis_sink = &rtm::cbor_sink(_rtm_client, channel + analysis_channel_suffix);
  } else {
    _analysis_sink = new file_cbor_dump_observer(std::cout);
  }

  if (cmd_args.count("debug_file")) {
    std::string debug_file = cmd_args["debug_file"].as<std::string>();
    LOG(INFO) << "saving debug output to " << debug_file;
    _debug_file.reset(new std::ofstream(debug_file.c_str()));
    _debug_sink = new file_cbor_dump_observer(*_debug_file);
  } else if (_rtm_client) {
    _debug_sink = &rtm::cbor_sink(_rtm_client, channel + debug_channel_suffix);
  } else {
    _debug_sink = new file_cbor_dump_observer(std::cerr);
  }

  if (_rtm_client) {
    _control_sink = &rtm::cbor_sink(_rtm_client, control_channel);
    _control_source = rtm::cbor_channel(_rtm_client, control_channel, {});
  } else {
    _control_sink = new file_cbor_dump_observer(std::cout);
    _control_source = streams::publishers::empty<cbor_item_t*>();
  }

  bool finished{false};
  int frames_count = 0;

  _source = std::move(_source)
            >> streams::signal_breaker<owned_image_packet>({SIGINT, SIGTERM, SIGQUIT})
            >> streams::map([frames_count](owned_image_packet&& pkt) mutable {
                frames_count++;
                constexpr int period = 100;
                if (!(frames_count % period)) {
                  LOG(INFO) << "Processed " << period << " frames";
                  log_important_counters();
                }
                return pkt;
              })
            >> streams::do_finally([this, &finished]() {
                finished = true;
                _bot_instance->stop();
                _tele_publisher.reset();
                if (_rtm_client) {
                  auto ec = _rtm_client->stop();
                  if (ec) LOG(ERROR) << "error stopping rtm client: " << ec.message();
                }
              });

  _bot_instance->start(_source, _control_source);

  if (!batch_mode) {
    LOG(INFO) << "entering asio loop";
    auto n = io_service.run();
    LOG(INFO) << "asio loop exited, executed " << n << " handlers";

    while (!finished) {
      // batch mode has no threads
      LOG(INFO) << "waiting for all threads to finish...";
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }

  return 0;
}

void bot_environment::on_error(std::error_condition ec) {
  ABORT() << "rtm error: " << ec.message();
}

}  // namespace video
}  // namespace satori
