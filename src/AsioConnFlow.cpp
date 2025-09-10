#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <iostream>
#include <memory>
#include <vector>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <chrono> //記錄時間

using boost::asio::ip::tcp;

enum class ClientMode {
    Normal,
    EarlyClose,
    Idle,
    RST,
    NoTLS
};
ClientMode mode_;

BOOLEAN behavior_test = FALSE; //模擬連線行為開關

// 模擬一個 TCP client，負責連線、送出 ID、持續發送訊息
class SimulatedClient : public std::enable_shared_from_this<SimulatedClient> {
public:
    // 建構子：初始化 socket、計時器、client 編號與目標端點，加入TLS
    SimulatedClient(boost::asio::io_context& io, int id, tcp::endpoint endpoint, boost::asio::ssl::context& ssl_ctx, ClientMode mode)
        : ssl_stream_(io, ssl_ctx), plain_socket_(io), timer_(io),
        id_(id), endpoint_(endpoint), io_(io), mode_(mode) {
    }



    // 啟動 client：非同步連線至 server
    void start() {

        //ssl_stream_連線與handshake握手流程
        auto self = shared_from_this();

        if (mode_ == ClientMode::NoTLS) {
            // 使用明文 TCP socket 傳送資料給 TLS server
            plain_socket_.async_connect(endpoint_, [self](boost::system::error_code ec) {
                if (!ec) {
                    std::string msg = "PLAIN:" + std::to_string(self->id_) + "\n";
                    boost::asio::async_write(self->plain_socket_, boost::asio::buffer(msg),
                        [self](boost::system::error_code, std::size_t) {
                            self->plain_socket_.close(); // 傳完就關閉
                        });
                }
                });
            return;
        }

        ssl_stream_.lowest_layer().async_connect(endpoint_, [self](boost::system::error_code ec) {
            if (!ec) {
                self->ssl_stream_.async_handshake(boost::asio::ssl::stream_base::client, [self](boost::system::error_code ec) {
                    if (!ec) {
                        self->handshake_ok_ = true;
                        self->handle_connect(ec);
                    }
                    else {
                        std::cerr << "[Client] TLS handshake failed: " << ec.message() << "\n";
                        self->graceful_close(); // 握手失敗仍需關閉 socket
                    }
                });
            }
            else {
                std::cerr << "Connect failed: " << ec.message() << "\n";
                return;
            }

        });
    }

    void graceful_close() {
        // 防止重入
        if (closing_.exchange(true)) return;

        // 停止未來排程
        boost::system::error_code ignore_ec;
        timer_.cancel();

        auto self = shared_from_this();
        if (handshake_ok_) {
            //  TLS 關閉：先 async_shutdown()（送出 close_notify）
            ssl_stream_.async_shutdown([self](const boost::system::error_code& /*ec*/) {
                // 無論成功與否，都關閉 TCP 層
                boost::system::error_code ec2;
                self->ssl_stream_.lowest_layer().shutdown(tcp::socket::shutdown_both, ec2);
                self->ssl_stream_.lowest_layer().close(ec2);
                });
        }
        else {
            // 未完成握手，直接關閉 TCP
            boost::system::error_code ec2;
            ssl_stream_.lowest_layer().shutdown(tcp::socket::shutdown_both, ec2);
            ssl_stream_.lowest_layer().close(ec2);
        }
    }

private:
    // 連線成功後送出 ID，失敗則輸出錯誤訊息
    void handle_connect(const boost::system::error_code& ec) {
        if (!ec) {
            send_id(); // 成功連線並傳送 ID
        }
        else {
            std::cerr << "Client " << id_ << " failed to connect: " << ec.message() << "\n";
        }
    }

    // 傳送 ID 訊息給 server（格式：ID:<編號>\n）
    void send_id() {


        //非同步寫入需要持有 buffer 直到完成
        std::string id_msg = "ID:" + std::to_string(id_) + "\n";

        auto self = shared_from_this();
        boost::asio::async_write(ssl_stream_, boost::asio::buffer(id_msg),
            [self](const boost::system::error_code& ec, std::size_t length) {
                if (!ec) {
                    self->handle_id_sent(ec, length);
                }
                else {
                    std::cerr << "Client " << self->id_ << " failed to send ID: " << ec.message() << "\n";
                    self->graceful_close();
                }
            });
    }

