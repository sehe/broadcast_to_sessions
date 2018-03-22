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
