#include "runner.hh"
#include "task.hh"
#include "descriptors.hh"
#include <boost/bind.hpp>
#include <iostream>

using namespace fw;

void echo_task(int sock) {
    task::socket s(sock);
    char buf[4096];
    for (;;) {
        ssize_t nr = s.recv(buf, sizeof(buf));
        if (nr <= 0) break;
        ssize_t nw = s.send(buf, nr);
    }
}

void listen_task() {
    task::socket s(AF_INET, SOCK_STREAM);
    s.setsockopt(SOL_SOCKET, SO_REUSEADDR, 1);
    address addr("127.0.0.1", 0);
    s.bind(addr);
    s.getsockname(addr);
    std::cout << "listening on: " << addr << "\n";
    s.listen();

    for (;;) {
        address client_addr;
        int sock;
        while ((sock = s.accept(client_addr, 0, 60*1000)) > 0) {
            task::spawn(boost::bind(echo_task, sock));
        }
        std::cout << "accept timeout reached\n";
    }
}

int main(int argc, char *argv[]) {
    runner::init();
    task::spawn(listen_task);
    return runner::main();
}
