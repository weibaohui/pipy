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
#include "log.hpp"

#ifdef __linux__
#include <linux/netfilter_ipv4.h>
#include <linux/ip.h>
#include <linux/udp.h>
#endif // __linux__

namespace pipy {

using tcp = asio::ip::tcp;

//
// Inbound
//

thread_local static Data::Producer s_dp_tcp("InboundTCP");
thread_local static Data::Producer s_dp_udp("InboundUDP Raw");

std::atomic<uint64_t> Inbound::s_inbound_id;

thread_local pjs::Ref<stats::Gauge> Inbound::s_metric_concurrency;
thread_local pjs::Ref<stats::Counter> Inbound::s_metric_traffic_in;
thread_local pjs::Ref<stats::Counter> Inbound::s_metric_traffic_out;

Inbound::Inbound() {
  init_metrics();
  Log::debug(Log::ALLOC, "[inbound  %p] ++", this);
  for (;;) {
    if (auto id = s_inbound_id.fetch_add(1, std::memory_order_relaxed) + 1) {
      m_id = id;
      break;
    }
  }
}

Inbound::~Inbound() {
  Log::debug(Log::ALLOC, "[inbound  %p] --", this);
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

void Inbound::on_weak_ptr_gone() {
  m_output = nullptr;
}

void Inbound::init_metrics() {
  if (!s_metric_concurrency) {
    pjs::Ref<pjs::Array> label_names = pjs::Array::make();
    label_names->length(2);
    label_names->set(0, "listen");
    label_names->set(1, "peer");

    s_metric_concurrency = stats::Gauge::make(
      pjs::Str::make("pipy_inbound_count"),
      label_names,
      [=](stats::Gauge *gauge) {
        int total = 0;
        Listener::for_each([&](Listener *listener) {
          if (auto *p = listener->pipeline_layout()) {
            auto k = p->name_or_label();
            auto l = gauge->with_labels(&k, 1);
            if (listener->options().peer_stats) {
              auto n = 0;
              l->zero_all();
              listener->for_each_inbound([&](Inbound *inbound) {
                auto k = inbound->remote_address();
                auto m = l->with_labels(&k, 1);
                m->increase();
                n++;
              });
              l->set(n);
              total += n;
            } else {
              auto n = listener->current_connections();
              l->set(n);
              total += n;
            }
          }
        });
        gauge->set(total);
      }
    );

    s_metric_traffic_in = stats::Counter::make(
      pjs::Str::make("pipy_inbound_in"),
      label_names,
      [](stats::Counter *counter) {
        Listener::for_each([&](Listener *listener) {
          listener->for_each_inbound([&](Inbound *inbound) {
            auto n = inbound->get_traffic_in();
            inbound->m_metric_traffic_in->increase(n);
            s_metric_traffic_in->increase(n);
          });
        });
      }
    );

    s_metric_traffic_out = stats::Counter::make(
      pjs::Str::make("pipy_inbound_out"),
      label_names,
      [](stats::Counter *counter) {
        Listener::for_each([&](Listener *listener) {
          listener->for_each_inbound([&](Inbound *inbound) {
            auto n = inbound->get_traffic_out();
            inbound->m_metric_traffic_out->increase(n);
            s_metric_traffic_out->increase(n);
          });
        });
      }
    );
  }
}

//
// InboundTCP
//

InboundTCP::InboundTCP(Listener *listener, const Inbound::Options &options)
  : SocketTCP(true, options)
  , m_listener(listener)
  , m_options(options)
{
}

InboundTCP::~InboundTCP() {
  if (m_listener) {
    m_listener->close(this);
  }
}

void InboundTCP::accept(asio::ip::tcp::acceptor &acceptor) {
  acceptor.async_accept(
    socket(), m_peer,
    [this](const std::error_code &ec) {
      InputContext ic(this);

      if (ec == asio::error::operation_aborted) {
        dangle();
      } else {
        if (ec) {
          log_error("error accepting connection", ec);
          dangle();

        } else {
          log_debug("connection accepted");

          if (m_listener && m_listener->pipeline_layout()) {
            start();
          }
        }
      }

      release();
    }
  );

  retain();
}

auto InboundTCP::get_traffic_in() -> size_t {
  auto n = SocketTCP::m_traffic_read;
  SocketTCP::m_traffic_read = 0;
  return n;
}

auto InboundTCP::get_traffic_out() -> size_t {
  auto n = SocketTCP::m_traffic_write;
  SocketTCP::m_traffic_write = 0;
  return n;
}

void InboundTCP::on_get_address() {
  auto &s = SocketTCP::socket();
  if (s.is_open()) {
    const auto &ep = s.local_endpoint();
    m_local_addr = ep.address().to_string();
    m_local_port = ep.port();
  }

  m_remote_addr = m_peer.address().to_string();
  m_remote_port = m_peer.port();

#ifdef __linux__
  if (m_options.transparent && s.is_open()) {
    struct sockaddr addr;
    socklen_t len = sizeof(addr);
    if (!getsockopt(s.native_handle(), SOL_IP, SO_ORIGINAL_DST, &addr, &len)) {
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
  }
#endif // __linux__
}

void InboundTCP::start() {
  Inbound::start(m_listener->pipeline_layout());

  auto p = pipeline();
  p->chain(EventTarget::input());
  m_input = p->input();
  m_listener->open(this);

  int n = 1;
  pjs::Str *labels[2];
  labels[0] = m_listener->pipeline_layout()->name_or_label();
  if (m_options.peer_stats) labels[n++] = remote_address();

  m_metric_traffic_in = Inbound::s_metric_traffic_in->with_labels(labels, n);
  m_metric_traffic_out = Inbound::s_metric_traffic_out->with_labels(labels, n);

  p->start();

  SocketTCP::start();
}

void InboundTCP::describe(char *buf, size_t len) {
  address();
  if (m_options.transparent) {
    std::snprintf(
      buf, len,
      "[inbound  %p] [%s]:%d -> [%s]:%d -> [%s]:%d",
      this,
      m_remote_addr.c_str(),
      m_remote_port,
      m_local_addr.c_str(),
      m_local_port,
      m_ori_dst_addr.c_str(),
      m_ori_dst_port
    );
  } else {
    std::snprintf(
      buf, len,
      "[inbound  %p] [%s]:%d -> [%s]:%d",
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

InboundUDP::InboundUDP(
  Listener* listener,
  const Options &options,
  asio::ip::udp::socket &socket,
  asio::generic::raw_protocol::socket &socket_raw,
  const asio::ip::udp::endpoint &local,
  const asio::ip::udp::endpoint &peer,
  const asio::ip::udp::endpoint &destination
) : m_listener(listener)
  , m_options(options)
  , m_socket_raw(socket_raw)
  , m_socket(socket)
  , m_local(local)
  , m_peer(peer)
  , m_destination(destination)
{
  listener->open(this);
#ifdef __linux__
  if (m_options.masquerade) {
    auto &src = destination.port() ? destination : local;
    auto &dst = peer;
    auto &ip = *reinterpret_cast<struct iphdr*>(m_datagram_header);
    auto &udp = *reinterpret_cast<struct udphdr*>(m_datagram_header + 20);
    ip.version = 4;
    ip.ihl = 20/4;
    ip.tos = 0;
    ip.tot_len = 0;
    ip.id = 0;
    ip.frag_off = 0;
    ip.ttl = 23;
    ip.protocol = 17; // UDP
    ip.check = 0;
    ip.saddr = htonl(src.address().to_v4().to_uint());
    ip.daddr = htonl(dst.address().to_v4().to_uint());
    udp.source = htons(src.port());
    udp.dest = htons(dst.port());
    udp.len = 0;
    udp.check = 0;
  }
#endif // __linux__
}

InboundUDP::~InboundUDP() {
  if (m_listener) {
    m_listener->close(this);
  }
}

void InboundUDP::start() {
  if (!pipeline()) {
    retain();
    Inbound::start(m_listener->pipeline_layout());
    auto p = pipeline();
    p->chain(EventTarget::input());
    m_input = p->input();
    p->start();
    wait_idle();
  }
}

void InboundUDP::receive(Data *data) {
  start();
  wait_idle();
  m_input->input(MessageStart::make());
  m_input->input(data);
  m_input->input(MessageEnd::make());
}

void InboundUDP::stop() {
  m_idle_timer.cancel();
  Inbound::stop();
  release();
}

auto InboundUDP::get_buffered() const -> size_t {
  return m_buffer.size() + m_sending_size;
}

auto InboundUDP::get_traffic_in() -> size_t {
  return 0;
}

auto InboundUDP::get_traffic_out() -> size_t {
  return 0;
}

void InboundUDP::on_get_address() {
  if (m_listener) {
    m_local_addr = m_local.address().to_string();
    m_local_port = m_local.port();
    m_remote_addr = m_peer.address().to_string();
    m_remote_port = m_peer.port();
    if (m_options.transparent) {
      m_ori_dst_addr = m_destination.address().to_string();
      m_ori_dst_port = m_destination.port();
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
      if (m_listener) {
#ifdef __linux__
        if (m_options.masquerade) {
          auto *buf = Data::make();
          auto *ip = reinterpret_cast<struct iphdr *>(m_datagram_header);
          auto *udp = reinterpret_cast<struct udphdr *>(m_datagram_header + 20);
          auto size = m_buffer.size();
          ip->tot_len = htons(20 + 8 + size);
          udp->len = htons(8 + size);
          buf->push(m_datagram_header, sizeof(m_datagram_header), &s_dp_udp);
          buf->push(std::move(m_buffer));
          m_socket_raw.async_send_to(
            DataChunks(buf->chunks()),
            m_peer,
            [=](const std::error_code &ec, std::size_t n) {
              m_sending_size -= size;
              buf->release();
            }
          );
          m_sending_size += size;
          buf->retain();

        } else
#endif // __linux__
        {
          auto *buf = Data::make(std::move(m_buffer));
          auto size = m_buffer.size();
          m_socket.async_send_to(
            DataChunks(buf->chunks()),
            m_peer,
            [=](const std::error_code &ec, std::size_t n) {
              m_sending_size -= size;
              buf->release();
            }
          );
          m_sending_size += size;
          buf->retain();
        }
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

template<> void ClassDef<InboundTCP>::init() {
  super<Inbound>();
}

template<> void ClassDef<InboundUDP>::init() {
  super<Inbound>();
}

} // namespace pjs