    // 等待 server 回覆 ID，收到後啟動訊息循環
    void handle_id_sent(const boost::system::error_code& ec, std::size_t /*length*/) {
        auto self(shared_from_this());
        if (!ec) {
            ssl_stream_.async_read_some(boost::asio::buffer(reply_buffer),
                [self](boost::system::error_code ec, std::size_t length) {
                    if (!ec) {
                        // 印出 server 回覆的 ID 驗證結果
                        std::string reply(self->reply_buffer.data(), length);
                        std::cout << "Client " << self->id_ << " received ID reply: " << reply << std::endl;

                        if (self->mode_ == ClientMode::EarlyClose) {
                            self->ssl_stream_.lowest_layer().close(); //模擬提早關閉
                            return;
                        }


                        if (self->mode_ == ClientMode::Idle) {
                            return; //模擬idle client (不傳訊息)
                        }

                        self->schedule_message(); // 啟動訊息循環
                    }
                });
        }
        else {
            std::cerr << "Client " << id_ << " failed to send ID: " << ec.message() << "\n";
        }
    }

    // 排程訊息傳送（固定間隔 5ms）
    void schedule_message() {
        timer_.expires_after(std::chrono::milliseconds(5)); // 控制 QPS
        //可調整成1000(ms);每秒 1 次（即 1 msg/sec），5,000 個 clients 同時可達總共 5,000 QPS
        auto self = shared_from_this(); //  確保物件在 callback 存活
        timer_.async_wait([self](const boost::system::error_code& ec) {
            if (!ec) {
                self->send_message();
            }
            });
    }

    // 傳送訊息給 server（格式：Hello from client <id>\n）
    void send_message() {
        auto self = shared_from_this();
        std::string msg = "Hello from client " + std::to_string(id_) + "\n";

        send_time_ = std::chrono::steady_clock::now(); // 記錄送出時間

        boost::asio::async_write(ssl_stream_, boost::asio::buffer(msg),
            [self](boost::system::error_code ec, std::size_t /*length*/) {
                if (!ec) {
                    // 等待 server 回覆 Echo 結果
                    self->ssl_stream_.async_read_some(boost::asio::buffer(self->reply_buffer),
                        [self](boost::system::error_code ec, std::size_t length) {
                            if (!ec) {
                                if (self->mode_ == ClientMode::RST) {
                                    self->ssl_stream_.lowest_layer().close(); // 模擬 RST（強制關閉）
                                    return;
                                }


                                auto end_time = std::chrono::steady_clock::now(); // 記錄回應時間
                                auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(end_time - self->send_time_).count();
                                self->latency_samples_.push_back(static_cast<int>(duration_us)); // 儲存 latency

                                // 每 100 筆輸出一次平均 latency
                                if (self->latency_samples_.size() >= 100) {
                                    int total = 0;
                                    for (int v : self->latency_samples_) total += v;
                                    int avg = total / static_cast<int>(self->latency_samples_.size());
                                    std::cout << "Client " << self->id_ << " avg latency: " << avg << "us over " << self->latency_samples_.size() << " samples\n";
                                    self->latency_samples_.clear();
                                }

                                self->schedule_message(); // 繼續下一輪，排程下一次訊息形成持續循環
                            }
                            else {
                                std::cerr << "[Client] Write failed: " << ec.message() << "\n";
                                self->graceful_close();
                            }
                        });
                }
            });
    }

    int id_;                                                         // client 編號
    //tcp::socket socket_;                                // 與 server 的 TCP socket
    tcp::endpoint endpoint_;                        // server 的 IP 與 port
    boost::asio::steady_timer timer_;           // 控制訊息間隔的計時器
    boost::asio::io_context& io_;                   // 事件迴圈
    std::array<char, 1024> reply_buffer;     // 接收 server 回覆的緩衝區
    std::chrono::steady_clock::time_point send_time_; // 記錄每筆訊息的送出時間
    std::vector<int> latency_samples_;                // 儲存每筆回應的耗時（微秒）
    boost::asio::ssl::stream<tcp::socket> ssl_stream_;

