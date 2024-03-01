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

#include "pipy.hpp"
#include "codebase.hpp"
#include "configuration.hpp"
#include "context.hpp"
#include "worker.hpp"
#include "worker-thread.hpp"
#include "thread.hpp"
#include "status.hpp"
#include "net.hpp"
#include "listener.hpp"
#include "outbound.hpp"
#include "file.hpp"
#include "fstream.hpp"
#include "api/pipeline-api.hpp"
#include "os-platform.hpp"
#include "utils.hpp"
#include "log.hpp"

#include <exception>

#ifndef _WIN32
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#endif

namespace pipy {

thread_local static pjs::Ref<pjs::Array> s_argv;
thread_local static std::list<pjs::Ref<pjs::Function>> s_exit_callbacks;

auto Pipy::argv() -> pjs::Array* {
  return s_argv;
}

void Pipy::argv(const std::vector<std::string> &argv) {
  s_argv = pjs::Array::make(argv.size());
  for (int i = 0; i < argv.size(); i++) {
    s_argv->set(i, pjs::Str::make(argv[i]));
  }
}

static std::function<void(int)> s_on_exit;

void Pipy::on_exit(const std::function<void(int)> &on_exit) {
  s_on_exit = on_exit;
}

thread_local static Data::Producer s_dp("pipy.exec()");

Pipy::ExecOptions::ExecOptions(pjs::Object *options) {
  Value(options, "stdin")
    .get(std_in)
    .check_nullable();
  Value(options, "stderr")
    .get(std_err)
    .check_nullable();
  Value(options, "onExit")
    .get(on_exit_f)
    .check_nullable();
}

inline static void no_arguments() {
  throw std::runtime_error("exec() with no arguments");
}

#ifndef _WIN32

static auto exec_argv(const std::list<std::string> &args, const Pipy::ExecOptions &options) -> Pipy::ExecResult {
  auto argc = args.size();
  if (!argc) {
    no_arguments();
  }

  int n = 0;
  char *argv[argc + 1];
  for (const auto &arg : args) argv[n++] = strdup(arg.c_str());
  argv[n] = nullptr;

  Pipy::ExecResult result;
  std::thread t_stdout, t_stderr;
  std::vector<uint8_t> buf_stdout;
  std::vector<uint8_t> buf_stderr;

  int pipes[3][2];
  std::memset(pipes, 0, sizeof(pipes));

  try {
    if (pipe(pipes[1]) ||
      (options.std_in && pipe(pipes[0])) ||
      (!options.std_err && pipe(pipes[2]))
    ) {
      throw std::runtime_error("unable to create pipes");
    }

    auto pid = fork();
    if (!pid) {
      if (pipes[0][0]) dup2(pipes[0][0], 0);
      dup2(pipes[1][1], 1);
      dup2(pipes[2][1] ? pipes[2][1] : pipes[1][1], 2);
      execvp(argv[0], argv);
      std::terminate();
    } else if (pid < 0) {
      throw std::runtime_error("unable to fork");
    }

    auto read_pipe = [&](int pipe) -> std::vector<uint8_t> {
      Data data;
      Data::Builder db(data, &s_dp);
      char buf[DATA_CHUNK_SIZE];

      while (auto len = read(pipe, buf, sizeof(buf))) {
        if (len < 0) break;
        db.push(buf, len);
      }
      db.flush();

      // Unfortunately we were not able to return this in SharedData
      // because SharedData relies on thread-local pjs::Pool,
      // which is dead right after the thread ends.
      return data.to_bytes();
    };

    if (pipes[1][0]) t_stdout = std::thread([&]() { buf_stdout = read_pipe(pipes[1][0]); });
    if (pipes[2][0]) t_stderr = std::thread([&]() { buf_stderr = read_pipe(pipes[2][0]); });

    if (auto *data = options.std_in.get()) {
      for (auto c : data->chunks()) {
        auto ptr = std::get<0>(c);
        auto len = std::get<1>(c);
        bool err = false;
        while (len > 0) {
          auto n = write(pipes[0][1], ptr, len);
          if (n < 0) { err = true; break; }
          len -= n;
          ptr += n;
        }
        if (err) break;
      }
    }

    for (int i = 0; i < 3; i++) {
      if (pipes[i][1]) {
        close(pipes[i][1]);
        pipes[i][1] = 0;
      }
    }

    int status;
    waitpid(pid, &status, 0);
    result.exit_code = WEXITSTATUS(status);

  } catch (std::runtime_error &) {
    for (int i = 0; i < 3; i++) {
      if (pipes[i][0]) close(pipes[i][0]);
      if (pipes[i][1]) close(pipes[i][1]);
    }
    throw;
  }

  if (t_stdout.joinable()) t_stdout.join();
  if (t_stderr.joinable()) t_stderr.join();

  for (int i = 0; i < 3; i++) {
    if (pipes[i][0]) close(pipes[i][0]);
    if (pipes[i][1]) close(pipes[i][1]);
  }

  result.out = s_dp.make(buf_stdout);

  if (!options.std_err) {
    result.err = s_dp.make(buf_stderr);
  }

  return result;
}

auto Pipy::exec(const std::string &cmd, const ExecOptions &options) -> ExecResult {
  return exec_argv(utils::split_argv(cmd), options);
}

auto Pipy::exec(pjs::Array *argv, const ExecOptions &options) -> ExecResult {
  if (!argv || argv->length() == 0) no_arguments();

  std::list<std::string> args;
  argv->iterate_all(
    [&](pjs::Value &v, int) {
      auto *s = v.to_string();
      args.push_back(s->str());
      s->release();
    }
  );

  return exec_argv(args, options);
}

#else // _WIN32

static auto exec_line(const std::string &line, const Pipy::ExecOptions &options) -> Pipy::ExecResult {
  Pipy::ExecResult result;
  std::thread t_stdout, t_stderr;
  std::vector<uint8_t> buf_stdout;
  std::vector<uint8_t> buf_stderr;

  HANDLE pipes[3][2];
  ZeroMemory(pipes, sizeof(pipes));

  try {
    SECURITY_ATTRIBUTES sa;
    ZeroMemory(&sa, sizeof(sa));
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;

    if (!CreatePipe(&pipes[1][0], &pipes[1][1], &sa, 0) ||
      (options.std_in && !CreatePipe(&pipes[0][0], &pipes[0][1], &sa, 0)) ||
      (!options.std_err && !CreatePipe(&pipes[2][0], &pipes[2][1], &sa, 0))
    ) {
      throw std::runtime_error(
        "unable to create pipe: " + os::windows::get_last_error()
      );
    }

    PROCESS_INFORMATION pif = {};
    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(STARTUPINFO);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = pipes[0][0] ? pipes[0][0] : INVALID_HANDLE_VALUE;
    si.hStdOutput = pipes[1][1];
    si.hStdError = pipes[2][1] ? pipes[2][1] : pipes[1][1];

    auto line_w = os::windows::a2w(line);
    pjs::vl_array<wchar_t, 1000> buf_line(line_w.length() + 1);
    std::memcpy(buf_line.data(), line_w.c_str(), buf_line.size() * sizeof(wchar_t));

    if (!CreateProcessW(
      NULL,
      buf_line.data(),
      NULL, NULL, TRUE, 0, NULL, NULL,
      &si, &pif
    )) {
      throw std::runtime_error(
        "unable to create process '" + line + "': " + os::windows::get_last_error()
      );
    }

    auto read_pipe = [&](HANDLE pipe) -> std::vector<uint8_t> {
      Data data;
      Data::Builder db(data, &s_dp);
      char buf[DATA_CHUNK_SIZE];
      DWORD len;

      while (ReadFile(pipe, buf, sizeof(buf), &len, NULL)) {
        db.push(buf, len);
      }
      db.flush();

      // Unfortunately we were not able to return this in SharedData
      // because SharedData relies on thread-local pjs::Pool,
      // which is dead right after the thread ends.
      return data.to_bytes();
    };

    if (pipes[1][0]) t_stdout = std::thread([&]() { buf_stdout = read_pipe(pipes[1][0]); });
    if (pipes[2][0]) t_stderr = std::thread([&]() { buf_stderr = read_pipe(pipes[2][0]); });

    if (auto *data = options.std_in.get()) {
      for (auto c : data->chunks()) {
        auto ptr = std::get<0>(c);
        auto len = std::get<1>(c);
        DWORD written;
        if (!WriteFile(pipes[0][1], ptr, len, &written, NULL)) break;
      }
    }

    for (int i = 0; i < 3; i++) {
      if (pipes[i][1]) {
        CloseHandle(pipes[i][1]);
        pipes[i][1] = 0;
      }
    }

    WaitForSingleObject(pif.hProcess, INFINITE);

    DWORD code;
    GetExitCodeProcess(pif.hProcess, &code);
    result.exit_code = code;

    CloseHandle(pif.hThread);
    CloseHandle(pif.hProcess);

  } catch (std::runtime_error &) {
    for (int i = 0; i < 3; i++) {
      if (pipes[i][0]) CloseHandle(pipes[i][0]);
      if (pipes[i][1]) CloseHandle(pipes[i][1]);
    }
    throw;
  }

  if (t_stdout.joinable()) t_stdout.join();
  if (t_stderr.joinable()) t_stderr.join();

  for (int i = 0; i < 3; i++) {
    if (pipes[i][0]) CloseHandle(pipes[i][0]);
    if (pipes[i][1]) CloseHandle(pipes[i][1]);
  }

  result.out = s_dp.make(buf_stdout);

  if (!options.std_err) {
    result.err = s_dp.make(buf_stderr);
  }

  return result;
}

auto Pipy::exec(const std::string &cmd, const ExecOptions &options) -> ExecResult {
  return exec_line(cmd, options);
}

auto Pipy::exec(pjs::Array *argv, const ExecOptions &options) -> ExecResult {
  if (!argv || argv->length() == 0) no_arguments();

  std::list<std::string> args;
  argv->iterate_all(
    [&](pjs::Value &v, int) {
      auto *s = v.to_string();
      args.push_back(s->str());
      s->release();
    }
  );

  return exec_line(os::windows::encode_argv(args), options);
}

#endif // _WIN32

void Pipy::listen(pjs::Context &ctx) {
  thread_local static pjs::ConstStr s_tcp("tcp");
  thread_local static pjs::ConstStr s_udp("udp");

  auto instance = ctx.root()->instance();
  auto worker = instance ? static_cast<Worker*>(instance) : nullptr;

  int i = 0;
  int port = 0;
  pjs::Str *address = nullptr;
  pjs::Str *protocol = nullptr;
  pjs::Object *options = nullptr;
  pjs::Function *builder = nullptr;
  PipelineLayoutWrapper *plw = nullptr;

  if (ctx.get(i, address) || ctx.get(i, port)) i++;
  else {
    ctx.error_argument_type(i, "a number or a string");
    return;
  }

  if (ctx.get(i, protocol)) i++;

  if (!ctx.get(i, builder) && !ctx.get(i, plw)) {
    if (!ctx.check(i++, options)) return;
    if (!ctx.check(i, builder) && !ctx.check(i, plw)) return;
  }

  Listener::Protocol proto = Listener::Protocol::TCP;
  if (protocol) {
    if (protocol == s_tcp) proto = Listener::Protocol::TCP; else
    if (protocol == s_udp) proto = Listener::Protocol::UDP; else {
      ctx.error("unknown protocol");
      return;
    }
  }

  std::string ip;

  if (address) {
    auto n = address->parse_float();
    if (std::isnan(n)) {
      if (!utils::get_host_port(address->str(), ip, port)) {
        ctx.error("invalid 'address:port' form");
        return;
      }
      uint8_t buf[16];
      if (!utils::get_ip_v4(ip, buf) && !utils::get_ip_v6(ip, buf)) {
        ctx.error("invalid IP address");
        return;
      }
    } else {
      auto i = (int)n;
      if (i != n || i < 1 || i > 65535) {
        ctx.error("invalid port number");
        return;
      }
      ip = "0.0.0.0";
      port = i;
    }
  } else {
    ip = "0.0.0.0";
  }

  if (port < 1 || 65535 < port) {
    ctx.error("port out of range");
    return;
  }

  PipelineLayout *pl = nullptr;
  if (plw) {
    pl = plw->get();
  } else if (builder) {
    pl = PipelineDesigner::make_pipeline_layout(ctx, builder);
    if (!pl) return;
  }

  auto l = Listener::get(proto, ip, port);
  if (!l->set_next_state(pl, options) && worker && !worker->started() && !worker->forced()) {
    l->rollback();
    ctx.error("unable to listen on [" + ip + "]:" + std::to_string(port));
    return;
  }

  if (!worker || worker->started()) {
    l->commit();
  }
}

auto Pipy::watch(pjs::Str *pathname) -> pjs::Promise* {
  auto w = new FileWatcher(pathname);
  return w->start();
}

void Pipy::exit(int code) {
  Net::main().post(
    [=]() {
      WorkerManager::get().stop(true);
      if (s_on_exit) s_on_exit(code);
    }
  );
}

void Pipy::exit(pjs::Function *cb) {
  s_exit_callbacks.push_back(cb);
}

bool Pipy::has_exit_callbacks() {
  return s_exit_callbacks.size() > 0;
}

bool Pipy::start_exiting(pjs::Context &ctx, const std::function<void()> &on_done) {
  thread_local static size_t s_exit_callbacks_counter = 0;
  std::list<pjs::Ref<pjs::Function>> callbacks(std::move(s_exit_callbacks));
  for (auto &cb : callbacks) {
    pjs::Value ret;
    (*cb)(ctx, 0, nullptr, ret);
    if (!ctx.ok()) return false;
    if (ret.is_promise()) {
      auto pcb = pjs::Promise::Callback::make(
        [=](pjs::Promise::State state, const pjs::Value &value) {
          printf("promise callback\n");
          if (!--s_exit_callbacks_counter) {
            on_done();
          }
        }
      );
      ret.as<pjs::Promise>()->then(&ctx, pcb->resolved(), pcb->rejected());
      s_exit_callbacks_counter++;
    }
  }
  return s_exit_callbacks_counter > 0;
}

void Pipy::operator()(pjs::Context &ctx, pjs::Object *obj, pjs::Value &ret) {
  pjs::Value ret_obj;
  pjs::Object *context_prototype = nullptr;
  if (!ctx.arguments(0, &context_prototype)) return;
  if (context_prototype && context_prototype->is_function()) {
    auto *f = context_prototype->as<pjs::Function>();
    (*f)(ctx, 0, nullptr, ret_obj);
    if (!ctx.ok()) return;
    if (!ret_obj.is_object()) {
      ctx.error("function did not return an object");
      return;
    }
    context_prototype = ret_obj.o();
  }
  try {
    auto config = Configuration::make(context_prototype);
    ret.set(config);
  } catch (std::runtime_error &err) {
    ctx.error(err);
  }
}

//
// Pipy::FileReader
//

Pipy::FileReader::FileReader(Worker *worker, pjs::Str *pathname, PipelineLayout *pt)
  : m_worker(worker)
  , m_pathname(pathname)
  , m_pt(pt)
  , m_file(File::make(pathname->str()))
{
}

auto Pipy::FileReader::start() -> pjs::Promise* {
  auto promise = pjs::Promise::make();
  m_settler = pjs::Promise::Settler::make(promise);
  m_file->open_read([this](FileStream *fs) { on_open(fs); });
  retain();
  return promise;
}

void Pipy::FileReader::on_open(FileStream *fs) {
  if (fs) {
    m_pipeline = Pipeline::make(m_pt, m_worker->new_context());
    m_pipeline->on_end(this);
    m_pipeline->chain(EventTarget::input());
    m_pipeline->start();
    fs->chain(m_pipeline->input());
  } else {
    std::string msg = "cannot open file: " + m_pathname->str();
    m_settler->reject(pjs::Error::make(pjs::Str::make(msg)));
    release();
  }
}

void Pipy::FileReader::on_event(Event *evt) {
  if (evt->is<StreamEnd>()) {
    m_pipeline = nullptr;
  }
}

void Pipy::FileReader::on_pipeline_result(Pipeline *p, pjs::Value &result) {
  m_settler->resolve(result);
  release();
}

//
// Pipy::FileWatcher
//

Pipy::FileWatcher::FileWatcher(pjs::Str *pathname)
  : m_net(Net::current())
  , m_pathname(pathname)
{
}

auto Pipy::FileWatcher::start() -> pjs::Promise* {
  auto promise = pjs::Promise::make();
  m_settler = pjs::Promise::Settler::make(promise);
  m_codebase_watch = Codebase::current()->watch(
    m_pathname->str(),
    [this](bool changed) { on_file_changed(changed); }
  );
  retain();
  return promise;
}

void Pipy::FileWatcher::on_file_changed(bool changed) {
  if (changed) {
    m_net.post([this]() {
      InputContext ic;
      m_settler->resolve(pjs::Value::undefined);
      m_codebase_watch->close();
      release();
    });
  }
}

} // namespace pipy

