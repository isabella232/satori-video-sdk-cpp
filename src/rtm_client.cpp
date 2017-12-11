#include "rtm_client.h"

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/format.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/regex.hpp>
#include <deque>
#include <gsl/gsl>
#include <iostream>
#include <json.hpp>
#include <memory>
#include <utility>

#include "cbor_json.h"
#include "logging.h"
#include "metrics.h"
#include "threadutils.h"

namespace asio = boost::asio;

namespace satori {

namespace video {

namespace rtm {

using endpoint_iterator_t = asio::ip::tcp::resolver::iterator;
using endpoint_t = asio::ip::tcp::resolver::endpoint_type;

struct client_error_category : std::error_category {
  const char *name() const noexcept override { return "rtm-client"; }
  std::string message(int ev) const override {
    switch (static_cast<client_error>(ev)) {
      case client_error::Unknown:
        return "unknown error";
      case client_error::NotConnected:
        return "client is not connected";
      case client_error::ResponseParsingError:
        return "error parsing response";
      case client_error::InvalidResponse:
        return "invalid response";
      case client_error::SubscriptionError:
        return "subscription error";
      case client_error::SubscribeError:
        return "subscribe error";
      case client_error::UnsubscribeError:
        return "unsubscribe error";
      case client_error::AsioError:
        return "asio error";
      case client_error::InvalidMessage:
        return "invalid message";
    }
  }
};

std::error_condition make_error_condition(client_error e) {
  static client_error_category category;
  return {static_cast<int>(e), category};
}

namespace {

constexpr int READ_BUFFER_SIZE = 100000;

const boost::posix_time::minutes WS_PING_INTERVAL{1};

auto &rtm_actions_received = prometheus::BuildCounter()
                                 .Name("rtm_actions_received_total")
                                 .Register(metrics_registry());

auto &rtm_messages_received = prometheus::BuildCounter()
                                  .Name("rtm_messages_received_total")
                                  .Register(metrics_registry());

auto &rtm_messages_bytes_received = prometheus::BuildCounter()
                                        .Name("rtm_messages_received_bytes_total")
                                        .Register(metrics_registry());

auto &rtm_messages_sent = prometheus::BuildCounter()
                              .Name("rtm_messages_sent_total")
                              .Register(metrics_registry());

auto &rtm_messages_bytes_sent = prometheus::BuildCounter()
                                    .Name("rtm_messages_sent_bytes_total")
                                    .Register(metrics_registry());

auto &rtm_bytes_written = prometheus::BuildCounter()
                              .Name("rtm_bytes_written_total")
                              .Register(metrics_registry())
                              .Add({});

auto &rtm_bytes_read = prometheus::BuildCounter()
                           .Name("rtm_bytes_read_total")
                           .Register(metrics_registry())
                           .Add({});

auto &rtm_pings_sent_total = prometheus::BuildCounter()
                                 .Name("rtm_pings_sent_total")
                                 .Register(metrics_registry())
                                 .Add({});

auto &rtm_frames_received_total = prometheus::BuildCounter()
                                      .Name("rtm_frames_received_total")
                                      .Register(metrics_registry());

auto &rtm_last_pong_time_seconds = prometheus::BuildGauge()
                                       .Name("rtm_last_pong_time_seconds")
                                       .Register(metrics_registry())
                                       .Add({});

auto &rtm_last_ping_time_seconds = prometheus::BuildGauge()
                                       .Name("rtm_last_ping_time_seconds")
                                       .Register(metrics_registry())
                                       .Add({});

struct subscribe_request {
  const uint64_t id;
  const std::string channel;
  boost::optional<uint64_t> age;
  boost::optional<uint64_t> count;

