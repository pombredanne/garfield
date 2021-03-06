// Copyright 2012, Evan Klitzke <evan@eklitzke.org>

#include "./connection.h"

#include <boost/lexical_cast.hpp>
#include <boost/regex.hpp>

#include <cassert>
#include <iterator>
#include <fstream>
#include <functional>

#include "./logging.h"

namespace {
const boost::regex request_line("^(\\u+) (.*) HTTP/1\\.([01])$");
const boost::regex header_line("^([-a-zA-Z0-9_]+):\\s+(.*?)$");
}

namespace garfield {
Connection::Connection(boost::asio::ip::tcp::socket *sock,
                       RequestCallback callback)
    :state_(UNCONNECTED), sock_(sock), callback_(callback),
     keep_alive_(true) {
}

void Connection::NotifyConnected() {
  assert(state_ == UNCONNECTED);
  state_ = WAITING_FOR_HEADERS;
  Request *req = new Request();

  boost::asio::ip::tcp::endpoint remote_ep = sock_->remote_endpoint();
  boost::asio::ip::address remote_ad = remote_ep.address();
  req->peername = remote_ad.to_string();

  boost::asio::async_read_until(
      *sock_, req->streambuf, "\r\n\r\n",
      std::bind(
          &Connection::OnHeaders, this, req,
          std::placeholders::_1,
          std::placeholders::_2));
}

void Connection::OnHeaders(Request *req,
                           const boost::system::error_code &err,
                           std::size_t bytes_transferred) {
  assert(state_ == WAITING_FOR_HEADERS);
  if (err) {
    // A closed connection is normal -- for instance, during HTTP keep-alive,
    // clients will unexpectedly disconnect when they're done sending
    // requests. Therefore, we ignore these errors, but log all other ones.
    if (err != boost::asio::error::connection_reset &&
        err != boost::asio::error::eof) {
      std::string err_name = boost::lexical_cast<std::string>(err);
      Log(ERROR, "system error in OnHeaders, %s", err_name.c_str());
    }
    callback_(this, req, SYSTEM_ERROR);
    return;
  }

  std::string data(
      (std::istreambuf_iterator<char>(&req->streambuf)),
      std::istreambuf_iterator<char>());

  std::string::size_type offset = 0;
  while (true) {
    std::string::size_type newline = data.find("\r\n", offset);
    if (newline == std::string::npos) {
      Log(ERROR, "malformed header line!");
      callback_(this, req, MALFORMED_HEADER_LINE);
      return;
    }
    std::string line = data.substr(offset, newline - offset);
    boost::smatch what;
    if (offset == 0) {
      // we're reading the first line of the request
      if (!boost::regex_match(line, what, request_line)) {
        Log(ERROR, "malformed first line!");
        callback_(this, req, MALFORMED_FIRST_LINE);
        return;
      }
      req->method = what[1];
      req->path = what[2];
      req->version = std::make_pair(1, what[3] == "0" ? 0 : 1);
      if (what[3] == "0") {
        keep_alive_ = false;  // Force non-keep alive for HTTP/1.0
      }
    } else if (line == "") {
      break;
    } else {
      if (!boost::regex_match(line, what, header_line)) {
        Log(ERROR, "malformed header line!");
        callback_(this, req, MALFORMED_HEADER_LINE);
        return;
      }
      HeaderKey hdr_key(what[1]);
      std::string hdr_val = what[2];
      req->headers()->SetHeader(hdr_key, hdr_val);
      if (hdr_key.norm_key == "connection" && hdr_val == "close") {
        keep_alive_ = false;
      }
    }
    offset = newline + 2;
  }
  callback_(this, req, OK);
}

Connection::~Connection() {
  sock_->cancel();
  delete sock_;
}
}
