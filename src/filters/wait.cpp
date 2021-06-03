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

#include "wait.hpp"
#include "session.hpp"
#include "logging.hpp"

namespace pipy {

//
// Wait
//

Wait::Wait()
{
}

Wait::Wait(pjs::Function *condition)
  : m_condition(condition)
{
}

Wait::Wait(const Wait &r)
  : Wait(r.m_condition)
{
}

Wait::~Wait()
{
}

auto Wait::help() -> std::list<std::string> {
  return {
    "wait(condition)",
    "Buffers up events until a condition is fulfilled",
    "condition = <function> Callback function that returns whether the condition is fulfilled",
  };
}

void Wait::dump(std::ostream &out) {
  out << "wait";
}

auto Wait::clone() -> Filter* {
  return new Wait(*this);
}

void Wait::reset() {
  Waiter::cancel();
  m_buffer.clear();
  m_fulfilled = false;
  m_session_end = false;
}

void Wait::process(Context *ctx, Event *inp) {
  if (m_session_end) return;

  if (m_fulfilled) {
    output(inp);
  } else {
    pjs::Value ret;
    if (!callback(*ctx, m_condition, 0, nullptr, ret)) return;
    if (ret.to_boolean()) {
      Waiter::cancel();
      m_fulfilled = true;
      m_buffer.flush(out());
      output(inp);
    } else {
      Waiter::wait(ctx->group());
      m_buffer.push(inp);
    }
  }

  if (inp->is<SessionEnd>()) {
    m_session_end = true;
  }
}

void Wait::on_notify(Context *ctx) {
  pjs::Value ret;
  if (!callback(*ctx, m_condition, 0, nullptr, ret)) return;
  if (ret.to_boolean()) {
    Waiter::cancel();
    m_fulfilled = true;
    m_buffer.flush(out());
  }
}

} // namespace pipy