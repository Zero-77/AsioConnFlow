#include <boost/asio.hpp>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>
#include <atomic>
#include <csignal>
#include <chrono> //�O���ɶ�

using boost::asio::ip::tcp;

// �����̤j�s�u�ƥH�ŦX�����ؼ�
constexpr int MAX_CONNECTIONS = 5000;

// ����έp�ܼơG�l�ܬ��D�s�u�B�B�z�T���ơB�ڵ��s�u��
std::atomic<int> active_connections{ 0 };
std::atomic<int> messages_processed{ 0 };
std::atomic<int> rejected_connections{ 0 };

std::vector<int> latency_samples;    //�x�s�C���B�z�Ӯɡ]�L��^
std::mutex latency_mutex;   //�O�@ latency_samples ���s��

// Session ���O�G�N��@�� client �s�u�A�t�d�B�zŪ�g
class Session : public std::enable_shared_from_this<Session> {
public:
    Session(tcp::socket socket) : socket_(std::move(socket)) {
        active_connections++;  // �s�s�u�إ߮ɡA���D�s�u�ƥ[�@
    }

    ~Session() {
        active_connections--; // �s�u�����ɡA���D�s�u�ƴ�@
    }

    void start() {
        do_read(); // �Ұ�Ū���y�{
    }

private:
    // �D�P�BŪ�� client ���
    void do_read() {
        auto self = shared_from_this();
        socket_.async_read_some(boost::asio::buffer(data_),
            [self](boost::system::error_code ec, std::size_t length) {
                if (!ec) {
                    messages_processed++; // ���\�B�z�@���T��

                    auto start = std::chrono::steady_clock::now();  //�O���}�l�ɶ�

                    std::string response = "Echo: " + std::string(self->data_, length);

                    // �D�P�B�^�g Echo �^��
                    boost::asio::async_write(self->socket_, boost::asio::buffer(response),
                        [self, start](boost::system::error_code ec, std::size_t /*length*/) {
                            if (!ec) {
                                // �p��B�z�Ӯɡ]�L��^
                                auto end = std::chrono::steady_clock::now();
                                auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

                                // �x�s latency �έp
                                {
                                    std::lock_guard<std::mutex> lock(latency_mutex);
                                    latency_samples.push_back(static_cast<int>(duration_us));
                                }
                                self->do_read(); // �Φ�Ū���g��Ū�`���A�^�g���\���~��U�@��Ū��
                            }
                        });
                }
            });
    }

    tcp::socket socket_;    //client �� TCP socket
    enum { max_length = 1024 };
    char data_[max_length];     // �����w�İ�
};

// Server ���O�G�t�d��ť�B�����s�u�B�έp�P�����y�{
class Server {
public:
    Server(boost::asio::io_context& io_context, tcp::endpoint endpoint)
        : io_context_(io_context),
        acceptor_(io_context, endpoint),
        stats_timer_(io_context),
        signals_(io_context, SIGINT, SIGTERM) {
        start_accept();      // �ҰʫD�P�B�����s�u
        start_stats();       // �Ұʲέp�p�ɾ�
        start_signal_wait(); // �ҰʰT����ť
    }

private:
    void start_accept() {
        //�D�P�B�����s�s�u�A�C���إ߷s socket�A�קK���ƨϥ�
        auto new_socket = std::make_shared<tcp::socket>(io_context_);
        acceptor_.async_accept(*new_socket,
            [this, new_socket](boost::system::error_code ec) {
                if (!ec) {
                    if (active_connections.load() >= MAX_CONNECTIONS) {
                        // �W�L�̤j�s�u�� �A�D������ socket
                        boost::system::error_code close_ec;
                        new_socket->close(close_ec); // �ڵ��W�L�W�����s�u
                        rejected_connections++;
                    }
                    else {
                        // �إ� Session �B�z�ӳs�u
                        std::make_shared<Session>(std::move(*new_socket))->start();
                    }
                }
                start_accept(); // �L���󻼰j�I�s�A�T�O���򱵨��s�u
            });
    }

    // �C���X�έp��T�]���D�s�u�BQPS�B�ڵ��ơ^
    void start_stats() {
        stats_timer_.expires_after(std::chrono::seconds(1));
        stats_timer_.async_wait([this](boost::system::error_code ec) {
            if (!ec) {
                /*std::cout << "[Server] Active:" << active_connections
                    << " | QPS:" << messages_processed
                    << " | Rejected:" << rejected_connections << "\n";*/

                    // �p�⥭�� latency�]�L��^
                int avg_latency = 0;
                {
                    std::lock_guard<std::mutex> lock(latency_mutex);
                    if (!latency_samples.empty()) {
                        int total = 0;
                        for (int v : latency_samples) total += v;
                        avg_latency = total / static_cast<int>(latency_samples.size());
                        latency_samples.clear(); // �C��M�šA�קK�ֿn
                    }
                }

                std::cout << "[Server] Active connections: " << active_connections
                    << " | Messages processed: " << messages_processed
                    << " | Rejected: " << rejected_connections
                    << " | Avg latency: " << avg_latency << "us\n";

                messages_processed = 0; // �C���] QPS �έp
                start_stats();          // �A���Ƶ{�U�@���έp
            }
            });
    }

    // ��ť SIGINT/SIGTERM
    void start_signal_wait() {
        signals_.async_wait([this](boost::system::error_code ec, int signal_number) {
            if (!ec) {
                std::cout << "\n[Server] Caught signal " << signal_number << ", shutting down...\n";

                // �w�������y�{
                boost::system::error_code ignore_ec;
                acceptor_.close(ignore_ec); // ������s�s�u
                stats_timer_.cancel();     // ����έp�p�ɾ�
                io_context_.stop();        // ����ƥ�j��
            }
            });
    }

    boost::asio::io_context& io_context_;
    tcp::acceptor acceptor_;
    boost::asio::steady_timer stats_timer_;
    boost::asio::signal_set signals_;
};

int main() {
    try {
        boost::asio::io_context io;
        tcp::endpoint endpoint(tcp::v4(), 12345);
        Server server(io, endpoint);

        // �إ� thread pool �B�z io_context ���ƥ�A�i�T�w thread �ơ]�Ҧp 8 �� 16�^�H�Q�����@�P��
        /*unsigned int thread_count = std::thread::hardware_concurrency();*/
        unsigned int thread_count = 8;
        std::vector<std::thread> threads;
        for (unsigned int i = 0; i < thread_count; i++) {
            threads.emplace_back([&io]() { io.run(); });
        }
        for (auto& t : threads) t.join();
    }
    catch (std::exception& e) {
        std::cerr << "Server exception: " << e.what() << "\n";
    }

    return 0;
}
