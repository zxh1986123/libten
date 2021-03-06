#ifndef LIBTEN_NET_HH
#define LIBTEN_NET_HH

#include "ten/thread_guard.hh"
#include "ten/descriptors.hh"
#include "ten/task.hh"
#include "ten/backoff.hh"
#include <chrono_io>
#include <memory>
#include <thread>

namespace ten {

class hostname_error : public errorx {
public:
    template <class ...A>
    hostname_error(const char *s, A... args) : errorx(s, std::forward<A>(args)...) {}
};

//! perform address resolution and connect fd, task friendly, all errors by exception
void netdial(int fd, const char *addr, uint16_t port, optional_timeout connect_ms);
//! connect fd using task io scheduling
int netconnect(int fd, const address &addr, optional_timeout ms);
//! task friendly accept
int netaccept(int fd, address &addr, int flags, optional_timeout ms);
//! task friendly recv
ssize_t netrecv(int fd, void *buf, size_t len, int flags, optional_timeout ms);
//! task friendly send
ssize_t netsend(int fd, const void *buf, size_t len, int flags, optional_timeout ms);

//! pure-virtual wrapper around socket_fd
class sockbase {
public:
    socket_fd s;

    sockbase(int fd=-1) noexcept
        : s(fd) {}
    sockbase(socket_fd sfd) noexcept
        : s(std::move(sfd)) {}
    sockbase(int domain, int type, int protocol=0)
        : s(domain, type | SOCK_NONBLOCK, protocol) {}

    sockbase(const sockbase &) = delete;
    sockbase &operator =(const sockbase &) = delete;

    sockbase(sockbase &&other) = default;
    sockbase &operator = (sockbase &&other) = default;

    virtual ~sockbase() {}

    void close() { s.close(); }
    bool valid() const { return s.valid(); }

    int fcntl(int cmd) { return s.fcntl(cmd); }
    int fcntl(int cmd, long arg) { return s.fcntl(cmd, arg); }

    void bind(const address &addr) { s.bind(addr); }

    // use a ridiculous number, kernel will truncate to max
    void listen(int backlog=100000) { s.listen(backlog); }

    int shutdown(int how) __attribute__((warn_unused_result)) {
        return s.shutdown(how);
    }

    bool getpeername(address &addr) __attribute__((warn_unused_result)) {
        return s.getpeername(addr);
    }

    void getsockname(address &addr) {
         s.getsockname(addr);
    }

    template <typename T>
    void getsockopt(int level, int optname, T &optval, socklen_t &optlen) {
        s.getsockopt(level, optname, optval, optlen);
    }

    template <typename T>
    void setsockopt(int level, int optname, const T &optval, socklen_t optlen) {
        s.setsockopt(level, optname, optval, optlen);
    }

    template <typename T>
    void setsockopt(int level, int optname, const T &optval) {
        s.setsockopt(level, optname, optval);
    }

    virtual void dial(const char *addr,
            uint16_t port,
            optional_timeout timeout_ms=nullopt) = 0;

    virtual int connect(const address &addr,
            optional_timeout timeout_ms = nullopt)
        __attribute__((warn_unused_result)) = 0;

    virtual int accept(address &addr,
            int flags=0,
            optional_timeout timeout_ms = nullopt)
        __attribute__((warn_unused_result)) = 0;

    virtual ssize_t recv(void *buf,
            size_t len,
            int flags=0,
            optional_timeout timeout_ms = nullopt)
        __attribute__((warn_unused_result)) = 0;

    virtual ssize_t send(const void *buf,
            size_t len,
            int flags=0,
            optional_timeout timeout_ms = nullopt)
        __attribute__((warn_unused_result)) = 0;

    ssize_t recvall(void *buf, size_t len, optional_timeout timeout_ms=nullopt) {
        size_t pos = 0;
        ssize_t left = len;
        while (pos != len) {
            ssize_t nr = this->recv(&((char *)buf)[pos], left, 0, timeout_ms);
            if (nr > 0) {
                pos += nr;
                left -= nr;
            } else {
                break;
            }
        }
        return pos;
    }
};

//! task friendly socket wrapper
class netsock : public sockbase {
public:
    netsock(int domain, int type, int protocol=0)
        : sockbase(domain, type, protocol) {}
    netsock(int fd=-1) noexcept
        : sockbase(fd) {}

    netsock(const netsock &) = delete;
    netsock &operator =(const netsock &) = delete;

    netsock(netsock &&other) = default;
    netsock &operator = (netsock &&other) = default;

    //! dial requires a large 8MB stack size for getaddrinfo; throws on error
    void dial(const char *addr,
            uint16_t port,
            optional_timeout timeout_ms=nullopt) override;

    int connect(const address &addr,
            optional_timeout timeout_ms=nullopt) override
        __attribute__((warn_unused_result))
    {
        return netconnect(s.fd, addr, timeout_ms);
    }