  nlohmann::json to_json() const {
    nlohmann::json document =
        R"({"action":"rtm/subscribe", "body":{"channel":"<not_set>", "subscription_id":"<not_set>"}, "id": 2})"_json;

    CHECK(document.is_object());
    document["id"] = id;
    auto &body = document["body"];
    body["channel"] = channel;
    body["subscription_id"] = channel;

    if (age || count) {
      nlohmann::json history;
      if (age) {
        history.emplace("age", *age);
      }
      if (count) {
        history.emplace("count", *count);
      }

      body.emplace("history", history);
    }

    return document;
  }
};

struct unsubscribe_request {
  const uint64_t id;
  const std::string channel;

  nlohmann::json to_json() const {
    nlohmann::json document =
        R"({"action":"rtm/unsubscribe", "body":{"subscription_id":"<not_set>"}, "id": 2})"_json;

    CHECK(document.is_object());
    document["id"] = id;
    auto &body = document["body"];
    body["subscription_id"] = channel;

    return document;
  }
};

// TODO: add <<(ostream &out, sub_status)
enum class subscription_status {
  PendingSubscribe = 1,
  Current = 2,
  PendingUnsubscribe = 3
};

struct subscription_impl {
  const std::string channel;
  const subscription &sub;
  subscription_callbacks &callbacks;
  subscription_status status;
  uint64_t pending_request_id{UINT64_MAX};
};

enum class client_state { Stopped = 1, Running = 2, PendingStopped = 3 };

std::ostream &operator<<(std::ostream &out, client_state const &s) {
  switch (s) {
    case client_state::Running:
      return out << "client_state_running";
    case client_state::PendingStopped:
      return out << "client_state_pending_stopped";
    case client_state::Stopped:
      return out << "client_state_stopped";
  }
}

class secure_client : public client {
 public:
  explicit secure_client(const std::string &host, const std::string &port,
                         const std::string &appkey, uint64_t client_id,
                         error_callbacks &callbacks, asio::io_service &io_service,
                         asio::ssl::context &ssl_ctx)
      : _host{host},
        _port{port},
        _appkey{appkey},
        _tcp_resolver{io_service},
        _ws{io_service, ssl_ctx},
        _client_id{client_id},
        _callbacks{callbacks},
        _ping_timer{io_service} {
    _control_callback = [](boost::beast::websocket::frame_type type,
                           boost::beast::string_view payload) {
      switch (type) {
        case boost::beast::websocket::frame_type::close:
          rtm_frames_received_total.Add({{"type", "close"}}).Increment();
          LOG(2) << "got close frame " << payload;
          break;
        case boost::beast::websocket::frame_type::ping:
          rtm_frames_received_total.Add({{"type", "ping"}}).Increment();
          LOG(2) << "got ping frame " << payload;
          break;
        case boost::beast::websocket::frame_type::pong:
          rtm_frames_received_total.Add({{"type", "pong"}}).Increment();
          auto time_since_epoch = std::chrono::system_clock::now().time_since_epoch();
          rtm_last_pong_time_seconds.Set(
              std::chrono::duration_cast<std::chrono::seconds>(time_since_epoch).count());
          LOG(2) << "got pong frame " << payload;
          break;
      }
    };
  }

  ~secure_client() override = default;

  std::error_condition start() override {
    CHECK_EQ(_client_state.load(), client_state::Stopped);
    LOG(INFO) << "Starting secure RTM client: " << _host << ":" << _port
              << ", appkey: " << _appkey;

    boost::system::error_code ec;

    auto endpoints = _tcp_resolver.resolve({_host, _port}, ec);
    if (ec.value() != 0) {
      LOG(ERROR) << "can't resolve endpoint: " << ec.message();
      return make_error_condition(client_error::AsioError);
    }

    _ws.read_message_max(READ_BUFFER_SIZE);

    // tcp connect
    asio::connect(_ws.next_layer().next_layer(), endpoints, ec);
    if (ec.value() != 0) {
      LOG(ERROR) << "can't connect: " << ec.message();
      return make_error_condition(client_error::AsioError);
    }

    // ssl handshake
    _ws.next_layer().handshake(boost::asio::ssl::stream_base::client);

    // upgrade to ws.
    _ws.handshake(_host, "/v2?appkey=" + _appkey, ec);
    if (ec.value() != 0) {
      LOG(ERROR) << "can't upgrade to websocket protocol: " << ec.message();
      return make_error_condition(client_error::AsioError);
    }
    LOG(1) << "Websocket open";

    _ws.control_callback(_control_callback);

    arm_ping_timer();

    _client_state = client_state::Running;
    ask_for_read();
    return {};
  }

