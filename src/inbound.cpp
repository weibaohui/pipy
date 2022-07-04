/*
 *  Copyright (c) 2019 by flomesh.io
 *
 *  Unless prior written consent has been obtained from the copyright
 *  owner, the following shall not be allowed.
 *
 *  1. The distribution of any source codes, header files, make files,
 *     or libraries of the software.
 *
 *  2. Disclosure of any source codes pertaining to the software to any
 *     additional parties.
 *
 *  3. Alteration or removal of any notices in or on the software or
 *     within the documentation included within the software.
 *
 *  ALL SOURCE CODE AS WELL AS ALL DOCUMENTATION INCLUDED WITH THIS
 *  SOFTWARE IS PROVIDED IN AN “AS IS” CONDITION, WITHOUT WARRANTY OF ANY
 *  KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 *  OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 *  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 *  CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 *  TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 *  SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "inbound.hpp"
#include "listener.hpp"
#include "pipeline.hpp"
#include "worker.hpp"
#include "constants.hpp"
#include "status.hpp"
#include "log.hpp"

#ifdef __linux__
#include <linux/netfilter_ipv4.h>
#endif

namespace pipy {

using tcp = asio::ip::tcp;

uint64_t Inbound::s_inbound_id = 0;

//
// Inbound
//

Inbound::Inbound() {
  Log::debug("[inbound  %p] ++", this);
  if (!++s_inbound_id) s_inbound_id++;
  m_id = s_inbound_id;
}

Inbound::~Inbound() {
  Log::debug("[inbound  %p] --", this);
  if (m_pipeline) {
    Pipeline::auto_release(m_pipeline);
  }
}

auto Inbound::output() -> Output* {
  if (!m_output) {
    m_output = Output::make(EventTarget::input());
    Output::WeakPtr::Watcher::watch(m_output.get());
  }
  return m_output.ptr();
}

auto Inbound::local_address() -> pjs::Str* {
  if (!m_str_local_addr) {
    address();
    m_str_local_addr = pjs::Str::make(m_local_addr);
  }
  return m_str_local_addr;
}

auto Inbound::remote_address() -> pjs::Str* {
  if (!m_str_remote_addr) {
    address();
    m_str_remote_addr = pjs::Str::make(m_remote_addr);
  }
  return m_str_remote_addr;
}

auto Inbound::ori_dst_address() -> pjs::Str* {
  if (!m_str_ori_dst_addr) {
    address();
    m_str_ori_dst_addr = pjs::Str::make(m_ori_dst_addr);
  }
  return m_str_ori_dst_addr;
}

void Inbound::start(PipelineLayout *layout) {
  if (!m_pipeline) {
    auto ctx = layout->new_context();
    ctx->m_inbound = this;
    m_pipeline = Pipeline::make(layout, ctx);
  }
}

void Inbound::stop() {
  m_pipeline = nullptr;
}

void Inbound::address() {
  if (!m_addressed) {
    on_get_address();
    m_addressed = true;
  }
}

void Inbound::get_original_dest(int sock) {
#ifdef __linux__
  struct sockaddr addr;
  socklen_t len = sizeof(addr);
  if (!getsockopt(sock, SOL_IP, SO_ORIGINAL_DST, &addr, &len)) {
    char str[100];
    auto n = std::sprintf(
      str, "%d.%d.%d.%d",
      (unsigned char)addr.sa_data[2],
      (unsigned char)addr.sa_data[3],
      (unsigned char)addr.sa_data[4],
      (unsigned char)addr.sa_data[5]
    );
    m_ori_dst_addr.assign(str, n);
    m_ori_dst_port = (
      ((int)(unsigned char)addr.sa_data[0] << 8) |
      ((int)(unsigned char)addr.sa_data[1] << 0)
    );
  }
#endif // __linux__
}

void Inbound::on_tap_open() {
  switch (m_receiving_state) {
    case PAUSING:
      m_receiving_state = RECEIVING;
      break;
    case PAUSED:
      m_receiving_state = RECEIVING;
      on_inbound_resume();
      release();
      break;
    default: break;
  }
}

void Inbound::on_tap_close() {
  if (m_receiving_state == RECEIVING) {
    m_receiving_state = PAUSING;
  }
}

void Inbound::on_weak_ptr_gone() {
  m_output = nullptr;
}

//
// InboundTCP
//

InboundTCP::InboundTCP(Listener *listener, const Options &options)
  : FlushTarget(true)
  , m_listener(listener)
  , m_options(options)
  , m_socket(Net::context())
{
}

InboundTCP::~InboundTCP() {
  if (m_listener) {
    m_listener->close(this);
  }
}

void InboundTCP::accept(asio::ip::tcp::acceptor &acceptor) {
  acceptor.async_accept(
    m_socket, m_peer,
    [this](const std::error_code &ec) {
      if (ec == asio::error::operation_aborted) {
        dangle();
      } else {
        if (ec) {
          if (Log::is_enabled(Log::ERROR)) {
            char desc[200];
            describe(desc);
            Log::error("%s error accepting connection: %s", desc, ec.message().c_str());
          }
          dangle();

        } else {
          if (Log::is_enabled(Log::DEBUG)) {
            char desc[200];
            describe(desc);
            Log::debug("%s connection accepted", desc);
          }
          InputContext ic(this);
          start();
        }
      }

      release();
    }
  );

  retain();
}

void InboundTCP::on_get_address() {
  if (m_socket.is_open()) {
    const auto &ep = m_socket.local_endpoint();
    m_local_addr = ep.address().to_string();
    m_local_port = ep.port();
  }
  m_remote_addr = m_peer.address().to_string();
  m_remote_port = m_peer.port();
  if (m_options.transparent && m_socket.is_open()) {
    get_original_dest(m_socket.native_handle());
  }
}

void InboundTCP::on_event(Event *evt) {
  if (!m_ended) {
    if (auto data = evt->as<Data>()) {
      if (data->size() > 0) {
        m_buffer.push(*data);
        need_flush();
      }

    } else if (auto end = evt->as<StreamEnd>()) {
      m_ended = true;
      if (m_buffer.empty()) {
        close(end->error());
      } else {
        pump();
      }
    }
  }
}

void InboundTCP::on_flush() {
  if (!m_ended) {
    pump();
  }
}

void InboundTCP::start() {
  Inbound::start(m_listener->pipeline_layout());

  auto p = pipeline();
  p->chain(EventTarget::input());
  m_input = p->input();
  m_listener->open(this);

  pjs::Str *labels[2];
  labels[0] = m_listener->pipeline_layout()->name();
  labels[1] = remote_address();
  m_metric_traffic_in = Status::metric_inbound_in->with_labels(labels, 2);
  m_metric_traffic_out = Status::metric_inbound_out->with_labels(labels, 2);

  receive();
}

void InboundTCP::receive() {
  if (!m_socket.is_open()) return;

  static Data::Producer s_data_producer("InboundTCP");
  pjs::Ref<Data> buffer(Data::make(RECEIVE_BUFFER_SIZE, &s_data_producer));

  m_socket.async_read_some(
    DataChunks(buffer->chunks()),
    [=](const std::error_code &ec, std::size_t n) {
      InputContext ic(this);

      if (m_options.read_timeout > 0) {
        m_read_timer.cancel();
      }

      if (ec != asio::error::operation_aborted) {
        if (n > 0) {
          buffer->pop(buffer->size() - n);
          if (m_socket.is_open()) {
            if (auto more = m_socket.available()) {
              Data buf(more, &s_data_producer);
              auto n = m_socket.read_some(DataChunks(buf.chunks()));
              if (n < more) buf.pop(more - n);
              buffer->push(buf);
            }
          }
          m_metric_traffic_in->increase(buffer->size());
          Status::metric_inbound_in->increase(buffer->size());
          output(buffer);
        }

        if (ec) {
          if (ec == asio::error::eof) {
            if (Log::is_enabled(Log::DEBUG)) {
              char desc[200];
              describe(desc);
              Log::debug("%s EOF from peer", desc);
            }
            linger();
            output(StreamEnd::make());
          } else if (ec == asio::error::connection_reset) {
            if (Log::is_enabled(Log::WARN)) {
              char desc[200];
              describe(desc);
              Log::warn("%s connection reset by peer", desc);
            }
            close(StreamEnd::CONNECTION_RESET);
          } else {
            if (Log::is_enabled(Log::WARN)) {
              char desc[200];
              describe(desc);
              Log::warn("%s error reading from peer: %s", desc, ec.message().c_str());
            }
            close(StreamEnd::READ_ERROR);
          }

        } else if (m_receiving_state == PAUSING) {
          m_receiving_state = PAUSED;
          retain();
          wait();

        } else if (m_receiving_state == RECEIVING) {
          receive();
          wait();
        }
      }

      release();
    }
  );

  if (m_options.read_timeout > 0) {
    m_read_timer.schedule(
      m_options.read_timeout,
      [this]() {
        close(StreamEnd::READ_TIMEOUT);
      }
    );
  }

  retain();
}

void InboundTCP::linger() {
  if (!m_socket.is_open()) return;

  m_socket.async_wait(
    tcp::socket::wait_error,
    [this](const std::error_code &ec) {
      if (ec != asio::error::operation_aborted) {
        char desc[200];
        describe(desc);
        Log::error("%s socket error: %s", desc, ec.message().c_str());
      }
      release();
    }
  );

  retain();
}

void InboundTCP::pump() {
  if (!m_socket.is_open()) return;
  if (m_pumping) return;
  if (m_buffer.empty()) return;

  m_socket.async_write_some(
    DataChunks(m_buffer.chunks()),
    [=](const std::error_code &ec, std::size_t n) {
      m_pumping = false;

      if (m_options.write_timeout > 0) {
        m_write_timer.cancel();
      }

      if (ec != asio::error::operation_aborted) {
        m_buffer.shift(n);
        m_metric_traffic_out->increase(n);
        Status::metric_inbound_out->increase(n);

        if (ec) {
          if (Log::is_enabled(Log::WARN)) {
            char desc[200];
            describe(desc);
            Log::warn("%s error writing to peer: %s", desc, ec.message().c_str());
          }
          close(StreamEnd::WRITE_ERROR);

        } else if (m_ended && m_buffer.empty()) {
          close(StreamEnd::NO_ERROR);

        } else {
          pump();
        }
      }

      release();
    }
  );

  if (m_options.write_timeout > 0) {
    m_write_timer.schedule(
      m_options.write_timeout,
      [this]() {
        close(StreamEnd::WRITE_TIMEOUT);
      }
    );
  }

  wait();
  retain();

  m_pumping = true;
}

void InboundTCP::wait() {
  if (!m_socket.is_open()) return;
  if (m_options.idle_timeout > 0) {
    m_idle_timer.cancel();
    m_idle_timer.schedule(
      m_options.idle_timeout,
      [this]() {
        close(StreamEnd::IDLE_TIMEOUT);
      }
    );
  }
}

void InboundTCP::output(Event *evt) {
  m_input->input(evt);
}

void InboundTCP::close(StreamEnd::Error err) {
  if (m_socket.is_open()) {
    std::error_code ec;
    if (err == StreamEnd::NO_ERROR) m_socket.shutdown(tcp::socket::shutdown_both, ec);
    m_socket.close(ec);

    if (ec) {
      if (Log::is_enabled(Log::ERROR)) {
        char desc[200];
        describe(desc);
        Log::error("%s error closing socket: %s", desc, ec.message().c_str());
      }
    } else {
      if (Log::is_enabled(Log::DEBUG)) {
        char desc[200];
        describe(desc);
        Log::debug("%s connection closed to peer", desc);
      }
    }
  }

  InputContext ic(this);
  output(StreamEnd::make(err));

  if (m_receiving_state == PAUSED) {
    m_receiving_state = RECEIVING;
    release();
  }
}

void InboundTCP::describe(char *buf) {
  address();
  if (m_options.transparent) {
    std::sprintf(
      buf, "[inbound  %p] [%s]:%d -> [%s]:%d -> [%s]:%d",
      this,
      m_remote_addr.c_str(),
      m_remote_port,
      m_local_addr.c_str(),
      m_local_port,
      m_ori_dst_addr.c_str(),
      m_ori_dst_port
    );
  } else {
    std::sprintf(
      buf, "[inbound  %p] [%s]:%d -> [%s]:%d",
      this,
      m_remote_addr.c_str(),
      m_remote_port,
      m_local_addr.c_str(),
      m_local_port
    );
  }
}

//
// InboundUDP
//

auto InboundUDP::get(int port, const std::string &peer) -> InboundUDP* {
  if (auto *listener = Listener::find(port, Listener::Protocol::UDP)) {
    if (auto *acceptor = listener->m_acceptor.get()) {
      std::string ip;
      int port;
      if (!utils::get_host_port(peer, ip, port)) return nullptr;
      asio::error_code ec;
      auto addr = asio::ip::make_address(ip, ec);
      if (ec) {
        std::string msg("invalid IP address: ");
        throw std::runtime_error(msg + ip);
      }
      asio::ip::udp::endpoint peer(addr, port);
      return static_cast<Listener::AcceptorUDP*>(acceptor)->inbound(peer, true);
    }
  }
  return nullptr;
}

InboundUDP::InboundUDP(
  Listener* listener,
  const Options &options,
  asio::ip::udp::socket &socket,
  const asio::ip::udp::endpoint &peer
) : m_listener(listener)
  , m_options(options)
  , m_socket(socket)
  , m_peer(peer)
{
  listener->open(this);
}

InboundUDP::~InboundUDP() {
  if (m_listener) {
    m_listener->close(this);
  }
}

void InboundUDP::start() {
  if (!pipeline()) {
    Inbound::start(m_listener->pipeline_layout());
    auto p = pipeline();
    p->chain(EventTarget::input());
    m_input = p->input();
    wait_idle();
  }
}

void InboundUDP::receive(Data *data) {
  start();
  wait_idle();
  InputContext ic;
  m_input->input(MessageStart::make());
  m_input->input(data);
  m_input->input(MessageEnd::make());
}

void InboundUDP::stop() {
  m_idle_timer.cancel();
  Inbound::stop();
}

auto InboundUDP::size_in_buffer() const -> size_t {
  return m_buffer.size() + m_sending_size;
}

void InboundUDP::on_get_address() {
  if (m_listener) {
    if (m_socket.is_open()) {
      const auto &ep = m_socket.local_endpoint();
      m_local_addr = ep.address().to_string();
      m_local_port = ep.port();
    }
    m_remote_addr = m_peer.address().to_string();
    m_remote_port = m_peer.port();
    if (m_options.transparent && m_socket.is_open()) {
      get_original_dest(m_socket.native_handle());
    }
  }
}

void InboundUDP::on_event(Event *evt) {
  wait_idle();

  if (evt->is<MessageStart>()) {
    m_message_started = true;
    m_buffer.clear();

  } else if (auto *data = evt->as<Data>()) {
    if (m_message_started) {
      m_buffer.push(*data);
    }

  } else if (evt->is<MessageEnd>()) {
    if (m_message_started) {
      m_message_started = false;
      m_sending_size += m_buffer.size();

      pjs::Ref<Data> buffer(Data::make(std::move(m_buffer)));

      if (m_listener) {
        m_socket.async_send_to(
          DataChunks(buffer->chunks()),
          m_peer,
          [=](const std::error_code &ec, std::size_t n) {
            m_sending_size -= buffer->size();
            release();
          }
        );

        retain();
      }
    }
  }
}

void InboundUDP::wait_idle() {
  if (m_options.idle_timeout > 0) {
    m_idle_timer.schedule(
      m_options.idle_timeout,
      [this]() { stop(); }
    );
  }
}

} // namespace pipy

namespace pjs {

using namespace pipy;

template<> void ClassDef<Inbound>::init() {
  accessor("id"                 , [](Object *obj, Value &ret) { ret.set(obj->as<Inbound>()->id()); });
  accessor("localAddress"       , [](Object *obj, Value &ret) { ret.set(obj->as<Inbound>()->local_address()); });
  accessor("localPort"          , [](Object *obj, Value &ret) { ret.set(obj->as<Inbound>()->local_port()); });
  accessor("remoteAddress"      , [](Object *obj, Value &ret) { ret.set(obj->as<Inbound>()->remote_address()); });
  accessor("remotePort"         , [](Object *obj, Value &ret) { ret.set(obj->as<Inbound>()->remote_port()); });
  accessor("destinationAddress" , [](Object *obj, Value &ret) { ret.set(obj->as<Inbound>()->ori_dst_address()); });
  accessor("destinationPort"    , [](Object *obj, Value &ret) { ret.set(obj->as<Inbound>()->ori_dst_port()); });
  accessor("output"             , [](Object *obj, Value &ret) { ret.set(obj->as<Inbound>()->output()); });
}

template<> void ClassDef<Constructor<Inbound>>::init() {
  ctor();

  method("udp", [](Context &ctx, Object *obj, Value &ret) {
    int port;
    Str *peer;
    if (!ctx.arguments(2, &port, &peer)) return;
    ret.set(InboundUDP::get(port, peer->str()));
  });
}

template<> void ClassDef<InboundTCP>::init() {
  super<Inbound>();
}

template<> void ClassDef<InboundUDP>::init() {
  super<Inbound>();
}

} // namespace pjs