    int accept(address &addr,
            int flags=0,
            optional_timeout timeout_ms=nullopt) override
        __attribute__((warn_unused_result))
    {
        return netaccept(s.fd, addr, flags, timeout_ms);
    }

    ssize_t recv(void *buf,
            size_t len,
            int flags=0,
            optional_timeout timeout_ms=nullopt) override
        __attribute__((warn_unused_result))
    {
        return netrecv(s.fd, buf, len, flags, timeout_ms);
    }

    ssize_t send(const void *buf,
            size_t len,
            int flags=0,
            optional_timeout timeout_ms=nullopt) override
        __attribute__((warn_unused_result))
    {
        return netsend(s.fd, buf, len, flags, timeout_ms);
    }
};

//! task/proc aware socket server
class netsock_server : public std::enable_shared_from_this<netsock_server> {
protected:
    netsock _sock;
    std::string _protocol_name;
    optional_timeout _recv_timeout_ms;
public:
    netsock_server(const std::string &protocol_name_,
                   nostacksize_t=nostacksize,
                   optional_timeout recv_timeout_ms=nullopt)
        : _protocol_name(protocol_name_),
          _recv_timeout_ms(recv_timeout_ms)
    {
    }

    netsock_server(const netsock_server &) = delete;
    netsock_server &operator=(const netsock_server &) = delete;

    ~netsock_server() {
    }

    //! listen and accept connections
    void serve(const std::string &ipaddr, uint16_t port, unsigned threads=1) {
        address baddr(ipaddr.c_str(), port);
        serve(baddr, threads);
    }

    //! listen and accept connections, and modify baddr to bound address
    void serve(address &baddr, unsigned threads=1) {
        // listening sockets we do want to share across exec
        netsock s = netsock(baddr.family(), SOCK_STREAM);
        int flags = s.fcntl(F_GETFD);
        throw_if(flags == -1 || s.fcntl(F_SETFD, flags & ~FD_CLOEXEC) == -1);
        setup_listen_socket(s);
        s.bind(baddr);
        serve(std::move(s), baddr, threads);
    }

    //! listen and accept connections, and modify baddr to bound address
    void serve(netsock s, address &baddr, unsigned nthreads=1) {
        _sock = std::move(s);
        _sock.getsockname(baddr);
        LOG(INFO) << "listening for " << _protocol_name
            << " on " << baddr << " with " << nthreads << " threads";
        _sock.listen();
        auto self = shared_from_this();
        std::vector<thread_guard> threads;
        try {
            for (unsigned n=1; n<nthreads; ++n) {
                threads.emplace_back(task::spawn_thread([=] {
                    self->accept_loop();
                }));
            }
            accept_loop();
        } catch (...) {
            // induce other service threads to quit, without invalidating the fd
            // until all the threads let go of self.
            if (nthreads) {
                int err = _sock.shutdown(SHUT_RDWR);
                (void)err; // during exception handling, no logging please
            }
            throw;
        }
    }

    int listen_fd() const {
        return _sock.s.fd;
    }

protected:

    virtual void setup_listen_socket(netsock &s) {
        s.setsockopt(SOL_SOCKET, SO_REUSEADDR, 1);
    }

    virtual void accept_loop() {
        using namespace std::chrono;
        const auto self = shared_from_this();
        auto bo = make_backoff(milliseconds{100}, milliseconds{500});
        for (;;) {
            address client_addr;
            int fd = _sock.accept(client_addr, 0);
            if (fd == -1) {
                const int e = errno;
                switch (e) {
                case ENFILE:
                case EMFILE:
                case ENOBUFS:
                case ENOMEM: {
                    auto delay = bo.next_delay();
                    LOG(ERROR) << "accept failed, sleeping " << delay << ": " << strerror(e);
                    this_task::sleep_for(delay);
                    break;
                  }
                default: {
                    LOG(ERROR) << "accept failed: " << strerror(e);
                    this_task::yield();
                    break;
                  }
                }
            }
            else if (fd <= 2) {
                ::close(fd);
                throw errorx("somebody closed stdin/stdout/stderr");
            }
            else {
                bool nomem = false;
                try {
                    task::spawn([=] {
                        self->client_task(fd);
                    });
                } catch (std::bad_alloc &e) {
                    ::close(fd);
                    nomem = true;
                } catch (...) {
                    ::close(fd);
                    throw;
                }
                if (nomem) {
                    auto delay = bo.next_delay();
                    LOG(ERROR) << "task spawn ran out of memory, sleeping " << delay;
                    this_task::sleep_for(delay);
                }
                else {
                    this_task::yield(); // yield to new client task
                }
            }
        }
    }

    void client_task(int fd) {
        netsock s(fd);
        try {
            on_connection(s);
        } catch (std::exception &e) {
            LOG(ERROR) << "unhandled client task error: " << e.what();
        }
    }

    virtual void on_connection(netsock &s) = 0;
};

} // end namespace ten

#endif // LIBTEN_NET_HH
