First off: You can broadcast UDP, but that's not to connected clients. That's just... UDP.

Secondly, that link shows how to have a condition-variable (event) like interface in Asio. That's only a tiny part of your problem. You forgot about the big picture: you need to know about the set of open connections, one way or the other:

 0. e.g. keeping a container of session pointers (`weak_ptr`) to each connection
 0. each connection subscribing to a signal slot (e.g. [Boost Signals][1]).

Option 1. is great for performance, option 2. is better for flexibility (decoupling the event source from subscribers, making it possible to have  heterogenous subscribers, e.g. not from connections).

Because I think Option 1. is much simpler w.r.t to threading, better w.r.t. efficiency (you can e.g. serve all clients from one buffer without copying) and you probably don't need to doubly decouple the signal/slots, let me refer to an answer where I already showed as much for pure Asio (without Beast):

 * https://stackoverflow.com/questions/43239208/how-to-design-proper-release-of-a-boostasio-socket-or-wrapper-thereof/43243314#43243314

It shows the concept of a "connection pool" - which is essentially a thread-safe container of `weak_ptr<connection>` objects with some garbage collection logic.

## Demonstration: Introducing Echo Server

After [chatting about things](https://chat.stackoverflow.com/transcript/message/41744223#41744223) I wanted to take the time to actually demonstrate the two approaches, so it's completely clear what I'm talking about. 

First let's present a simple, run-of-the mill asynchronous TCP server with 

 - with multiple concurrent connections
 - each connected session reads from the client line-by-line, and echoes the same back to the client
 - stops accepting after 3 seconds, and exits after the last client disconnects

**<kbd>[master branch on github](https://github.com/sehe/broadcast_to_sessions/blob/master/test.cpp)</kbd>**

```cpp
#include <boost/asio.hpp>
#include <memory>
#include <list>
#include <iostream>

namespace ba = boost::asio;
using ba::ip::tcp;
using boost::system::error_code;
using namespace std::chrono_literals;
using namespace std::string_literals;

static bool s_verbose = false;

struct connection : std::enable_shared_from_this<connection> {
    connection(ba::io_context& ioc) : _s(ioc) {}

    void start() { read_loop(); }
    void send(std::string msg, bool at_front = false) {
        post(_s.get_io_service(), [=] { // _s.get_executor() for newest Asio
                if (enqueue(std::move(msg), at_front))
                write_loop();
                });
    }

    private:
    void do_echo() {
        std::string line;
        if (getline(std::istream(&_rx), line)) {
            send(std::move(line) + '\n');
        }
    }

    bool enqueue(std::string msg, bool at_front)
    { // returns true if need to start write loop
        at_front &= !_tx.empty(); // no difference
        if (at_front)
            _tx.insert(std::next(begin(_tx)), std::move(msg));
        else
            _tx.push_back(std::move(msg));

        return (_tx.size() == 1);
    }
    bool dequeue()
    { // returns true if more messages pending after dequeue
        assert(!_tx.empty());
        _tx.pop_front();
        return !_tx.empty();
    }

    void write_loop() {
        ba::async_write(_s, ba::buffer(_tx.front()), [this,self=shared_from_this()](error_code ec, size_t n) {
                if (s_verbose) std::cout << "Tx: " << n << " bytes (" << ec.message() << ")" << std::endl;
                if (!ec && dequeue()) write_loop();
                });
    }

    void read_loop() {
        ba::async_read_until(_s, _rx, "\n", [this,self=shared_from_this()](error_code ec, size_t n) {
                if (s_verbose) std::cout << "Rx: " << n << " bytes (" << ec.message() << ")" << std::endl;
                do_echo();
                if (!ec)
                read_loop();
                });
    }

    friend struct server;
    ba::streambuf          _rx;
    std::list<std::string> _tx;
    tcp::socket            _s;
};

struct server {
    server(ba::io_context& ioc) : _ioc(ioc) {
        _acc.bind({{}, 6767});
        _acc.set_option(tcp::acceptor::reuse_address());
        _acc.listen();
        accept_loop();
    }

    void stop() {
        _ioc.post([=] {
                _acc.cancel();
                _acc.close();
                });
    }

    private:
    void accept_loop() {
        auto session = std::make_shared<connection>(_acc.get_io_context());
        _acc.async_accept(session->_s, [this,session](error_code ec) {
                auto ep = ec? tcp::endpoint{} : session->_s.remote_endpoint();
                std::cout << "Accept from " << ep << " (" << ec.message() << ")" << std::endl;

                session->start();
                if (!ec)
                accept_loop();
                });
    }

    ba::io_context& _ioc;
    tcp::acceptor _acc{_ioc, tcp::v4()};
};

int main(int argc, char** argv) {
    s_verbose = argc>1 && argv[1] == "-v"s;

    ba::io_context ioc;

    server s(ioc);

    std::thread th([&ioc] { ioc.run(); }); // todo exception handling

    std::this_thread::sleep_for(3s);
    s.stop(); // active connections will continue

    th.join();
}
```

## Approach 1. Adding Broadcast Messages

So, let's add "broadcast messages" that get sent to all active connections simultaneously. We add two:

 - one at each new connection (saying "Player ## has entered the game")
 - one that emulates a global "server event", like you described in the question). It gets triggered from within main:

     ```cpp
     std::this_thread::sleep_for(1s);

     auto n = s.broadcast("random global event broadcast\n");
     std::cout << "Global event broadcast reached " << n << " active connections\n";
     ```

Note how we do this by registering a weak pointer to each accepted connection and operating on each:

```cpp
_acc.async_accept(session->_s, [this,session](error_code ec) {
        auto ep = ec? tcp::endpoint{} : session->_s.remote_endpoint();
        std::cout << "Accept from " << ep << " (" << ec.message() << ")" << std::endl;

        if (!ec) {
        auto n = reg_connection(session);

        session->start();
        accept_loop();

        broadcast("player #" + std::to_string(n) + " has entered the game\n");
        }

        });
```

`broadcast` is also used directly from `main` and is simply:

```cpp
size_t broadcast(std::string const& msg) {
    return for_each_active([msg](connection& c) { c.send(msg, true); });
}
```

**<kbd>[`using-asio-post` branch on github](https://github.com/sehe/broadcast_to_sessions/blob/using-asio-post/test.cpp)</kbd>**

```cpp
#include <boost/asio.hpp>
#include <memory>
#include <list>
#include <iostream>

namespace ba = boost::asio;
using ba::ip::tcp;
using boost::system::error_code;
using namespace std::chrono_literals;
using namespace std::string_literals;

static bool s_verbose = false;

struct connection : std::enable_shared_from_this<connection> {
    connection(ba::io_context& ioc) : _s(ioc) {}

    void start() { read_loop(); }
    void send(std::string msg, bool at_front = false) {
        post(_s.get_io_service(), [=] { // _s.get_executor() for newest Asio
                if (enqueue(std::move(msg), at_front))
                write_loop();
                });
    }

    private:
    void do_echo() {
        std::string line;
        if (getline(std::istream(&_rx), line)) {
            send(std::move(line) + '\n');
        }
    }

    bool enqueue(std::string msg, bool at_front)
    { // returns true if need to start write loop
        at_front &= !_tx.empty(); // no difference
        if (at_front)
            _tx.insert(std::next(begin(_tx)), std::move(msg));
        else
            _tx.push_back(std::move(msg));

        return (_tx.size() == 1);
    }
    bool dequeue()
    { // returns true if more messages pending after dequeue
        assert(!_tx.empty());
        _tx.pop_front();
        return !_tx.empty();
    }

    void write_loop() {
        ba::async_write(_s, ba::buffer(_tx.front()), [this,self=shared_from_this()](error_code ec, size_t n) {
                if (s_verbose) std::cout << "Tx: " << n << " bytes (" << ec.message() << ")" << std::endl;
                if (!ec && dequeue()) write_loop();
                });
    }

    void read_loop() {
        ba::async_read_until(_s, _rx, "\n", [this,self=shared_from_this()](error_code ec, size_t n) {
                if (s_verbose) std::cout << "Rx: " << n << " bytes (" << ec.message() << ")" << std::endl;
                do_echo();
                if (!ec)
                read_loop();
                });
    }

    friend struct server;
    ba::streambuf          _rx;
    std::list<std::string> _tx;
    tcp::socket            _s;
};

struct server {
    server(ba::io_context& ioc) : _ioc(ioc) {
        _acc.bind({{}, 6767});
        _acc.set_option(tcp::acceptor::reuse_address());
        _acc.listen();
        accept_loop();
    }

    void stop() {
        _ioc.post([=] {
                _acc.cancel();
                _acc.close();
                });
    }

    size_t broadcast(std::string const& msg) {
        return for_each_active([msg](connection& c) { c.send(msg, true); });
    }

    private:
    using connptr = std::shared_ptr<connection>;
    using weakptr = std::weak_ptr<connection>;

    std::mutex _mx;
    std::vector<weakptr> _registered;

    size_t reg_connection(weakptr wp) {
        std::lock_guard<std::mutex> lk(_mx);
        _registered.push_back(wp);
        return _registered.size();
    }

    template <typename F>
        size_t for_each_active(F f) {
            std::vector<connptr> active;
            {
                std::lock_guard<std::mutex> lk(_mx);
                for (auto& w : _registered)
                    if (auto c = w.lock())
                        active.push_back(c);
            }

            for (auto& c : active) {
                std::cout << "(running action for " << c->_s.remote_endpoint() << ")" << std::endl;
                f(*c);
            }

            return active.size();
        }

    void accept_loop() {
        auto session = std::make_shared<connection>(_acc.get_io_context());
        _acc.async_accept(session->_s, [this,session](error_code ec) {
                auto ep = ec? tcp::endpoint{} : session->_s.remote_endpoint();
                std::cout << "Accept from " << ep << " (" << ec.message() << ")" << std::endl;

                if (!ec) {
                auto n = reg_connection(session);

                session->start();
                accept_loop();

                broadcast("player #" + std::to_string(n) + " has entered the game\n");
                }

                });
    }

    ba::io_context& _ioc;
    tcp::acceptor _acc{_ioc, tcp::v4()};
};

int main(int argc, char** argv) {
    s_verbose = argc>1 && argv[1] == "-v"s;

    ba::io_context ioc;

    server s(ioc);

    std::thread th([&ioc] { ioc.run(); }); // todo exception handling

    std::this_thread::sleep_for(1s);

    auto n = s.broadcast("random global event broadcast\n");
    std::cout << "Global event broadcast reached " << n << " active connections\n";

    std::this_thread::sleep_for(2s);
    s.stop(); // active connections will continue

    th.join();
}
```

## Approach 2: Those Broadcast But With Boost Signals2

The Signals approach is a fine example of [Dependency Inversion](https://en.wikipedia.org/wiki/Inversion_of_control).

Most salient notes:

 - signal slots get invoked on the thread invoking it ("raising the event")
 - the `scoped_connection` is there so subscriptions are ***automatically** removed when the `connection` is destructed
 - there's [subtle difference in the wording of the console message](https://github.com/sehe/broadcast_to_sessions/compare/using-asio-post...using-signals2?expand=1#diff-dfe5c65cf4ac041daf85f8f5f3d6dd74L156) from "reached # active connections" to "reached # active **subscribers**". 

> _The difference is key to understanding the added flexibility: the signal owner/invoker does not know anything about the subscribers. That's the decoupling/dependency inversion we're talking about_

**<kbd>[`using-signals2` branch on github](https://github.com/sehe/broadcast_to_sessions/blob/using-signals2/test.cpp)</kbd>**

```cpp
#include <boost/asio.hpp>
#include <memory>
#include <list>
#include <iostream>
#include <boost/signals2.hpp>

namespace ba = boost::asio;
using ba::ip::tcp;
using boost::system::error_code;
using namespace std::chrono_literals;
using namespace std::string_literals;

static bool s_verbose = false;

struct connection : std::enable_shared_from_this<connection> {
    connection(ba::io_context& ioc) : _s(ioc) {}

    void start() { read_loop(); }
    void send(std::string msg, bool at_front = false) {
        post(_s.get_io_service(), [=] { // _s.get_executor() for newest Asio
                if (enqueue(std::move(msg), at_front))
                write_loop();
                });
    }

    private:
    void do_echo() {
        std::string line;
        if (getline(std::istream(&_rx), line)) {
            send(std::move(line) + '\n');
        }
    }

    bool enqueue(std::string msg, bool at_front)
    { // returns true if need to start write loop
        at_front &= !_tx.empty(); // no difference
        if (at_front)
            _tx.insert(std::next(begin(_tx)), std::move(msg));
        else
            _tx.push_back(std::move(msg));

        return (_tx.size() == 1);
    }
    bool dequeue()
    { // returns true if more messages pending after dequeue
        assert(!_tx.empty());
        _tx.pop_front();
        return !_tx.empty();
    }

    void write_loop() {
        ba::async_write(_s, ba::buffer(_tx.front()), [this,self=shared_from_this()](error_code ec, size_t n) {
                if (s_verbose) std::cout << "Tx: " << n << " bytes (" << ec.message() << ")" << std::endl;
                if (!ec && dequeue()) write_loop();
                });
    }

    void read_loop() {
        ba::async_read_until(_s, _rx, "\n", [this,self=shared_from_this()](error_code ec, size_t n) {
                if (s_verbose) std::cout << "Rx: " << n << " bytes (" << ec.message() << ")" << std::endl;
                do_echo();
                if (!ec)
                read_loop();
                });
    }

    friend struct server;
    ba::streambuf          _rx;
    std::list<std::string> _tx;
    tcp::socket            _s;

    boost::signals2::scoped_connection _subscription;
};

struct server {
    server(ba::io_context& ioc) : _ioc(ioc) {
        _acc.bind({{}, 6767});
        _acc.set_option(tcp::acceptor::reuse_address());
        _acc.listen();
        accept_loop();
    }

    void stop() {
        _ioc.post([=] {
                _acc.cancel();
                _acc.close();
                });
    }

    size_t broadcast(std::string const& msg) {
        _broadcast_event(msg);
        return _broadcast_event.num_slots();
    }

    private:
    boost::signals2::signal<void(std::string const& msg)> _broadcast_event;

    size_t reg_connection(connection& c) {
        c._subscription = _broadcast_event.connect(
                [&c](std::string msg){ c.send(msg, true); }
                );

        return _broadcast_event.num_slots();
    }

    void accept_loop() {
        auto session = std::make_shared<connection>(_acc.get_io_context());
        _acc.async_accept(session->_s, [this,session](error_code ec) {
                auto ep = ec? tcp::endpoint{} : session->_s.remote_endpoint();
                std::cout << "Accept from " << ep << " (" << ec.message() << ")" << std::endl;

                if (!ec) {
                auto n = reg_connection(*session);

                session->start();
                accept_loop();

                broadcast("player #" + std::to_string(n) + " has entered the game\n");
                }

                });
    }

    ba::io_context& _ioc;
    tcp::acceptor _acc{_ioc, tcp::v4()};
};

int main(int argc, char** argv) {
    s_verbose = argc>1 && argv[1] == "-v"s;

    ba::io_context ioc;

    server s(ioc);

    std::thread th([&ioc] { ioc.run(); }); // todo exception handling

    std::this_thread::sleep_for(1s);

    auto n = s.broadcast("random global event broadcast\n");
    std::cout << "Global event broadcast reached " << n << " active subscribers\n";

    std::this_thread::sleep_for(2s);
    s.stop(); // active connections will continue

    th.join();
}
```

> See the diff between Approach 1. and 2.: **<kbd>[Compare View on github](https://github.com/sehe/broadcast_to_sessions/compare/using-asio-post...using-signals2?expand=1#diff-dfe5c65cf4ac041daf85f8f5f3d6dd74)</kbd>**

A sample of the output when run against 3 concurrent clients with:

```bash
(for a in {1..3}; do netcat localhost 6767 < /etc/dictionaries-common/words > echoed.$a& sleep .1; done; time wait)
```

[![enter image description here][1]][1]


  [1]: https://i.stack.imgur.com/fNumi.png
