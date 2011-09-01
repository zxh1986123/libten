#include "runner.hh"
#include "task.hh"
#include "buffer.hh"
#include "http/http_message.hh"
#include "uri/uri.hh"
#include <boost/bind.hpp>

#include <iostream>

using namespace fw;

static void do_get(uri u) {
    task::socket s(AF_INET, SOCK_STREAM);
    u.normalize();
    if (u.scheme != "http") return;
    if (u.port == 0) u.port = 80;
    s.dial(u.host.c_str(), u.port);

    http_request r("GET", u.compose(true));
    // HTTP/1.1 requires host header
    r.append_header("Host", u.host);
    std::string data = r.data();
    std::cout << "Request:\n" << "--------------\n";
    std::cout << data;
    ssize_t nw = s.send(data.c_str(), data.size());

    buffer buf(4*1024);
    buffer::slice rb = buf(0);

    http_parser parser;
    http_response resp;
    resp.parser_init(&parser);

    for (;;) {
        ssize_t nr = s.recv(rb.data(), rb.size());
        if (nr <= 0) { std::cerr << "Error: " << strerror(errno) << "\n"; break; }
        if (resp.parse(&parser, rb.data(), nr)) break;
    }

    std::cout << "Response:\n" << "--------------\n";
    std::cout << resp.data();
    std::cout << "Body size: " << resp.body.size() << "\n";
}

int main(int argc, char *argv[]) {
    if (argc < 2) return -1;
    runner::init();

    uri u(argv[1]);
    task::spawn(boost::bind(do_get, u), 0, 4*1024*1024);
    return runner::main();
}