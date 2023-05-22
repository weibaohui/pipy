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

#include "compress-message.hpp"
#include "compressor.hpp"
#include "data.hpp"

namespace pipy {

thread_local static const pjs::ConstStr s_headers("headers");
thread_local static const pjs::ConstStr s_content_encoding("content-encoding");
thread_local static const pjs::ConstStr s_gzip("gzip");
thread_local static const pjs::ConstStr s_br("br");
thread_local static const pjs::ConstStr s_inflate("inflate");
thread_local static const pjs::ConstStr s_deflate("deflate");
thread_local static const pjs::ConstStr s_brotli("brotli");

thread_local static Data::Producer s_dp("compressMessage()");

//
// Compress
//

Compress::Compress(const pjs::Value &algorithm)
  : m_algorithm(algorithm)
{
}

Compress::Compress(const Compress &r)
  : Filter(r)
  , m_algorithm(r.m_algorithm)
{
}

Compress::~Compress()
{
}

void Compress::dump(Dump &d) {
  Filter::dump(d);
  d.name = "compress";
}

auto Compress::clone() -> Filter* {
  return new Compress(*this);
}

void Compress::reset() {
  Filter::reset();
  if (m_compressor) {
    m_compressor->finalize();
    m_compressor = nullptr;
  }
  m_is_started = false;
}

void Compress::process(Event *evt) {
  if (!m_is_started) {
    m_is_started = true;
    pjs::Value algorithm;
    if (!Filter::eval(m_algorithm, algorithm)) return;
    if (!algorithm.is_string()) {
      Filter::error("algorithm is not or did not return a string");
      return;
    }
    auto out = [this](Data &data) { compressor_output(data); };
    auto str = algorithm.s();
    if (str == s_deflate) {
      m_compressor = Compressor::deflate(out);
    } else if (str == s_gzip) {
      m_compressor = Compressor::gzip(out);
    } else {
      Filter::error("unknown compression algorithm: %s", str->c_str());
      return;
    }
  }

  if (m_compressor) {
    if (auto data = evt->as<Data>()) {
      m_compressor->input(*data, false);
    } else if (evt->is<StreamEnd>()) {
      m_compressor->flush();
      m_compressor->finalize();
      m_compressor = nullptr;
    }
  }

  if (evt->is<StreamEnd>()) {
    Filter::output(evt);
  }
}

void Compress::compressor_output(Data &data) {
  Filter::output(Data::make(std::move(data)));
}

//
// CompressMessageBase::Options
//

CompressMessageBase::Options::Options(pjs::Object *options) {
  Value(options, "method")
    .get_enum(method)
    .get(method_f)
    .check_nullable();
  Value(options, "level")
    .get_enum(level)
    .get(level_f)
    .check_nullable();
}

//
// CompressMessageBase
//

CompressMessageBase::CompressMessageBase(const Options &options)
  : m_options(options)
{
}

CompressMessageBase::CompressMessageBase(const CompressMessageBase &r)
  : Filter(r)
  , m_options(r.m_options)
{
  m_output = [this](Data &data) {
    output(Data::make(std::move(data)));
  };
}

void CompressMessageBase::reset() {
  Filter::reset();
  if (m_compressor) {
    m_compressor->finalize();
    m_compressor = nullptr;
  }
  m_message_started = false;
}

void CompressMessageBase::process(Event *evt) {
  if (auto start = evt->as<MessageStart>()) {
    if (!m_message_started) {
      Method method;
      Level level;
      m_compressor = new_compressor(start, method, level, m_output);
      m_message_started = true;
    }

  } else if (auto *data = evt->as<Data>()) {
    if (m_compressor) {
      m_compressor->input(*data, false);
    } else {
      output(evt);
    }

  } else if (evt->is<MessageEnd>()) {
    if (m_compressor) {
      m_compressor->flush();
      m_compressor->finalize();
      m_compressor = nullptr;
    }
    m_message_started = false;
  }

  output(evt);
}

auto CompressMessageBase::new_compressor(
  MessageStart *start,
  Method &method,
  Level &level,
  const std::function<void(Data&)> &out
) -> Compressor* {

  method = Method::NO_COMPRESSION;
  level = Level::DEFAULT;

  if (m_options.method_f) {
    pjs::Value v;
    if (!eval(m_options.method_f, v)) return nullptr;
    if (v.is_string()) {
      method = pjs::EnumDef<Method>::value(v.s());
      if (int(method) < 0) {
        Filter::error("invalid method: %s", v.s()->c_str());
        return nullptr;
      }
    } else {
      Filter::error("invalid non-string method name");
      return nullptr;
    }
  } else {
    method = m_options.method;
  }

  if (m_options.level_f) {
    pjs::Value v;
    if (!eval(m_options.level_f, v)) return nullptr;
    if (v.is_string()) {
      level = pjs::EnumDef<Level>::value(v.s());
      if (int(level) < 0) {
        Filter::error("invalid level: %s", v.s()->c_str());
        return nullptr;
      }
    } else {
      Filter::error("invalid non-string level name");
      return nullptr;
    }
  } else {
    level = m_options.level;
  }

  switch (method) {
  case Method::DEFLATE:
    return Compressor::deflate(out);
  case Method::GZIP:
    return Compressor::gzip(out);
  default:
    return nullptr;
  }
}

//
// CompressMessage
//

CompressMessage::CompressMessage(const Options &options)
  : CompressMessageBase(options)
{
}

CompressMessage::CompressMessage(const CompressMessage &r)
  : CompressMessageBase(r)
{
}

CompressMessage::~CompressMessage()
{
}

void CompressMessage::dump(Dump &d) {
  Filter::dump(d);
  d.name = "compressMessage";
}

auto CompressMessage::clone() -> Filter * {
  return new CompressMessage(*this);
}

//
// CompressHTTP
//

CompressHTTP::CompressHTTP(const Options &options)
  : CompressMessageBase(options)
{
}

CompressHTTP::CompressHTTP(const CompressHTTP &r)
  : CompressMessageBase(r)
{
}

CompressHTTP::~CompressHTTP() {
}

void CompressHTTP::dump(Dump &d) {
  Filter::dump(d);
  d.name = "compressHTTP";
}

auto CompressHTTP::clone() -> Filter * {
  return new CompressHTTP(*this);
}

auto CompressHTTP::new_compressor(
  MessageStart *start,
  Method &method,
  Level &level,
  const std::function<void(Data&)> &out
) -> Compressor* {
  thread_local static pjs::ConstStr s_headers("headers");
  thread_local static pjs::ConstStr s_content_encoding("content-encoding");
  thread_local static pjs::ConstStr s_deflate("deflate");
  thread_local static pjs::ConstStr s_gzip("gzip");
  thread_local static pjs::ConstStr s_brotli("brotli");

  auto compressor = CompressMessageBase::new_compressor(start, method, level, out);
  if (compressor) {
    if (auto head = start->head()) {
      pjs::Value headers;
      head->get(s_headers, headers);
      if (headers.is_object()) {
        auto h = headers.o();
        switch (method) {
          case Method::DEFLATE: h->set(s_content_encoding, s_deflate.get()); break;
          case Method::GZIP: h->set(s_content_encoding, s_gzip.get()); break;
          case Method::BROTLI: h->set(s_content_encoding, s_brotli.get()); break;
          default: break;
        }
      }
    }
  }

  return compressor;
}

} // namespace pipy

namespace pjs {

using namespace pipy;

template<> void EnumDef<CompressMessageBase::Method>::init() {
  define(CompressMessageBase::Method::NO_COMPRESSION, "");
  define(CompressMessageBase::Method::DEFLATE, "deflate");
  define(CompressMessageBase::Method::GZIP, "gzip");
  define(CompressMessageBase::Method::BROTLI, "brotli");
}

template<> void EnumDef<CompressMessageBase::Level>::init() {
  define(CompressMessageBase::Level::DEFAULT, "default");
  define(CompressMessageBase::Level::SPEED, "speed");
  define(CompressMessageBase::Level::BEST, "best");
}

} // namespace pjs
