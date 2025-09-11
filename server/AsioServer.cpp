#include <boost/asio.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/ssl.hpp>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>
#include <atomic>
#include <csignal>
#include <chrono> //�O���ɶ�

using boost::asio::ip::tcp;

// �����̤j�s�u�ƥH�ŦX�����ؼ�
constexpr int MAX_CONNECTIONS = 10000;

// ����έp�ܼơG�l�ܬ��D�s�u�B�B�z�T���ơB�ڵ��s�u��
std::atomic<int> total_connections{ 0 };
std::atomic<int> messages_processed{ 0 };
std::atomic<int> rejected_connections{ 0 };

std::atomic<int> active_tcp_connections{ 0 };
std::atomic<int> active_tls_connections{ 0 };

std::vector<int> latency_samples;    //�x�s�C���B�z�Ӯɡ]�L��^
std::mutex latency_mutex;   //�O�@ latency_samples ���s��


// Session ���O�G�N��@�� client �s�u�A�t�d�B�zŪ�g
class Session : public std::enable_shared_from_this<Session> {
public:
    
    Session(tcp::socket socket, boost::asio::ssl::context& ctx) : ssl_stream_(std::move(socket), ctx) {
    }
    
    Session(boost::asio::io_context& io_context, boost::asio::ssl::context& ssl_ctx)
        : ssl_stream_(io_context, ssl_ctx){ 
    }
    
    boost::asio::ssl::stream<tcp::socket>& stream() {
        return ssl_stream_;
    }

    // �s�u�ƥ[��|�O�Ӱ��D�A�ƻ�ɭԺ�O���\/���� �n�T�{
    ~Session() {
        if (armed_tcp_) active_tcp_connections--;
        if (armed_tls_) active_tls_connections--;
    }

    void start() {

        armed_tcp_ = true;
        active_tcp_connections++;  // �s�s�u�إ߮ɡATCP���D�s�u�ƥ[�@

        auto self = shared_from_this();
        ssl_stream_.async_handshake(boost::asio::ssl::stream_base::server,
            [self](const boost::system::error_code& ec) {
                if (!ec) {
                     // ���⦨�\�F�A���|���p�J TLS �s�u
                    // ���T�{�O�_�W�L�W��
                    if (active_tls_connections.load() >= MAX_CONNECTIONS) {
                        boost::system::error_code close_ec;
                        self->stream().lowest_layer().close(close_ec);
                        std::cout << "[Server] TLS handshake rejected (limit reached)\n";
                        rejected_connections++;
                        return; // ������^�G�o�ӳs�u���� armed_tls_�A�����p��
                    }

                    // �u�������G�u���b�o�̤~ armed + increment
                    self->handshake_ok_ = true;
                    self->armed_tls_ = true;
                    active_tls_connections++;

                    // �i�JŪ�g�`��
                    self->do_read();
                }
                else {
                    std::string reason = "Unknown";

                    if (ec == boost::asio::error::eof) {
                        reason = "Client closed connection (EarlyClose)";
                    }
                    else if (ec == boost::asio::error::connection_reset) {
                        reason = "Client sent TCP RST";
                    }
                    else if (ec == boost::asio::ssl::error::stream_truncated) {
                        reason = "Client sent non-TLS data (NoTLS)";
                    }
                    else if (ec == boost::asio::error::operation_aborted) {
                        reason = "Handshake aborted (timeout or shutdown)";
                    }
                    else {
                        reason = "Handshake failed: " + ec.message(); 
                        //�ثeNoTLS �Ҧ��|�L�XHandshake failed: packet length too long (SSL routines)
                    }

                    //std::cerr << "[Server] TLS handshake failed: " << reason << "\n"; 

                    std::cerr << "[Server] TLS handshake failed: " << ec.message() << "\n";

                    self->graceful_close();
                }
            });
    }