  std::error_condition stop() override {
    CHECK_EQ(_client_state, client_state::Running);
    LOG(INFO) << "Stopping secure RTM client";

    _client_state = client_state::PendingStopped;
    boost::system::error_code ec;
    _ping_timer.cancel(ec);
    if (ec.value() != 0) {
      LOG(ERROR) << "can't stop ping timer: " << ec.message();
      return make_error_condition(client_error::AsioError);
    }

    ec.clear();
    _ws.next_layer().next_layer().close(ec);
    if (ec.value() != 0) {
      LOG(ERROR) << "can't close: " << ec.message();
      return make_error_condition(client_error::AsioError);
    }

    _ws.control_callback();
    return {};
  }

  void publish(const std::string &channel, cbor_item_t *message,
               publish_callbacks *callbacks) override {
    CHECK(!callbacks) << "not implemented";
    if (_client_state == client_state::PendingStopped) {
      LOG(1) << "RTM client is pending stop";
      return;
    }
    CHECK_EQ(_client_state, client_state::Running) << "RTM client is not running";

    CHECK_EQ(0, cbor_refcount(message));
    cbor_incref(message);
    auto decref = gsl::finally([&message]() { cbor_decref(&message); });

    nlohmann::json document =
        R"({"action":"rtm/publish", "body":{"channel":"<not_set>", "message":"<not_set>"}})"_json;

    CHECK(document.is_object());

    auto &body = document["body"];
    body["channel"] = channel;
    body["message"] = video::cbor_to_json(message);

    const std::string document_str = document.dump();

    rtm_messages_sent.Add({{"channel", channel}}).Increment();
    rtm_messages_bytes_sent.Add({{"channel", channel}}).Increment(document_str.size());
    rtm_bytes_written.Increment(document_str.size());
    _ws.write(asio::buffer(document_str));
  }

  void subscribe_channel(const std::string &channel, const subscription &sub,
                         subscription_callbacks &callbacks,
                         const subscription_options *options) override {
    if (_client_state == client_state::PendingStopped) {
      LOG(1) << "RTM client is pending stop";
      return;
    }
    CHECK_EQ(_client_state, client_state::Running) << "RTM client is not running";

    _subscriptions.emplace(std::make_pair(
        channel,
        subscription_impl{channel, sub, callbacks, subscription_status::PendingSubscribe,
                          ++_request_id}));

    subscribe_request request{_request_id, channel};
    if (options != nullptr) {
      request.age = options->history.age;
      request.count = options->history.count;
    }

    nlohmann::json document = request.to_json();
    const std::string document_str = document.dump();

    rtm_bytes_written.Increment(document_str.size());
    _ws.write(asio::buffer(document_str));

    LOG(1) << "requested subscribe: " << document;
  }

  void subscribe_filter(const std::string & /*filter*/, const subscription & /*sub*/,
                        subscription_callbacks & /*callbacks*/,
                        const subscription_options * /*options*/) override {
    ABORT() << "NOT IMPLEMENTED";
  }

  void unsubscribe(const subscription &sub_to_delete) override {
    if (_client_state == client_state::PendingStopped) {
      LOG(1) << "RTM client is pending stop";
      return;
    }
    CHECK_EQ(_client_state, client_state::Running) << "RTM client is not running";

    for (auto &it : _subscriptions) {
      const std::string &sub_id = it.first;
      subscription_impl &sub = it.second;
      if (&sub.sub != &sub_to_delete) {
        continue;
      }

      unsubscribe_request request{++_request_id, sub_id};
      nlohmann::json document = request.to_json();
      const std::string document_str = document.dump();

      rtm_bytes_written.Increment(document_str.size());
      _ws.write(asio::buffer(document_str));

      sub.pending_request_id = _request_id;
      sub.status = subscription_status::PendingUnsubscribe;

      LOG(1) << "requested unsubscribe: " << document;
      return;
    }
    ABORT() << "didn't find subscription";
  }

