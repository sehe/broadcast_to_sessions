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
