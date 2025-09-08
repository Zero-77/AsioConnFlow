#include <boost/asio.hpp>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>
#include <atomic>
#include <csignal>
#include <chrono> //記錄時間

using boost::asio::ip::tcp;

// 提高最大連線數以符合壓測目標
constexpr int MAX_CONNECTIONS = 5000;

// 全域統計變數：追蹤活躍連線、處理訊息數、拒絕連線數
std::atomic<int> active_connections{ 0 };
std::atomic<int> messages_processed{ 0 };
std::atomic<int> rejected_connections{ 0 };

std::vector<int> latency_samples;    //儲存每筆處理耗時（微秒）
std::mutex latency_mutex;   //保護 latency_samples 的存取

// Session 類別：代表一個 client 連線，負責處理讀寫
class Session : public std::enable_shared_from_this<Session> {
public:
    Session(tcp::socket socket) : socket_(std::move(socket)) {
        active_connections++;  // 新連線建立時，活躍連線數加一
    }

    ~Session() {
        active_connections--; // 連線結束時，活躍連線數減一
    }

    void start() {
        do_read(); // 啟動讀取流程
    }

private:
    // 非同步讀取 client 資料
    void do_read() {
        auto self = shared_from_this();
        socket_.async_read_some(boost::asio::buffer(data_),
            [self](boost::system::error_code ec, std::size_t length) {
                if (!ec) {
                    messages_processed++; // 成功處理一筆訊息

                    auto start = std::chrono::steady_clock::now();  //記錄開始時間

                    std::string response = "Echo: " + std::string(self->data_, length);

                    // 非同步回寫 Echo 回應
                    boost::asio::async_write(self->socket_, boost::asio::buffer(response),
                        [self, start](boost::system::error_code ec, std::size_t /*length*/) {
                            if (!ec) {
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
                        });
                }
            });
    }

    tcp::socket socket_;    //client 的 TCP socket
    enum { max_length = 1024 };
    char data_[max_length];     // 接收緩衝區
};

// Server 類別：負責監聽、接受連線、統計與關閉流程
class Server {
public:
    Server(boost::asio::io_context& io_context, tcp::endpoint endpoint)
        : io_context_(io_context),
        acceptor_(io_context, endpoint),
        stats_timer_(io_context),
        signals_(io_context, SIGINT, SIGTERM) {
        start_accept();      // 啟動非同步接受連線
        start_stats();       // 啟動統計計時器
        start_signal_wait(); // 啟動訊號監聽
    }

private:
    void start_accept() {
        //非同步接受新連線，每次建立新 socket，避免重複使用
        auto new_socket = std::make_shared<tcp::socket>(io_context_);
        acceptor_.async_accept(*new_socket,
            [this, new_socket](boost::system::error_code ec) {
                if (!ec) {
                    if (active_connections.load() >= MAX_CONNECTIONS) {
                        // 超過最大連線數 ，主動關閉 socket
                        boost::system::error_code close_ec;
                        new_socket->close(close_ec); // 拒絕超過上限的連線
                        rejected_connections++;
                    }
                    else {
                        // 建立 Session 處理該連線
                        std::make_shared<Session>(std::move(*new_socket))->start();
                    }
                }
                start_accept(); // 無條件遞迴呼叫，確保持續接受連線
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

                std::cout << "[Server] Active connections: " << active_connections
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
};

int main() {
    try {
        boost::asio::io_context io;
        tcp::endpoint endpoint(tcp::v4(), 12345);
        Server server(io, endpoint);

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