namespace pjs {

using namespace pipy;

template<> void ClassDef<Pipy>::init() {
  super<Function>();
  ctor();

  variable("inbound", class_of<Pipy::Inbound>());
  variable("outbound", class_of<Pipy::Outbound>());

  accessor("pid", [](Object *, Value &ret) {
    ret.set(os::process_id());
  });

  accessor("since", [](Object *, Value &ret) {
    ret.set(Status::LocalInstance::since);
  });

  accessor("source", [](Object *, Value &ret) {
    thread_local static pjs::Ref<pjs::Str> str;
    if (!str) str = pjs::Str::make(Status::LocalInstance::source);
    ret.set(str);
  });

  accessor("name", [](Object *, Value &ret) {
    thread_local static pjs::Ref<pjs::Str> str;
    if (!str) str = pjs::Str::make(Status::LocalInstance::name);
    ret.set(str);
  });

  accessor("uuid", [](Object *, Value &ret) {
    thread_local static pjs::Ref<pjs::Str> str;
    if (!str) str = pjs::Str::make(Status::LocalInstance::uuid);
    ret.set(str);
  });

  accessor("argv", [](Object *, Value &ret) {
    ret.set(s_argv.get());
  });

  accessor("thread", [](Object *, Value &ret) {
    ret.set(Thread::current());
  });

  method("now", [](Context &ctx, Object *obj, Value &ret) {
    ret.set(utils::now_since(Status::LocalInstance::since));
  });

  method("fork", [](Context &ctx, Object *obj, Value &ret) {
    Function *func;
    if (!ctx.arguments(1, &func)) return;
    auto root = static_cast<pipy::Context*>(ctx.root());
    auto worker = static_cast<Worker*>(ctx.instance());
    pjs::Ref<pipy::Context> context = worker->new_context(root);
    (*func)(*context, 0, nullptr, ret);
    if (!context->ok()) ctx.error(*context);
  });

  method("load", [](Context &ctx, Object*, Value &ret) {
    std::string filename;
    if (!ctx.arguments(1, &filename)) return;
    auto path = utils::path_normalize(filename);
    auto data = Codebase::current()->get(path);
    ret.set(data ? pipy::Data::make(*data) : nullptr);
    if (data) data->release();
  });

  method("list", [](Context &ctx, Object*, Value &ret) {
    std::string pathname;
    if (!ctx.arguments(1, &pathname)) return;
    auto codebase = Codebase::current();
    auto a = Array::make();
    std::function<void(const std::string&, const std::string&)> list_dir;
    list_dir = [&](const std::string &path, const std::string &base) {
      for (const auto &name : codebase->list(path)) {
        if (name.back() == '/') {
          auto sub = name.substr(0, name.length() - 1);
          auto str = path + '/' + sub;
          list_dir(str, base + sub + '/');
        } else {
          a->push(Str::make(base + name));
        }
      }
    };
    list_dir(utils::path_normalize(pathname), "");
    ret.set(a);
  });

  method("watch", [](Context &ctx, Object*, Value &ret) {
    Str *pathname;
    if (!ctx.arguments(1, &pathname)) return;
    ret.set(Pipy::watch(pathname));
  });

  method("import", [](Context &ctx, Object*, Value &ret) {
    Str *path;
    if (!ctx.arguments(1, &path)) return;
    auto worker = static_cast<pipy::Context*>(ctx.root())->worker();
    auto referer = ctx.caller()->call_site().module;
    if (auto mod = worker->load_module(referer, path->str())) {
      ret.set(mod->exports_object());
    } else {
      ctx.error("cannot import module: " + path->str());
    }
  });

  method("solve", [](Context &ctx, Object*, Value &ret) {
    Str *filename;
    if (!ctx.arguments(1, &filename)) return;
    auto worker = static_cast<pipy::Context*>(ctx.root())->worker();
    worker->solve(ctx, filename, ret);
  });

  method("restart", [](Context&, Object*, Value&) {
    Net::main().post(
      []() {
        InputContext ic;
        Codebase::current()->sync(
          true, [](bool ok) {
            if (ok) {
              WorkerManager::get().reload();
            }
          }
        );
      }
    );
  });

  method("exit", [](Context &ctx, Object*, Value&) {
    Function *callback = nullptr;
    int exit_code = 0;
    if (ctx.try_arguments(1, &callback)) {
      Pipy::exit(callback);
    } else if (ctx.try_arguments(0, &exit_code)) {
      Pipy::exit(exit_code);
    } else {
      ctx.error_argument_type(0, "a number or a function");
    }
  });

  method("exec", [](Context &ctx, Object*, Value &ret) {
    Str *cmd = nullptr;
    Array *argv = nullptr;
    try {
      if (ctx.get(0, cmd) || ctx.get(0, argv)) {
        Object *options;
        if (!ctx.check<Object>(1, options, nullptr)) return;
        Pipy::ExecOptions opts(options);
        Pipy::ExecResult result;
        if (cmd) {
          result = Pipy::exec(cmd->str(), opts);
        } else {
          result = Pipy::exec(argv, opts);
        }
        if (auto f = opts.on_exit_f.get()) {
          int argc = 1;
          Value args[2], ret;
          args[0].set(result.exit_code);
          if (!opts.std_err) {
            args[1].set(result.err);
            argc = 2;
          }
          (*f)(ctx, argc, args, ret);
        }
        ret.set(result.out);
      } else {
        ctx.error_argument_type(0, "a string or an array");
      }
    } catch (const std::runtime_error &err) {
      ctx.error(err);
    }
  });

  method("read", [](Context &ctx, Object*, Value &ret) {
    auto instance = ctx.root()->instance();
    auto worker = instance ? static_cast<Worker*>(instance) : nullptr;
    Str *pathname;
    Function *builder = nullptr;
    if (!ctx.arguments(2, &pathname, &builder)) return;
    auto pt = PipelineDesigner::make_pipeline_layout(ctx, builder);
    if (!pt) return;
    auto fr = new Pipy::FileReader(worker, pathname, pt);
    ret.set(fr->start());
  });

  method("listen", [](Context &ctx, Object*, Value &ret) {
    Pipy::listen(ctx);
  });
}

template<> void ClassDef<Pipy::Inbound>::init() {
  ctor();

  accessor("count", [](Object*, Value &ret) { ret.set(pipy::Inbound::count()); });

  method("forEach", [](Context &ctx, Object*, Value&) {
    Function *cb;
    if (!ctx.arguments(1, &cb)) return;
    pipy::Inbound::for_each(
      [&](pipy::Inbound *ib) {
        Value arg(ib), ret;
        (*cb)(ctx, 1, &arg, ret);
        return ctx.ok();
      }
    );
  });
}

template<> void ClassDef<Pipy::Outbound>::init() {
  ctor();

  accessor("count", [](Object*, Value &ret) { ret.set(pipy::Outbound::count()); });

  method("forEach", [](Context &ctx, Object*, Value&) {
    Function *cb;
    if (!ctx.arguments(1, &cb)) return;
    pipy::Outbound::for_each(
      [&](pipy::Outbound *ob) {
        Value arg(ob), ret;
        (*cb)(ctx, 1, &arg, ret);
        return ctx.ok();
      }
    );
  });
}

} // namespace pjs
