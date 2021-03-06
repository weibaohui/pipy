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

#ifndef DATA_HPP
#define DATA_HPP

#include "object.hpp"
#include "constants.hpp"

#include <cstring>
#include <functional>

NS_BEGIN

//
// Data
//

class Data : public Object, public Pooled<Data> {
  virtual auto type() const -> Type override {
    return Object::Data;
  }

  virtual auto name() const -> const char* override {
    return "Data";
  }

  virtual auto clone() const -> Object* override {
    return new Data(*this);
  }

  struct Chunk : public Pooled<Chunk> {
    char   data[DATA_CHUNK_SIZE];
    int    retain_count = 0;
    int    size() const { return sizeof(data); }
    void   retain() { ++retain_count; }
    void   release() { if (!--retain_count) delete this; }
  };

  struct View : public Pooled<View> {
    View*  prev = nullptr;
    View*  next = nullptr;
    Chunk* chunk;
    int    offset;
    int    length;

    View(Chunk *c, int o, int l)
      : chunk(c)
      , offset(o)
      , length(l)
    {
      c->retain();
    }

    View(View *view)
      : chunk(view->chunk)
      , offset(view->offset)
      , length(view->length)
    {
      chunk->retain();
    }

    ~View() {
      chunk->release();
    }

    int push(const void *p, int n) {
      int tail = offset + length;
      int room = std::min(chunk->size() - tail, n);
      if (room > 0) {
        std::memcpy(chunk->data + tail, p, room);
        length += room;
        return room;
      } else {
        return 0;
      }
    }

    View* pop(int n) {
      length -= n;
      return new View(chunk, offset + length, n);
    }

    View* shift(int n) {
      auto view = new View(chunk, offset, n);
      offset += n;
      length -= n;
      return view;
    }
  };

public:
  class Chunks {
    friend class Data;
    View* m_head;
    Chunks(View *head) : m_head(head) {}

  public:
    class Iterator {
      friend class Chunks;
      View* m_p;
      Iterator(View *p) : m_p(p) {}

    public:
      auto operator++() -> Iterator& {
        m_p = m_p->next;
        return *this;
      }

      bool operator==(Iterator other) const {
        return m_p == other.m_p;
      }

      bool operator!=(Iterator other) const {
        return m_p != other.m_p;
      }

      auto operator*() const -> std::tuple<char*, int> {
        return std::make_tuple(m_p->chunk->data + m_p->offset, m_p->length);
      }
    };

    auto begin() const -> Iterator {
      return Iterator(m_head);
    }

    auto end() const -> Iterator {
      return Iterator(nullptr);
    }
  };

  Data()
    : m_head(nullptr)
    , m_tail(nullptr)
    , m_size(0)
  {
  }

  Data(int size)
    : m_head(nullptr)
    , m_tail(nullptr)
    , m_size(0)
  {
    while (size > 0) {
      auto chunk = new Chunk;
      auto length = std::min(size, chunk->size());
      push_view(new View(chunk, 0, length));
      size -= length;
    }
  }

  Data(int size, int value)
    : m_head(nullptr)
    , m_tail(nullptr)
    , m_size(0)
  {
    while (size > 0) {
      auto chunk = new Chunk;
      auto length = std::min(size, chunk->size());
      std::memset(chunk->data, value, length);
      push_view(new View(chunk, 0, length));
      size -= length;
    }
  }

  Data(const void *data, int size)
    : m_head(nullptr)
    , m_tail(nullptr)
    , m_size(0)
  {
    push(data, size);
  }

  Data(const char *str) : Data(str, std::strlen(str)) {
  }

  Data(const std::string &str) : Data(str.c_str(), str.length()) {
  }

  Data(const Data &other)
    : m_head(nullptr)
    , m_tail(nullptr)
    , m_size(0)
  {
    for (auto view = other.m_head; view; view = view->next) {
      push_view(new View(view));
    }
  }

  Data(Data &&other)
    : m_head(other.m_head)
    , m_tail(other.m_tail)
    , m_size(other.m_size)
  {
    other.m_head = nullptr;
    other.m_tail = nullptr;
    other.m_size = 0;
  }

  ~Data() {
    clear();
  }

  auto operator=(const Data &other) -> Data& {
    if (this != &other) {
      clear();
      for (auto view = other.m_head; view; view = view->next) {
        push_view(new View(view));
      }
    }
    return *this;
  }

  auto operator=(Data &&other) -> Data& {
    if (this != &other) {
      m_head = other.m_head;
      m_tail = other.m_tail;
      m_size = other.m_size;
      other.m_head = nullptr;
      other.m_tail = nullptr;
      other.m_size = 0;
    }
    return *this;
  }

  bool empty() const {
    return !m_size;
  }

  int size() const {
    return m_size;
  }

  Chunks chunks() {
    return Chunks(m_head);
  }

  void clear() {
    for (auto view = m_head; view; ) {
      auto next = view->next;
      delete view;
      view = next;
    }
    m_head = m_tail = nullptr;
    m_size = 0;
  }

