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

#ifndef TEE_HPP
#define TEE_HPP

#include "filter.hpp"
#include "file.hpp"
#include "data.hpp"
#include "options.hpp"
#include "fstream.hpp"
#include "timer.hpp"

namespace pipy {

//
// Tee
//

class Tee : public Filter {
public:
  struct Options : public pipy::Options {
    bool append = false;
    Options() {}
    Options(pjs::Object *options);
  };

  Tee(const pjs::Value &filename, const Options &options);

private:
  Tee(const Tee &r);
  ~Tee();

  virtual auto clone() -> Filter* override;
  virtual void reset() override;
  virtual void process(Event *evt) override;
  virtual void dump(Dump &d) override;

  Options m_options;
  pjs::Value m_filename;
  pjs::Ref<pjs::Str> m_resolved_filename;
  pjs::Ref<File> m_file;
  Timer m_keep_alive;

  void keep_alive();
};

} // namespace pipy

#endif // TEE_HPP
