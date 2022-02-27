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

#ifndef OPTIONS_HPP
#define OPTIONS_HPP

#include "api/crypto.hpp"
#include "logging.hpp"

#include <list>
#include <string>

namespace pipy {

//
// Options
//

class Options {
public:
  static void show_help();

  Options(int argc, char *argv[]);

  std::string filename;
  bool        version = false;
  bool        help = false;
  bool        verify = false;
  bool        reuse_port = false;
  int         admin_port = 0;
  Log::Level  log_level = Log::ERROR;
  std::string openssl_engine;

  pjs::Ref<crypto::Certificate> admin_tls_cert;
  pjs::Ref<crypto::PrivateKey>  admin_tls_key;
  pjs::Ref<pjs::Array>          admin_tls_trusted;
  pjs::Ref<crypto::Certificate> tls_cert;
  pjs::Ref<crypto::PrivateKey>  tls_key;
  pjs::Ref<pjs::Array>          tls_trusted;

private:
  auto load_private_key(const std::string &filename) -> crypto::PrivateKey*;
  auto load_certificate(const std::string &filename) -> crypto::Certificate*;
  auto load_certificate_list(const std::string &filename) -> pjs::Array*;
};

} // namespace pipy

#endif // OPTIONS_HPP