  channel_position position(const subscription & /*sub*/) override {
    ABORT() << "NOT IMPLEMENTED";
    return {0, 0};
  }

  bool is_up(const subscription & /*sub*/) override {
    ABORT() << "NOT IMPLEMENTED";
    return false;
  }

 private:
  void ask_for_read() {
    _ws.async_read(_read_buffer, [this](boost::system::error_code const &ec,
                                        unsigned long) {
      LOG(9) << this << " async_read";
      if (ec == boost::asio::error::operation_aborted) {
        LOG(9) << this << " async_read operation is aborted/cancelled";
        CHECK(_client_state == client_state::PendingStopped);
        LOG(INFO) << "Got stop request for async_read loop";
        _client_state = client_state::Stopped;
        _subscriptions.clear();
        return;
      }

      LOG(9) << this << " async_read ec.value() = " << ec.value();
      if (ec.value() != 0) {
        if (_client_state == client_state::Running) {
          LOG(ERROR) << this << " asio error: " << ec.message();
          _callbacks.on_error(client_error::AsioError);
        } else {
          LOG(INFO) << this << " ignoring asio error: " << ec.message()
                    << " because in state " << _client_state;
        }

        return;
      }

      std::string input = boost::lexical_cast<std::string>(buffers(_read_buffer.data()));
      auto input_size = _read_buffer.size();
      _read_buffer.consume(input_size);
      rtm_bytes_read.Increment(input_size);

      LOG(9) << this << " async_read input_size = " << input_size;
      nlohmann::json document;
      try {
        document = nlohmann::json::parse(input);
      } catch (const std::exception &e) {
        LOG(ERROR) << "Parse message error: " << e.what() << ", message: " << input;
        _callbacks.on_error(client_error::InvalidMessage);
        return;
      }

      LOG(9) << this << " async_read processing input";
      process_input(document, input_size);

      LOG(9) << this << " async_read asking for read";
      ask_for_read();
    });
  }

  void arm_ping_timer() {
    LOG(2) << this << " setting ws ping timer";

    _ping_timer.expires_from_now(WS_PING_INTERVAL);
    _ping_timer.async_wait([this](const boost::system::error_code &ec) {
      rtm_pings_sent_total.Increment();
      auto time_since_epoch = std::chrono::system_clock::now().time_since_epoch();
      rtm_last_ping_time_seconds.Set(
          std::chrono::duration_cast<std::chrono::seconds>(time_since_epoch).count());

      _ws.async_ping("pingmsg", [this](boost::system::error_code const &ec) {
        LOG(2) << this << " ping_write_handler";
        if (ec == boost::asio::error::operation_aborted) {
          LOG(ERROR) << this << " ping operation is aborted/cancelled";
          // TODO: should we react, or just rely on _ws.async_read()
          return;
        }

        LOG(2) << this << " ping_write_handler ec.value() = " << ec.value();
        if (ec.value() != 0) {
          if (_client_state == client_state::Running) {
            LOG(ERROR) << this << " asio error: " << ec.message();
            _callbacks.on_error(client_error::AsioError);
          } else {
            LOG(INFO) << this << " ignoring asio error: " << ec.message()
                      << " because in state " << _client_state;
          }

          return;
        }

        LOG(2) << this << " requesting another ping";
        arm_ping_timer();
      });
    });
  }