    bool handshake_ok_ = false;       // 握手是否成功
    bool armed_tcp_ = false;          // 是否已加 active_tcp_connections
    bool armed_tls_ = false;          // 是否已加 active_tls_connections

    std::atomic<bool> closing_{ false };                     // 防止重入關閉
    ClientMode mode_;
    tcp::socket plain_socket_;  // 用於 NoTLS 模式

};

// 批次啟動器：用來分批啟動大量 client，避免瞬間爆量
class BatchLauncher : public std::enable_shared_from_this<BatchLauncher> {
public:
    BatchLauncher(boost::asio::io_context& io, tcp::endpoint endpoint, boost::asio::ssl::context& ssl_ctx,
        int total_clients, int batch_size, int interval_ms)
        : io_(io), endpoint_(endpoint), ssl_ctx_(ssl_ctx), total_clients_(total_clients),
        batch_size_(batch_size), interval_ms_(interval_ms),
        launched_(0), timer_(io) {
    }

    void start() {
        launch_batch(); // 啟動第一批 client
    }

private:
    // 啟動一批 client，並排程下一批
    void launch_batch() {
        
        //std::make_shared<SimulatedClient>(io_, launched_, endpoint_)->start();

        for (int i = 0; i < batch_size_ && launched_ < total_clients_; i++) {
            ClientMode mode = ClientMode::Normal;   //可以設定Client端連線模式Normal、EarlyClose、Idle、RST、NoTLS
            if(behavior_test == TRUE) {
                if (launched_ % 10 == 0) mode = ClientMode::EarlyClose;
                else if (launched_ % 15 == 0) mode = ClientMode::Idle;
                else if (launched_ % 20 == 0) mode = ClientMode::RST;
                else if (launched_ % 25 == 0) mode = ClientMode::NoTLS;
            }
            std::make_shared<SimulatedClient>(io_, launched_, endpoint_, ssl_ctx_, mode)->start();
            launched_++;
        }

        // 若尚有 client 未啟動，排程下一批
        if (launched_ < total_clients_) {
            timer_.expires_after(std::chrono::milliseconds(interval_ms_));
            timer_.async_wait(std::bind(&BatchLauncher::handle_timer, shared_from_this(),
                std::placeholders::_1));
        }
    }

    // 計時器觸發，啟動下一批 client
    void handle_timer(const boost::system::error_code& ec) {
        if (!ec) {
            launch_batch();
        }
    }


    int total_clients_;     // 總共要啟動的 client 數量
    int batch_size_;        // 每批啟動的 client 數量
    int interval_ms_;       // 每批間隔時間（毫秒）
    int launched_;          // 已啟動的 client 數量
    tcp::endpoint endpoint_;
    boost::asio::io_context& io_;
    boost::asio::steady_timer timer_; // 控制批次間隔的計時器
    boost::asio::ssl::context& ssl_ctx_;
};

int main() {
    std::srand(static_cast<unsigned int>(std::time(nullptr))); // 初始化隨機種子

    boost::asio::io_context io;
    tcp::endpoint endpoint(boost::asio::ip::make_address("127.0.0.1"), 12345); // server 端點

    //TLS 1.3 boost
    //建立TLS context
    boost::asio::ssl::context ssl_ctx(boost::asio::ssl::context::tlsv13_client);
    ssl_ctx.set_verify_mode(boost::asio::ssl::verify_none); //測試用，不驗證對方，直接接受任何憑證

    // 模擬參數：可依壓測目標調整
    int total_clients = 5000;   // 總 client 數
    int batch_size = 500;       // 每批啟動數量
    int interval_ms = 100;      // 每批間隔時間（毫秒）

    // 啟動批次啟動器
    std::make_shared<BatchLauncher>(io, endpoint, ssl_ctx, total_clients, batch_size, interval_ms)->start();
    io.run(); // 啟動事件迴圈

    return 0;
}