  void push(const Data &data) {
    for (auto view = data.m_head; view; view = view->next) {
      push_view(new View(view));
    }
  }

  void push(const std::string &str) {
    push(str.c_str(), str.length());
  }

  void push(const char *str) {
    push(str, std::strlen(str));
  }

  void push(const void *data, int n) {
    const char *p = (const char*)data;
    if (auto view = m_tail) {
      auto chunk = view->chunk;
      if (chunk->retain_count == 1) {
        int added = view->push(p, n);
        m_size += added;
        p += added;
        n -= added;
      }
    }
    while (n > 0) {
      auto view = new View(new Chunk, 0, 0);
      auto added = view->push(p, n);
      p += added;
      n -= added;
      push_view(view);
    }
  }

  void push(char ch) {
    push(&ch, 1);
  }

  auto pop(int n) -> Data {
    Data buf;
    while (auto view = m_tail) {
      if (n <= 0) break;
      if (view->length <= n) {
        n -= view->length;
        buf.unshift_view(pop_view());
      } else {
        buf.unshift_view(view->pop(n));
        m_size -= n;
        break;
      }
    }
    return buf;
  }

  auto pop_until(std::function<bool(int)> f) -> Data {
    Data buf;
    while (auto view = m_tail) {
      auto data = view->chunk->data;
      auto size = view->length;
      auto tail = view->offset + size - 1;
      auto n = 0; while (n < size && !f(data[tail - n])) ++n;
      if (n == size) {
        buf.unshift_view(pop_view());
      } else {
        buf.unshift_view(view->pop(n));
        m_size -= n;
        break;
      }
    }
    return buf;
  }

  auto shift(int n) -> Data {
    Data buf;
    while (auto view = m_head) {
      if (n <= 0) break;
      if (view->length <= n) {
        n -= view->length;
        buf.push_view(shift_view());
      } else {
        buf.push_view(view->shift(n));
        m_size -= n;
        break;
      }
    }
    return buf;
  }

  auto shift(std::function<bool(int)> f) -> Data {
    Data buf;
    while (auto view = m_head) {
      auto data = view->chunk->data;
      auto size = view->length;
      auto head = view->offset;
      auto n = 0;
      while (n < size) if (f(data[head + n++])) break;
      if (n == size) {
        buf.push_view(shift_view());
      } else {
        buf.push_view(view->shift(n));
        m_size -= n;
        break;
      }
    }
    return buf;
  }

  auto shift_until(std::function<bool(int)> f) -> Data {
    Data buf;
    while (auto view = m_head) {
      auto data = view->chunk->data;
      auto size = view->length;
      auto head = view->offset;
      auto n = 0; while (n < size && !f(data[head + n])) ++n;
      if (n == size) {
        buf.push_view(shift_view());
      } else {
        buf.push_view(view->shift(n));
        m_size -= n;
        break;
      }
    }
    return buf;
  }

  void to_chunks(std::function<void(const uint8_t*, int)> cb) {
    for (auto view = m_head; view; view = view->next) {
      cb((uint8_t*)view->chunk->data + view->offset, view->length);
    }
  }

  void to_bytes(uint8_t *buf) {
    auto p = buf;
    for (auto view = m_head; view; view = view->next) {
      auto length = view->length;
      std::memcpy(p, view->chunk->data + view->offset, length);
      p += length;
    }
  }

  auto to_string() -> std::string {
    std::string str(m_size, 0);
    int i = 0;
    for (auto view = m_head; view; view = view->next) {
      auto length = view->length;
      str.replace(i, length, view->chunk->data + view->offset, length);
      i += length;
    }
    return str;
  }

private:
  View*  m_head;
  View*  m_tail;
  int    m_size;

  void push_view(View *view) {
    auto size = view->length;
    if (auto tail = m_tail) {
      if (tail->chunk == view->chunk &&
          tail->offset + tail->length == view->offset)
      {
        delete view;
        tail->length += size;
        m_size += size;
        return;
      }
      tail->next = view;
      view->prev = tail;
    } else {
      m_head = view;
    }
    m_tail = view;
    m_size += size;
  }

  auto pop_view() -> View* {
    auto view = m_tail;
    m_tail = view->prev;
    view->prev = nullptr;
    if (m_tail) {
      m_tail->next = nullptr;
    } else {
      m_head = nullptr;
    }
    m_size -= view->length;
    return view;
  }

  auto shift_view() -> View* {
    auto view = m_head;
    m_head = view->next;
    view->next = nullptr;
    if (m_head) {
      m_head->prev = nullptr;
    } else {
      m_tail = nullptr;
    }
    m_size -= view->length;
    return view;
  }

  void unshift_view(View *view) {
    auto size = view->length;
    if (auto head = m_head) {
      if (head->chunk == view->chunk &&
          head->offset == view->offset + size)
      {
        delete view;
        head->offset -= size;
        head->length += size;
        m_size += size;
        return;
      }
      head->prev = view;
      view->next = head;
    } else {
      m_tail = view;
    }
    m_head = view;
    m_size += size;
  }
};

NS_END

#endif // DATA_HPP