  void process_input(const nlohmann::json &document, size_t byte_size) {
    CHECK(document.is_object()) << "not an object: " << document;

    if (document.count("action") == 0) {
      LOG(ERROR) << "no action in pdu: " << document;
    }

    std::string action = document["action"];
    rtm_actions_received.Add({{"action", action}}).Increment();

    if (action == "rtm/subscription/data") {
      auto &body = document["body"];
      std::string subscription_id = body["subscription_id"];
      auto it = _subscriptions.find(subscription_id);
      CHECK(it != _subscriptions.end());
      subscription_impl &sub = it->second;
      CHECK(sub.status == subscription_status::Current
            || sub.status == subscription_status::PendingUnsubscribe);
      if (sub.status == subscription_status::PendingUnsubscribe) {
        LOG(2) << "Got data for subscription pending deletion";
        return;
      }

      rtm_messages_received.Add({{"channel", sub.channel}}).Increment();
      rtm_messages_bytes_received.Add({{"channel", sub.channel}}).Increment(byte_size);

      for (auto &m : body["messages"]) {
        sub.callbacks.on_data(sub.sub, cbor_move(video::json_to_cbor(m)));
      }
    } else if (action == "rtm/subscribe/ok") {
      const uint64_t id = document["id"];
      for (auto &it : _subscriptions) {
        const std::string &sub_id = it.first;
        subscription_impl &sub = it.second;
        if (sub.pending_request_id == id) {
          LOG(1) << "got subscribe confirmation for subscription " << sub_id
                 << " in status " << std::to_string((int)sub.status) << ": " << document;
          CHECK(sub.status == subscription_status::PendingSubscribe);
          sub.pending_request_id = UINT64_MAX;
          sub.status = subscription_status::Current;
          return;
        }
      }
      ABORT() << "got unexpected subscribe confirmation: " << document;
    } else if (action == "rtm/subscribe/error") {
      const uint64_t id = document["id"];
      for (auto it = _subscriptions.begin(); it != _subscriptions.end(); ++it) {
        const std::string &sub_id = it->first;
        subscription_impl &sub = it->second;
        if (sub.pending_request_id == id) {
          LOG(ERROR) << "got subscribe error for subscription " << sub_id << " in status "
                     << std::to_string((int)sub.status) << ": " << document;
          CHECK(sub.status == subscription_status::PendingSubscribe);
          _callbacks.on_error(client_error::SubscribeError);
          _subscriptions.erase(it);
          return;
        }
      }
      ABORT() << "got unexpected subscribe error: " << document;
    } else if (action == "rtm/unsubscribe/ok") {
      const uint64_t id = document["id"];
      for (auto it = _subscriptions.begin(); it != _subscriptions.end(); ++it) {
        const std::string &sub_id = it->first;
        subscription_impl &sub = it->second;
        if (sub.pending_request_id == id) {
          LOG(1) << "got unsubscribe confirmation for subscription " << sub_id
                 << " in status " << std::to_string((int)sub.status) << ": " << document;
          CHECK(sub.status == subscription_status::PendingUnsubscribe);
          it = _subscriptions.erase(it);
          return;
        }
      }
      ABORT() << "got unexpected unsubscribe confirmation: " << document;
    } else if (action == "rtm/unsubscribe/error") {
      const uint64_t id = document["id"];  // check type
      for (auto it = _subscriptions.begin(); it != _subscriptions.end(); ++it) {
        const std::string &sub_id = it->first;
        subscription_impl &sub = it->second;
        if (sub.pending_request_id == id) {
          LOG(ERROR) << "got unsubscribe error for subscription " << sub_id
                     << " in status " << std::to_string((int)sub.status) << ": "
                     << document;
          CHECK(sub.status == subscription_status::PendingUnsubscribe);
          _callbacks.on_error(client_error::UnsubscribeError);
          _subscriptions.erase(it);
          return;
        }
      }
      ABORT() << "got unexpected unsubscribe error: " << document;
    } else if (action == "rtm/subscription/error") {
      LOG(ERROR) << "subscription error: " << document;
      _callbacks.on_error(client_error::SubscriptionError);
    } else {
      ABORT() << "unsupported action: " << document;
    }
  }

  std::atomic<client_state> _client_state{client_state::Stopped};

  const std::string _host;
  const std::string _port;
  const std::string _appkey;
  const uint64_t _client_id;
  error_callbacks &_callbacks;