    void graceful_close() {
        if (closed_.exchange(true)) return; // �����J

        boost::system::error_code ec;
        if (handshake_ok_) {
            ssl_stream_.shutdown(ec); // �ɶq�e close_notify
        }
        ssl_stream_.lowest_layer().shutdown(tcp::socket::shutdown_both, ec);
        ssl_stream_.lowest_layer().close(ec);
    }

private:
    // �D�P�BŪ�� client ���
    void do_read() {
        auto self = shared_from_this();
        ssl_stream_.async_read_some(boost::asio::buffer(data_),
            [self](boost::system::error_code ec, std::size_t length) {
                if (!ec) {
                    messages_processed++; // ���\�B�z�@���T��

                    auto start = std::chrono::steady_clock::now();  //�O���}�l�ɶ�

                    std::string response = "Echo: " + std::string(self->data_, length);

                    // �D�P�B�^�g Echo �^��
                    boost::asio::async_write(self->ssl_stream_, boost::asio::buffer(response),
                        [self, start](boost::system::error_code ec2, std::size_t) {
                            if (!ec2) {
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
                            else {
                                self->graceful_close(); //�T�O����P�p�ƻ���C
                            }
                        });
                }
                else {
                    self->graceful_close(); //�T�O����P�p�ƻ���C
                }
            });
    }

    //tcp::socket socket_;    //client �� TCP socket
    enum { max_length = 1024 };
    char data_[max_length];     // �����w�İ�
    boost::asio::ssl::stream<tcp::socket> ssl_stream_;

    bool handshake_ok_ = false;       // ����O�_���\
    bool armed_tcp_ = false;          // �O�_�w�[ active_tcp_connections
    bool armed_tls_ = false;          // �O�_�w�[ active_tls_connections
    std::atomic<bool> closed_{ false }; //graceful_close ����h������
};

// Server ���O�G�t�d��ť�B�����s�u�B�έp�P�����y�{
class Server {
public:
    Server(boost::asio::io_context& io_context, tcp::endpoint endpoint, boost::asio::ssl::context& ssl_ctx)
        : io_context_(io_context),
        acceptor_(io_context),
        ssl_ctx_(ssl_ctx),
        stats_timer_(io_context),
        signals_(io_context, SIGINT, SIGTERM) {

        // ��� open/bind/listen
        boost::system::error_code ec;
        acceptor_.open(endpoint.protocol(), ec);
        if (ec) throw std::runtime_error("acceptor open failed: " + ec.message());

        acceptor_.set_option(boost::asio::socket_base::reuse_address(true), ec);
        if (ec) std::cerr << "[Server] set_option(reuse_address) failed: " << ec.message() << "\n";

        acceptor_.bind(endpoint, ec);
        if (ec) throw std::runtime_error("acceptor bind failed: " + ec.message());

        acceptor_.listen(boost::asio::socket_base::max_listen_connections, ec);
        if (ec) std::cerr << "[Server] listen failed: " << ec.message() << "\n";


        start_accept();      // �ҰʫD�P�B�����s�u
        start_stats();       // �Ұʲέp�p�ɾ�
        start_signal_wait(); // �ҰʰT����ť
    }

private:
    void start_accept() {
        //�D�P�B�����s�s�u�A�C���إ߷s socket�A�קK���ƨϥ�
        auto session = std::make_shared<Session>(io_context_, ssl_ctx_);
        auto& socket = session->stream().lowest_layer(); // ���o���h TCP socket

        acceptor_.async_accept(socket,
            [this, session](boost::system::error_code ec) {
                if (!ec) {
                    total_connections++; //�έp�Ҧ��s�u����(���׬O�_���\����)

                     // �إ� Session �B�z�ӳs�u�A�Ұ� TLS sessio
                    session->start();

                }
                else {
                    std::cerr << "[Server] Accept failed: " << ec.message() << "\n";
                }
                start_accept(); // �L���󻼰j�I�s�A���򱵨��U�@���s�u
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
                /*
                std::cout << "[Server] Active connections: " << active_connections
                    << " | Messages processed: " << messages_processed
                    << " | Rejected: " << rejected_connections
                    << " | Avg latency: " << avg_latency << "us\n";
                */
                std::cout << "[Server] Active TLS connections: " << active_tls_connections
                    << " | Active TCP connections: " << active_tcp_connections
                    << " | Total connections: " << total_connections
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
    boost::asio::ssl::context& ssl_ctx_;
};

int main() {
    try {

        boost::asio::io_context io;

        //TLS 1.3 boost
        //�إ�TLS context
        
        boost::asio::ssl::context ssl_ctx(boost::asio::ssl::context::tlsv13_server);
        //1. ���JServer�����һP�p�_
        ssl_ctx.use_certificate_chain_file("../../../server-certs/public/server.crt");
        ssl_ctx.use_private_key_file("../../../server-certs/private/server.key", boost::asio::ssl::context::pem);

        //2. �]�w���ҼҦ�:�n�Dclient���Ѿ��ҡA�ñj������
        ssl_ctx.set_verify_mode(boost::asio::ssl::verify_peer | boost::asio::ssl::verify_fail_if_no_peer_cert);
        
        // 3. ���J CA ���ҡG�Ψ����� client �����ҬO�_�ѫH���� CA ñ�o
        ssl_ctx.load_verify_file("../../../CA/ca.pem");

        tcp::endpoint endpoint(tcp::v4(), 12345);
        Server server(io, endpoint, ssl_ctx);



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
