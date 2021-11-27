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

#include "version.h"

#include "admin-service.hpp"
#include "admin-proxy.hpp"
#include "codebase.hpp"
#include "filters/tls.hpp"
#include "listener.hpp"
#include "net.hpp"
#include "options.hpp"
#include "fs.hpp"
#include "status.hpp"
#include "timer.hpp"
#include "utils.hpp"
#include "worker.hpp"

#include <signal.h>

#include <list>
#include <string>
#include <tuple>

#include <openssl/opensslv.h>
#include <openssl/ssl.h>

using namespace pipy;

extern const char FILTERS_HELP[];

AdminService *s_admin = nullptr;
AdminProxy *s_admin_proxy = nullptr;

//
// Show version
//

static void show_version() {
  std::cout << "Version     : " << PIPY_VERSION << std::endl;
  std::cout << "Commit      : " << PIPY_COMMIT << std::endl;
  std::cout << "Commit Date : " << PIPY_COMMIT_DATE << std::endl;
  std::cout << "Host        : " << PIPY_HOST << std::endl;
  std::cout << "OpenSSL     : " << OPENSSL_VERSION_TEXT << std::endl;

#ifdef PIPY_USE_GUI
  std::cout << "Builtin GUI : " << "Yes" << std::endl;
#else
  std::cout << "Builtin GUI : " << "No" << std::endl;
#endif

#ifdef PIPY_USE_TUTORIAL
  std::cout << "Tutorial    : " << "Yes" << std::endl;
#else
  std::cout << "Tutorial    : " << "No" << std::endl;
#endif
}

//
// Show help about filters usage
//

static void get_filters_help(
  std::map<std::string, std::list<std::string>> &filters
) {
  auto lines = utils::split(FILTERS_HELP, '\n');
  lines.push_back("");

  std::list<std::string> paragraph;

  for (const auto &line : lines) {
    if (utils::trim(line).empty()) {
      if (!paragraph.empty()) {
        auto name = paragraph.front();
        auto i = name.find('(');
        if (i != std::string::npos) name = name.substr(0, i);
        filters[name] = std::move(paragraph);
      }
    } else {
      paragraph.push_back(line);
    }
  }
}

static void list_filters() {
  std::map<std::string, std::list<std::string>> filters;
  get_filters_help(filters);
  size_t name_width = 0;
  size_t args_width = 0;
  std::list<std::tuple<std::string, std::string, std::string>> list;
  for (auto &f : filters) {
    std::string name, args, desc;
    auto help = f.second;
    auto i = help.begin();
    if (i != help.end()) name = *i++;
    if (i != help.end()) desc = *i++;
    auto p = name.find('(');
    if (p != std::string::npos) {
      args = name.substr(p);
      name = name.substr(0, p);
    }
    name_width = std::max(name_width, name.length());
    args_width = std::max(args_width, args.length());
    list.push_back({ name, args, desc });
  }
  for (const auto &t : list) {
    const auto &name = std::get<0>(t);
    const auto &args = std::get<1>(t);
    const auto &desc = std::get<2>(t);
    std::cout << name;
    std::cout << std::string(name_width - name.length() + 1, ' ');
    std::cout << args;
    if (desc.length() > 0) {
      std::cout << std::string(args_width - args.length() + 3, ' ');
      std::cout << desc;
    }
    std::cout << std::endl;
  }
}

static void help_filters() {
  std::map<std::string, std::list<std::string>> filters;
  get_filters_help(filters);
  for (const auto &p : filters) {
    std::string name, desc;
    auto help = p.second;
    auto i = help.begin();
    if (i != help.end()) name = *i++;
    if (i != help.end()) desc = *i++;
    if (!name.empty()) {
      std::cout << name << std::endl;
      std::cout << std::string(name.length(), '=') << std::endl;
      std::cout << std::endl;
      std::cout << "  " << desc << std::endl;
      std::cout << std::endl;
      if (i != help.end()) {
        std::list<std::pair<std::string, std::string>> lines;
        for (; i != help.end(); ++i) {
          auto n = i->find('=');
          if (n == std::string::npos) {
            lines.push_back({ utils::trim(*i), "" });
          } else {
            lines.push_back({
              utils::trim(i->substr(0,n)),
              utils::trim(i->substr(n+1)),
            });
          }
        }
        size_t max_width = 0;
        for (const auto &p : lines) max_width = std::max(max_width, p.first.length());
        for (const auto &p : lines) {
          std::cout << "  " << p.first;
          std::cout << std::string(max_width - p.first.length(), ' ');
          std::cout << " - " << p.second << std::endl;
        }
        std::cout << std::endl << std::endl;
      }
    }
  }
}

//
// Periodically check codebase updates
//

static void start_checking_updates() {
  static Timer timer;
  static std::function<void()> poll;
  poll = []() {
    if (!Worker::exited()) {
      Status::local.timestamp = utils::now();
      Codebase::current()->sync(
        Status::local,
        [&](bool ok) {
          if (ok) {
            Worker::restart();
          }
        }
      );
    }
    timer.schedule(5, poll);
  };
  poll();
}

//
// Handle signals
//

static void handle_signal(int sig) {
  if (auto worker = Worker::current()) {
    if (worker->handling_signal(sig)) {
      return;
    }
  }

  switch (sig) {
    case SIGINT:
      if (s_admin) s_admin->close();
      Worker::exit(-1);
      break;
    case SIGHUP:
      Worker::restart();
      break;
    case SIGTSTP:
      Status::dump_memory();
      break;
  }
}