  asio::ip::tcp::resolver _tcp_resolver;
  boost::beast::websocket::stream<boost::asio::ssl::stream<boost::asio::ip::tcp::socket> >
      _ws;
  uint64_t _request_id{0};
  boost::beast::multi_buffer _read_buffer{READ_BUFFER_SIZE};
  std::map<std::string, subscription_impl> _subscriptions;
  boost::asio::deadline_timer _ping_timer;
  std::function<void(boost::beast::websocket::frame_type type,
                     boost::beast::string_view payload)>
      _control_callback;
};

}  // namespace

std::unique_ptr<client> new_client(const std::string &endpoint, const std::string &port,
                                   const std::string &appkey,
                                   asio::io_service &io_service,
                                   asio::ssl::context &ssl_ctx, size_t id,
                                   error_callbacks &callbacks) {
  LOG(1) << "Creating RTM client for " << endpoint << ":" << port << "?appkey=" << appkey;
  std::unique_ptr<secure_client> client(
      new secure_client(endpoint, port, appkey, id, callbacks, io_service, ssl_ctx));
  return std::move(client);
}

resilient_client::resilient_client(asio::io_service &io_service,
                                   std::thread::id io_thread_id,
                                   resilient_client::client_factory_t &&factory,
                                   error_callbacks &callbacks)
    : _io(io_service),
      _io_thread_id(io_thread_id),
      _factory(std::move(factory)),
      _error_callbacks(callbacks) {}

void resilient_client::publish(const std::string &channel, cbor_item_t *message,
                               publish_callbacks *callbacks) {
  CHECK_EQ(std::this_thread::get_id(), _io_thread_id)
      << "Invocation from " << threadutils::get_current_thread_name();

  _client->publish(channel, message, callbacks);
}

void resilient_client::subscribe_channel(const std::string &channel,
                                         const subscription &sub,
                                         subscription_callbacks &callbacks,
                                         const subscription_options *options) {
  CHECK_EQ(std::this_thread::get_id(), _io_thread_id)
      << "Invocation from " << threadutils::get_current_thread_name();

  _subscriptions.push_back({channel, &sub, &callbacks, options});
  _client->subscribe_channel(channel, sub, callbacks, options);
}

void resilient_client::subscribe_filter(const std::string & /*filter*/,
                                        const subscription & /*sub*/,
                                        subscription_callbacks & /*callbacks*/,
                                        const subscription_options * /*options*/) {
  CHECK_EQ(std::this_thread::get_id(), _io_thread_id)
      << "Invocation from " << threadutils::get_current_thread_name();

  ABORT() << "not implemented";
}

void resilient_client::unsubscribe(const subscription &sub) {
  CHECK_EQ(std::this_thread::get_id(), _io_thread_id)
      << "Invocation from " << threadutils::get_current_thread_name();

  _client->unsubscribe(sub);
  std::remove_if(_subscriptions.begin(), _subscriptions.end(),
                 [&sub](const subscription_info &si) { return &sub == si.sub; });
}

channel_position resilient_client::position(const subscription &sub) {
  CHECK_EQ(std::this_thread::get_id(), _io_thread_id)
      << "Invocation from " << threadutils::get_current_thread_name();

  return _client->position(sub);
}

bool resilient_client::is_up(const subscription &sub) {
  CHECK_EQ(std::this_thread::get_id(), _io_thread_id)
      << "Invocation from " << threadutils::get_current_thread_name();

  return _client->is_up(sub);
}

std::error_condition resilient_client::start() {
  CHECK_EQ(std::this_thread::get_id(), _io_thread_id)
      << "Invocation from " << threadutils::get_current_thread_name();

  if (!_client) {
    LOG(1) << "creating new client";
    _client = _factory(*this);
  }

  _started = true;
  return _client->start();
}

std::error_condition resilient_client::stop() {
  CHECK_EQ(std::this_thread::get_id(), _io_thread_id)
      << "Invocation from " << threadutils::get_current_thread_name();

  _started = false;
  return _client->stop();
}

void resilient_client::on_error(std::error_condition ec) {
  CHECK_EQ(std::this_thread::get_id(), _io_thread_id)
      << "Invocation from " << threadutils::get_current_thread_name();

  LOG(INFO) << "restarting rtm client because of error: " << ec.message();
  restart();
}

void resilient_client::restart() {
  CHECK_EQ(std::this_thread::get_id(), _io_thread_id)
      << "Invocation from " << threadutils::get_current_thread_name();

  LOG(1) << "creating new client";
  _client = _factory(*this);
  if (!_started) {
    return;
  }

  LOG(1) << "starting new client";
  auto ec = _client->start();
  if (ec) {
    LOG(ERROR) << "can't restart client: " << ec.message();
    _error_callbacks.on_error(ec);
    return;
  }

  LOG(1) << "restoring subscriptions";
  for (const auto &sub : _subscriptions) {
    _client->subscribe_channel(sub.channel, *sub.sub, *sub.callbacks, sub.options);
  }

  LOG(1) << "client restart done";
}

thread_checking_client::thread_checking_client(asio::io_service &io,
                                               std::thread::id io_thread_id,
                                               std::unique_ptr<client> client)
    : _io(io), _io_thread_id(io_thread_id), _client(std::move(client)) {}

void thread_checking_client::publish(const std::string &channel, cbor_item_t *message,
                                     publish_callbacks *callbacks) {
  if (std::this_thread::get_id() != _io_thread_id) {
    LOG(WARNING) << "Forwarding request from thread "
                 << threadutils::get_current_thread_name();
    _io.post([this, channel, message, callbacks]() {
      _client->publish(channel, message, callbacks);
    });
    return;
  }

  _client->publish(channel, message, callbacks);
}

void thread_checking_client::subscribe_channel(const std::string &channel,
                                               const subscription &sub,
                                               subscription_callbacks &callbacks,
                                               const subscription_options *options) {
  if (std::this_thread::get_id() != _io_thread_id) {
    LOG(WARNING) << "Forwarding request from thread "
                 << threadutils::get_current_thread_name();
    _io.post([this, channel, &sub, &callbacks, options]() {
      _client->subscribe_channel(channel, sub, callbacks, options);
    });
    return;
  }

  _client->subscribe_channel(channel, sub, callbacks, options);
}

void thread_checking_client::subscribe_filter(const std::string &filter,
                                              const subscription &sub,
                                              subscription_callbacks &callbacks,
                                              const subscription_options *options) {
  if (std::this_thread::get_id() != _io_thread_id) {
    LOG(WARNING) << "Forwarding request from thread "
                 << threadutils::get_current_thread_name();
    _io.post([this, filter, &sub, &callbacks, options]() {
      _client->subscribe_filter(filter, sub, callbacks, options);
    });
    return;
  }

  _client->subscribe_filter(filter, sub, callbacks, options);
}

void thread_checking_client::unsubscribe(const subscription &sub) {
  if (std::this_thread::get_id() != _io_thread_id) {
    LOG(5) << "Forwarding request from thread " << threadutils::get_current_thread_name();
    _io.post([this, &sub]() { _client->unsubscribe(sub); });
    return;
  }

  _client->unsubscribe(sub);
}

channel_position thread_checking_client::position(const subscription & /*sub*/) {
  ABORT() << "not implemented";
  return channel_position{0, 0};
}

bool thread_checking_client::is_up(const subscription & /*sub*/) {
  ABORT() << "not implemented";
  return false;
}

std::error_condition thread_checking_client::start() {
  CHECK_EQ(std::this_thread::get_id(), _io_thread_id)
      << "Invocation from " << threadutils::get_current_thread_name();

  return _client->start();
}

std::error_condition thread_checking_client::stop() {
  CHECK_EQ(std::this_thread::get_id(), _io_thread_id)
      << "Invocation from " << threadutils::get_current_thread_name();

  return _client->stop();
}

}  // namespace rtm
}  // namespace video
}  // namespace satori
