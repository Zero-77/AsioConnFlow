#include <boost/asio.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/ssl.hpp>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>
#include <atomic>
#include <csignal>
#include <chrono> //記錄時間

using boost::asio::ip::tcp;

// 提高最大連線數以符合壓測目標
constexpr int MAX_CONNECTIONS = 10000;

// 全域統計變數：追蹤活躍連線、處理訊息數、拒絕連線數
std::atomic<int> total_connections{ 0 };
std::atomic<int> messages_processed{ 0 };
std::atomic<int> rejected_connections{ 0 };

std::atomic<int> active_tcp_connections{ 0 };
std::atomic<int> active_tls_connections{ 0 };

std::vector<int> latency_samples;    //儲存每筆處理耗時（微秒）
std::mutex latency_mutex;   //保護 latency_samples 的存取


// Session 類別：代表一個 client 連線，負責處理讀寫
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

    // 連線數加減會是個問題，甚麼時候算是成功/失敗 要確認
    ~Session() {
        if (armed_tcp_) active_tcp_connections--;
        if (armed_tls_) active_tls_connections--;
    }

    void start() {

        armed_tcp_ = true;
        active_tcp_connections++;  // 新連線建立時，TCP活躍連線數加一

        auto self = shared_from_this();
        ssl_stream_.async_handshake(boost::asio::ssl::stream_base::server,
            [self](const boost::system::error_code& ec) {
                if (!ec) {
                     // 握手成功了，但尚未計入 TLS 連線
                    // 先確認是否超過上限
                    if (active_tls_connections.load() >= MAX_CONNECTIONS) {
                        boost::system::error_code close_ec;
                        self->stream().lowest_layer().close(close_ec);
                        std::cout << "[Server] TLS handshake rejected (limit reached)\n";
                        rejected_connections++;
                        return; // 直接返回：這個連線不應 armed_tls_，不應計數
                    }

                    // 真正接受：只有在這裡才 armed + increment
                    self->handshake_ok_ = true;
                    self->armed_tls_ = true;
                    active_tls_connections++;

                    // 進入讀寫循環
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
                        //目前NoTLS 模式會印出Handshake failed: packet length too long (SSL routines)
                    }

                    //std::cerr << "[Server] TLS handshake failed: " << reason << "\n"; 

                    std::cerr << "[Server] TLS handshake failed: " << ec.message() << "\n";

                    self->graceful_close();
                }
            });
    }

    void graceful_close() {
        if (closed_.exchange(true)) return; // 防重入

        boost::system::error_code ec;
        if (handshake_ok_) {
            ssl_stream_.shutdown(ec); // 盡量送 close_notify
        }
        ssl_stream_.lowest_layer().shutdown(tcp::socket::shutdown_both, ec);
        ssl_stream_.lowest_layer().close(ec);
    }

private:
    // 非同步讀取 client 資料
    void do_read() {
        auto self = shared_from_this();
        ssl_stream_.async_read_some(boost::asio::buffer(data_),
            [self](boost::system::error_code ec, std::size_t length) {
                if (!ec) {
                    messages_processed++; // 成功處理一筆訊息

                    auto start = std::chrono::steady_clock::now();  //記錄開始時間

                    std::string response = "Echo: " + std::string(self->data_, length);

                    // 非同步回寫 Echo 回應
                    boost::asio::async_write(self->ssl_stream_, boost::asio::buffer(response),
                        [self, start](boost::system::error_code ec2, std::size_t) {
                            if (!ec2) {
                                // 計算處理耗時（微秒）
                                auto end = std::chrono::steady_clock::now();
                                auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

                                // 儲存 latency 統計
                                {
                                    std::lock_guard<std::mutex> lock(latency_mutex);
                                    latency_samples.push_back(static_cast<int>(duration_us));
                                }
                                self->do_read(); // 形成讀→寫→讀循環，回寫成功後繼續下一輪讀取
                            }
                            else {
                                self->graceful_close(); //確保釋放與計數遞減。
                            }
                        });
                }
                else {
                    self->graceful_close(); //確保釋放與計數遞減。
                }
            });
    }

    //tcp::socket socket_;    //client 的 TCP socket
    enum { max_length = 1024 };
    char data_[max_length];     // 接收緩衝區
    boost::asio::ssl::stream<tcp::socket> ssl_stream_;

    bool handshake_ok_ = false;       // 握手是否成功
    bool armed_tcp_ = false;          // 是否已加 active_tcp_connections
    bool armed_tls_ = false;          // 是否已加 active_tls_connections
    std::atomic<bool> closed_{ false }; //graceful_close 防止多次關閉
};