//
// Wait for signals
//

static void wait_for_signals(asio::signal_set &signals) {
  signals.async_wait(
    [&](const std::error_code &ec, int sig) {
      if (!ec) handle_signal(sig);
      wait_for_signals(signals);
    }
  );
}

//
// Program entrance
//

int main(int argc, char *argv[]) {
  utils::gen_uuid_v4(Status::local.uuid);
  Status::local.timestamp = utils::now();

  SSL_load_error_strings();
  SSL_library_init();
  OpenSSL_add_all_algorithms();

  tls::TLSSession::init();

  try {
    Options opts(argc, argv);

    if (opts.version) {
      show_version();
      return 0;
    }

    if (opts.help) {
      Options::show_help();
      return 0;
    }

    if (opts.help_filters) {
      std::cout << std::endl;
      help_filters();
      std::cout << std::endl;
      return 0;
    }

    if (opts.list_filters) {
      std::cout << std::endl;
      list_filters();
      std::cout << std::endl;
      return 0;
    }

    Log::set_level(opts.log_level);
    Listener::set_reuse_port(opts.reuse_port);

    AdminService::Options admin_options;
    admin_options.cert = opts.admin_tls_cert;
    admin_options.key = opts.admin_tls_key;
    admin_options.trusted = opts.admin_tls_trusted;

    auto admin_port = opts.admin_port;
    if (!admin_port) admin_port = 6060; // default repo port

    bool is_repo = false;
    bool is_repo_proxy = false;
    bool is_remote = false;
    bool is_tls = false;

    if (opts.filename.empty()) {
      is_repo = true;

    } else if (utils::starts_with(opts.filename, "http://")) {
      is_remote = true;

    } else if (utils::starts_with(opts.filename, "https://")) {
      is_remote = true;
      is_tls = true;

    } else if (utils::is_host_port(opts.filename)) {
      is_repo_proxy = true;

    } else {
      auto full_path = fs::abs_path(opts.filename);
      opts.filename = full_path;
      if (!fs::exists(full_path)) {
        std::string msg("file or directory does not exist: ");
        throw std::runtime_error(msg + full_path);
      }
      is_repo = fs::is_dir(full_path);
    }

    if (is_remote) {
      auto i = opts.filename.find('/');
      auto target = opts.filename.substr(i+2);
      if (!target.empty() && target.back() == '/') {
        target.resize(target.size() - 1);
      }
      if (utils::is_host_port(target)) {
        opts.filename = target;
        is_remote = false;
        is_repo_proxy = true;
      }
    }

    Store *store = nullptr;
    CodebaseStore *repo = nullptr;
    Codebase *codebase = nullptr;

    std::function<void()> load, fail;
    Timer retry_timer;

    // Start as codebase repo service
    if (is_repo) {
      store = opts.filename.empty()
        ? Store::open_memory()
        : Store::open_level_db(opts.filename);
      repo = new CodebaseStore(store);
      s_admin = new AdminService(repo);
      s_admin->open(admin_port, admin_options);

    // Start as codebase repo proxy
    } else if (is_repo_proxy) {
      AdminProxy::Options options;
      options.cert = opts.admin_tls_cert;
      options.key = opts.admin_tls_key;
      options.trusted = opts.admin_tls_trusted;
      options.fetch_options.tls = is_tls;
      options.fetch_options.cert = opts.tls_cert;
      options.fetch_options.key = opts.tls_key;
      options.fetch_options.trusted = opts.tls_trusted;
      s_admin_proxy = new AdminProxy(opts.filename);
      s_admin_proxy->open(admin_port, options);

    // Start as a fixed codebase
    } else {
      if (is_remote) {
        Fetch::Options options;
        options.tls = is_tls;
        options.cert = opts.tls_cert;
        options.key = opts.tls_key;
        options.trusted = opts.tls_trusted;
        codebase = Codebase::from_http(opts.filename, options);
      } else {
        codebase = Codebase::from_fs(opts.filename);
      }

      codebase->set_current();

      load = [&]() {
        codebase->sync(
          Status::local,
          [&](bool ok) {
            if (!ok) {
              fail();
              return;
            }

            auto &entry = Codebase::current()->entry();
            auto worker = Worker::make();
            auto mod = worker->load_module(entry);

            if (!mod) {
              fail();
              return;
            }

            if (opts.verify) {
              Worker::exit(0);
              return;
            }

            if (!worker->start()) {
              fail();
              return;
            }

            Status::local.version = Codebase::current()->version();
            Status::local.update_modules();

            if (opts.admin_port) {
              s_admin = new AdminService(nullptr);
              s_admin->open(opts.admin_port, admin_options);
            }

            start_checking_updates();
          }
        );
      };

      fail = [&]() {
        if (is_remote) {
          retry_timer.schedule(5, load);
        } else {
          Worker::exit(-1);
        }
      };

      load();
    }

    asio::signal_set signals(Net::service());
    signals.add(SIGINT);
    signals.add(SIGHUP);
    signals.add(SIGTSTP);
    wait_for_signals(signals);

    Net::run();

    delete s_admin;
    delete s_admin_proxy;
    delete repo;

    if (store) store->close();

    std::cout << "Done." << std::endl;

  } catch (std::runtime_error &e) {
    std::cerr << e.what() << std::endl;
    return -1;
  }

  return Worker::exit_code();
}
