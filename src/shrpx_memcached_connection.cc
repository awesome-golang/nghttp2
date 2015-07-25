/*
 * nghttp2 - HTTP/2 C Library
 *
 * Copyright (c) 2015 Tatsuhiro Tsujikawa
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include "shrpx_memcached_connection.h"

#include "shrpx_memcached_request.h"
#include "shrpx_memcached_result.h"
#include "shrpx_config.h"
#include "util.h"

namespace shrpx {

namespace {
void timeoutcb(struct ev_loop *loop, ev_timer *w, int revents) {
  auto conn = static_cast<Connection *>(w->data);
  auto mconn = static_cast<MemcachedConnection *>(conn->data);

  if (LOG_ENABLED(INFO)) {
    MCLOG(INFO, mconn) << "Time out";
  }

  mconn->disconnect();
}
} // namespace

namespace {
void readcb(struct ev_loop *loop, ev_io *w, int revents) {
  auto conn = static_cast<Connection *>(w->data);
  auto mconn = static_cast<MemcachedConnection *>(conn->data);

  if (mconn->on_read() != 0) {
    mconn->disconnect();
    return;
  }
}
} // namespace

namespace {
void writecb(struct ev_loop *loop, ev_io *w, int revents) {
  auto conn = static_cast<Connection *>(w->data);
  auto mconn = static_cast<MemcachedConnection *>(conn->data);

  if (mconn->on_write() != 0) {
    mconn->disconnect();
    return;
  }
}
} // namespace

namespace {
void connectcb(struct ev_loop *loop, ev_io *w, int revents) {
  auto conn = static_cast<Connection *>(w->data);
  auto mconn = static_cast<MemcachedConnection *>(conn->data);

  if (mconn->on_connect() != 0) {
    mconn->disconnect();
    return;
  }

  writecb(loop, w, revents);
}
} // namespace

constexpr ev_tstamp write_timeout = 10.;
constexpr ev_tstamp read_timeout = 10.;

MemcachedConnection::MemcachedConnection(const sockaddr_union *addr,
                                         size_t addrlen, struct ev_loop *loop)
    : conn_(loop, -1, nullptr, write_timeout, read_timeout, 0, 0, 0, 0,
            connectcb, readcb, timeoutcb, this),
      parse_state_{}, addr_(addr), addrlen_(addrlen), sendsum_(0),
      connected_(false) {}

MemcachedConnection::~MemcachedConnection() { disconnect(); }

namespace {
void clear_request(std::deque<std::unique_ptr<MemcachedRequest>> &q) {
  for (auto &req : q) {
    if (req->cb) {
      req->cb(req.get(), MemcachedResult(MEMCACHED_ERR_ERROR));
    }
  }
  q.clear();
}
} // namespace

void MemcachedConnection::disconnect() {
  clear_request(recvq_);
  clear_request(sendq_);

  sendbufv_.clear();
  sendsum_ = 0;

  parse_state_ = {};

  connected_ = false;

  conn_.disconnect();

  assert(recvbuf_.rleft() == 0);
  recvbuf_.reset();
}

int MemcachedConnection::initiate_connection() {
  assert(conn_.fd == -1);

  conn_.fd = util::create_nonblock_socket(addr_->storage.ss_family);

  if (conn_.fd == -1) {
    auto error = errno;
    MCLOG(WARN, this) << "socket() failed; errno=" << error;

    return -1;
  }

  int rv;
  rv = connect(conn_.fd, &addr_->sa, addrlen_);
  if (rv != 0 && errno != EINPROGRESS) {
    auto error = errno;
    MCLOG(WARN, this) << "connect() failed; errno=" << error;

    close(conn_.fd);
    conn_.fd = -1;

    return -1;
  }

  if (LOG_ENABLED(INFO)) {
    MCLOG(INFO, this) << "Connecting to memcached server";
  }

  ev_io_set(&conn_.wev, conn_.fd, EV_WRITE);
  ev_io_set(&conn_.rev, conn_.fd, EV_READ);

  ev_set_cb(&conn_.wev, connectcb);

  conn_.wlimit.startw();
  ev_timer_again(conn_.loop, &conn_.wt);

  return 0;
}

int MemcachedConnection::on_connect() {
  if (!util::check_socket_connected(conn_.fd)) {
    conn_.wlimit.stopw();

    if (LOG_ENABLED(INFO)) {
      MCLOG(INFO, this) << "memcached connect failed";
    }

    return -1;
  }

  if (LOG_ENABLED(INFO)) {
    MCLOG(INFO, this) << "connected to memcached server";
  }

  connected_ = true;

  ev_set_cb(&conn_.wev, writecb);

  conn_.rlimit.startw();
  ev_timer_again(conn_.loop, &conn_.rt);

  return 0;
}

int MemcachedConnection::on_write() {
  if (!connected_) {
    return 0;
  }

  ev_timer_again(conn_.loop, &conn_.rt);

  if (sendq_.empty()) {
    conn_.wlimit.stopw();
    ev_timer_stop(conn_.loop, &conn_.wt);

    return 0;
  }

  int rv;

  for (; !sendq_.empty();) {
    rv = send_request();

    if (rv < 0) {
      return -1;
    }

    if (rv == 1) {
      // blocked
      return 0;
    }
  }

  conn_.wlimit.stopw();
  ev_timer_stop(conn_.loop, &conn_.wt);

  return 0;
}

int MemcachedConnection::on_read() {
  if (!connected_) {
    return 0;
  }

  ev_timer_again(conn_.loop, &conn_.rt);

  for (;;) {
    auto nread = conn_.read_clear(recvbuf_.last, recvbuf_.wleft());

    if (nread == 0) {
      return 0;
    }

    if (nread < 0) {
      return -1;
    }

    recvbuf_.write(nread);

    if (parse_packet() != 0) {
      return -1;
    }
  }

  return 0;
}

int MemcachedConnection::parse_packet() {
  auto in = recvbuf_.pos;

  for (;;) {
    auto busy = false;

    switch (parse_state_.state) {
    case MEMCACHED_PARSE_HEADER24: {
      if (recvbuf_.last - in < 24) {
        recvbuf_.drain_reset(in - recvbuf_.pos);
        return 0;
      }

      if (recvq_.empty()) {
        MCLOG(WARN, this)
            << "Response received, but there is no in-flight request.";
        return -1;
      }

      auto &req = recvq_.front();

      if (*in != MEMCACHED_RES_MAGIC) {
        MCLOG(WARN, this) << "Response has bad magic: "
                          << static_cast<uint32_t>(*in);
        return -1;
      }
      ++in;

      parse_state_.op = *in++;
      parse_state_.keylen = util::get_uint16(in);
      in += 2;
      parse_state_.extralen = *in++;
      // skip 1 byte reserved data type
      ++in;
      parse_state_.status_code = util::get_uint16(in);
      in += 2;
      parse_state_.totalbody = util::get_uint32(in);
      in += 4;
      // skip 4 bytes opaque
      in += 4;
      parse_state_.cas = util::get_uint64(in);
      in += 8;

      if (req->op != parse_state_.op) {
        MCLOG(WARN, this)
            << "opcode in response does not match to the request: want "
            << static_cast<uint32_t>(req->op) << ", got " << parse_state_.op;
        return -1;
      }

      if (parse_state_.keylen != 0) {
        MCLOG(WARN, this) << "zero length keylen expected: got "
                          << parse_state_.keylen;
        return -1;
      }

      if (parse_state_.totalbody > 16_k) {
        MCLOG(WARN, this) << "totalbody is too large: got "
                          << parse_state_.totalbody;
        return -1;
      }

      if (parse_state_.op == MEMCACHED_OP_GET &&
          parse_state_.status_code == 0 && parse_state_.extralen == 0) {
        MCLOG(WARN, this) << "response for GET does not have extra";
        return -1;
      }

      if (parse_state_.totalbody <
          parse_state_.keylen + parse_state_.extralen) {
        MCLOG(WARN, this) << "totalbody is too short: totalbody "
                          << parse_state_.totalbody << ", want min "
                          << parse_state_.keylen + parse_state_.extralen;
        return -1;
      }

      if (parse_state_.extralen) {
        parse_state_.state = MEMCACHED_PARSE_EXTRA;
        parse_state_.read_left = parse_state_.extralen;
      } else {
        parse_state_.state = MEMCACHED_PARSE_VALUE;
        parse_state_.read_left = parse_state_.totalbody - parse_state_.keylen -
                                 parse_state_.extralen;
      }
      busy = true;
      break;
    }
    case MEMCACHED_PARSE_EXTRA: {
      // We don't use extra for now. Just read and forget.
      auto n = std::min(static_cast<size_t>(recvbuf_.last - in),
                        parse_state_.read_left);

      parse_state_.read_left -= n;
      in += n;
      if (parse_state_.read_left) {
        recvbuf_.reset();
        return 0;
      }
      parse_state_.state = MEMCACHED_PARSE_VALUE;
      // since we require keylen == 0, totalbody - extralen ==
      // valuelen
      parse_state_.read_left =
          parse_state_.totalbody - parse_state_.keylen - parse_state_.extralen;
      busy = true;
      break;
    }
    case MEMCACHED_PARSE_VALUE: {
      auto n = std::min(static_cast<size_t>(recvbuf_.last - in),
                        parse_state_.read_left);

      parse_state_.value.insert(std::end(parse_state_.value), in, in + n);

      parse_state_.read_left -= n;
      in += n;
      if (parse_state_.read_left) {
        recvbuf_.reset();
        return 0;
      }

      if (LOG_ENABLED(INFO)) {
        if (parse_state_.status_code) {
          MCLOG(INFO, this)
              << "response returned error status: " << parse_state_.status_code;
        }
      }

      auto req = std::move(recvq_.front());
      recvq_.pop_front();

      if (!req->canceled && req->cb) {
        req->cb(req.get(), MemcachedResult(parse_state_.status_code,
                                           std::move(parse_state_.value)));
      }

      parse_state_ = {};
      break;
    }
    }

    if (!busy && in == recvbuf_.last) {
      break;
    }
  }

  assert(in == recvbuf_.last);
  recvbuf_.reset();

  return 0;
}

int MemcachedConnection::send_request() {
  ssize_t nwrite;

  if (sendsum_ == 0) {
    for (auto &req : sendq_) {
      if (req->canceled) {
        continue;
      }
      if (serialized_size(req.get()) + sendsum_ > 1300) {
        break;
      }
      sendbufv_.emplace_back();
      sendbufv_.back().req = req.get();
      make_request(&sendbufv_.back(), req.get());
      sendsum_ += sendbufv_.back().left();
    }

    if (sendsum_ == 0) {
      sendq_.clear();
      return 0;
    }
  }

  std::array<struct iovec, IOV_MAX> iov;
  size_t iovlen = 0;
  for (auto &buf : sendbufv_) {
    if (iovlen + 2 > iov.size()) {
      break;
    }

    auto req = buf.req;
    if (buf.headbuf.rleft()) {
      iov[iovlen++] = {buf.headbuf.pos, buf.headbuf.rleft()};
    }
    if (buf.send_value_left) {
      iov[iovlen++] = {req->value.data() + req->value.size() -
                           buf.send_value_left,
                       buf.send_value_left};
    }
  }

  nwrite = conn_.writev_clear(iov.data(), iovlen);
  if (nwrite < 0) {
    return -1;
  }
  if (nwrite == 0) {
    return 1;
  }

  sendsum_ -= nwrite;

  while (nwrite > 0) {
    auto &buf = sendbufv_.front();
    auto &req = sendq_.front();
    if (req->canceled) {
      sendq_.pop_front();
      continue;
    }
    assert(buf.req == req.get());
    auto n = std::min(static_cast<size_t>(nwrite), buf.headbuf.rleft());
    buf.headbuf.drain(n);
    nwrite -= n;
    n = std::min(static_cast<size_t>(nwrite), buf.send_value_left);
    buf.send_value_left -= n;
    nwrite -= n;

    if (buf.headbuf.rleft() || buf.send_value_left) {
      break;
    }
    sendbufv_.pop_front();
    recvq_.push_back(std::move(sendq_.front()));
    sendq_.pop_front();
  }

  return 0;
}

size_t MemcachedConnection::serialized_size(MemcachedRequest *req) {
  switch (req->op) {
  case MEMCACHED_OP_GET:
    return 24 + req->key.size();
  case MEMCACHED_OP_ADD:
  default:
    return 24 + 8 + req->key.size() + req->value.size();
  }
}

void MemcachedConnection::make_request(MemcachedSendbuf *sendbuf,
                                       MemcachedRequest *req) {
  auto &headbuf = sendbuf->headbuf;

  std::fill(std::begin(headbuf.buf), std::end(headbuf.buf), 0);

  headbuf[0] = MEMCACHED_REQ_MAGIC;
  headbuf[1] = req->op;
  switch (req->op) {
  case MEMCACHED_OP_GET:
    util::put_uint16be(&headbuf[2], req->key.size());
    util::put_uint32be(&headbuf[8], req->key.size());
    headbuf.write(24);
    break;
  case MEMCACHED_OP_ADD:
    util::put_uint16be(&headbuf[2], req->key.size());
    headbuf[4] = 8;
    util::put_uint32be(&headbuf[8], 8 + req->key.size() + req->value.size());
    util::put_uint32be(&headbuf[28], req->expiry);
    headbuf.write(32);
    break;
  }

  headbuf.write(req->key.c_str(), req->key.size());

  sendbuf->send_value_left = req->value.size();
}

int MemcachedConnection::add_request(std::unique_ptr<MemcachedRequest> req) {
  sendq_.push_back(std::move(req));

  if (connected_) {
    signal_write();
    return 0;
  }

  if (conn_.fd == -1) {
    if (initiate_connection() != 0) {
      return -1;
    }
  }

  return 0;
}

// TODO should we start write timer too?
void MemcachedConnection::signal_write() { conn_.wlimit.startw(); }

} // namespace shrpx