// Server 類別：負責監聽、接受連線、統計與關閉流程
class Server {
public:
    Server(boost::asio::io_context& io_context, tcp::endpoint endpoint, boost::asio::ssl::context& ssl_ctx)
        : io_context_(io_context),
        acceptor_(io_context),
        ssl_ctx_(ssl_ctx),
        stats_timer_(io_context),
        signals_(io_context, SIGINT, SIGTERM) {

        // 手動 open/bind/listen
        boost::system::error_code ec;
        acceptor_.open(endpoint.protocol(), ec);
        if (ec) throw std::runtime_error("acceptor open failed: " + ec.message());

        acceptor_.set_option(boost::asio::socket_base::reuse_address(true), ec);
        if (ec) std::cerr << "[Server] set_option(reuse_address) failed: " << ec.message() << "\n";

        acceptor_.bind(endpoint, ec);
        if (ec) throw std::runtime_error("acceptor bind failed: " + ec.message());

        acceptor_.listen(boost::asio::socket_base::max_listen_connections, ec);
        if (ec) std::cerr << "[Server] listen failed: " << ec.message() << "\n";


        start_accept();      // 啟動非同步接受連線
        start_stats();       // 啟動統計計時器
        start_signal_wait(); // 啟動訊號監聽
    }

private:
    void start_accept() {
        //非同步接受新連線，每次建立新 socket，避免重複使用
        auto session = std::make_shared<Session>(io_context_, ssl_ctx_);
        auto& socket = session->stream().lowest_layer(); // 取得底層 TCP socket

        acceptor_.async_accept(socket,
            [this, session](boost::system::error_code ec) {
                if (!ec) {
                    total_connections++; //統計所有連線嘗試(不論是否成功握手)

                     // 建立 Session 處理該連線，啟動 TLS sessio
                    session->start();

                }
                else {
                    std::cerr << "[Server] Accept failed: " << ec.message() << "\n";
                }
                start_accept(); // 無條件遞迴呼叫，持續接受下一條連線
            });
    }

    // 每秒輸出統計資訊（活躍連線、QPS、拒絕數）
    void start_stats() {
        stats_timer_.expires_after(std::chrono::seconds(1));
        stats_timer_.async_wait([this](boost::system::error_code ec) {
            if (!ec) {
                /*std::cout << "[Server] Active:" << active_connections
                    << " | QPS:" << messages_processed
                    << " | Rejected:" << rejected_connections << "\n";*/

                    // 計算平均 latency（微秒）
                int avg_latency = 0;
                {
                    std::lock_guard<std::mutex> lock(latency_mutex);
                    if (!latency_samples.empty()) {
                        int total = 0;
                        for (int v : latency_samples) total += v;
                        avg_latency = total / static_cast<int>(latency_samples.size());
                        latency_samples.clear(); // 每秒清空，避免累積
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

                messages_processed = 0; // 每秒重設 QPS 統計
                start_stats();          // 再次排程下一次統計
            }
            });
    }

    // 監聽 SIGINT/SIGTERM
    void start_signal_wait() {
        signals_.async_wait([this](boost::system::error_code ec, int signal_number) {
            if (!ec) {
                std::cout << "\n[Server] Caught signal " << signal_number << ", shutting down...\n";

                // 安全關閉流程
                boost::system::error_code ignore_ec;
                acceptor_.close(ignore_ec); // 停止接受新連線
                stats_timer_.cancel();     // 停止統計計時器
                io_context_.stop();        // 停止事件迴圈
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
        //建立TLS context
        
        boost::asio::ssl::context ssl_ctx(boost::asio::ssl::context::tlsv13_server);
        //1. 載入Server的憑證與私鑰
        ssl_ctx.use_certificate_chain_file("../../../server-certs/public/server.crt");
        ssl_ctx.use_private_key_file("../../../server-certs/private/server.key", boost::asio::ssl::context::pem);

        //2. 設定驗證模式:要求client提供憑證，並強制驗證
        ssl_ctx.set_verify_mode(boost::asio::ssl::verify_peer | boost::asio::ssl::verify_fail_if_no_peer_cert);
        
        // 3. 載入 CA 憑證：用來驗證 client 的憑證是否由信任的 CA 簽發
        ssl_ctx.load_verify_file("../../../CA/ca.pem");

        tcp::endpoint endpoint(tcp::v4(), 12345);
        Server server(io, endpoint, ssl_ctx);



        // 建立 thread pool 處理 io_context 的事件，可固定 thread 數（例如 8 或 16）以利壓測一致性
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
