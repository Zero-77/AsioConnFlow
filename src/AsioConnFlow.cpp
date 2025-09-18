#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <iostream>
#include <memory>
#include <vector>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <chrono> //記錄時間
#include <random>
#include <numeric>
#include <csignal>
#include <thread>



using boost::asio::ip::tcp;

enum class ClientMode {
    Normal,
    EarlyClose,
    Idle,
    RST,
    NoTLS
};
ClientMode mode_;


struct RetryPolicy {
    int max_attempts = 5;
    int base_delay_ms = 500;

    bool should_retry(const boost::system::error_code& ec) const {
        std::string msg = ec.message();
        return !(msg.find("certificate verify failed") != std::string::npos ||
            msg.find("unknown ca") != std::string::npos ||
            msg.find("expired") != std::string::npos);
    }

    int delay_for(int attempt, std::mt19937& rng) const {
        int base = base_delay_ms * (1 << attempt); // exponential backoff
        int jitter = rand() % 200;                 // random jitter
        return base + jitter;
    }
};

std::atomic<int> qps_counter{ 0 };
std::atomic<bool> stop_qps_monitor{ false };
std::atomic<bool> stop_requested{ false };


void record_qps(std::atomic<bool>& stop_flag) {
    std::thread([&]() {
        while (!stop_flag.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (stop_qps_monitor.load()) break;
            int qps = qps_counter.exchange(0);
            std::cout << "[Monitor] QPS: " << qps << "\n";
        }
        }).detach();
}


bool behavior_test = false; //模擬連線行為開關


// 模擬一個 TCP client，負責連線、送出 ID、持續發送訊息
class SimulatedClient : public std::enable_shared_from_this<SimulatedClient> {
public:
    // 建構子：初始化 socket、計時器、client 編號與目標端點，加入TLS
    /*
    SimulatedClient(boost::asio::io_context& io, int id, tcp::endpoint endpoint, boost::asio::ssl::context& ssl_ctx, ClientMode mode)
        : ssl_stream_(io, ssl_ctx), plain_socket_(io), timer_(io),
        id_(id), endpoint_(endpoint), io_(io), mode_(mode)
    {

    }
    */
    SimulatedClient(boost::asio::io_context& io,
        int id,
        tcp::endpoint endpoint,
        boost::asio::ssl::context& ssl_ctx,
        ClientMode mode,
        std::shared_ptr<std::vector<int>> shared_latencies = nullptr)
        : ssl_stream_(io, ssl_ctx),
        plain_socket_(io),
        timer_(io),
        id_(id),
        endpoint_(std::move(endpoint)),
        io_(io),
        ssl_context_(ssl_ctx),
        mode_(mode),
        shared_latencies_(std::move(shared_latencies)),
        rng_(std::random_device{}())
    { }


    // 啟動 client：非同步連線至 server
    void start() {

        //主機名驗證(SNI + CN / SAN)
        SSL* ssl = ssl_stream_.native_handle();
        SSL_set_tlsext_host_name(ssl, "localhost"); //SNI
        SSL_set1_host(ssl, "localhost");    //主機名驗證(CN/SAN)


        //ssl_stream_連線與handshake握手流程
        auto self = shared_from_this();

        /*連線模擬一個假錯誤（例如 connect failed），進入retry連線流程
        *************************
        // 模擬一個假錯誤（例如 connect failed）
        boost::system::error_code fake_error = boost::asio::error::connection_refused;

        // 判斷是否應該重試
        if (retry_policy_.should_retry(fake_error)) {
            std::cout << "[Client #" << id_ << "] Simulating retry due to: " << fake_error.message() << "\n";
            schedule_retry(1); // 手動觸發 retry
        }
        else {
            std::cerr << "[Client #" << id_ << "] Simulated fatal error: " << fake_error.message() << "\n";
            graceful_close();
        }
        *************************
         */

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
                      //*  std::cout << "[Client] TLS handshake success — server verified\n";
                        self->handshake_ok_ = true;
                        self->handle_connect(ec);
                    }
                    else {
                        std::cerr << "[Client] TLS handshake failed: " << ec.message() << "\n";
                        self->graceful_close(); // 握手失敗仍需關閉 socket
                        return;
                    }
                });
            }
            else {
                std::cerr << "Connect failed: " << ec.message() << "\n";
                return;
            }

        });
    }

    void start_with_retry(int attempt = 0) {
        if (attempt > retry_policy_.max_attempts) {
            std::cerr << "[Client] Max retry reached. Giving up.\n";
            return;
        }

        auto self = shared_from_this();
        ssl_stream_.lowest_layer().async_connect(endpoint_, [self = shared_from_this(), attempt](boost::system::error_code ec) {
            if (!ec) {
                self->ssl_stream_.async_handshake(boost::asio::ssl::stream_base::client, [self, attempt](boost::system::error_code ec) {
                    if (!ec) {
                        //* std::cout << "[Client] TLS handshake success\n";
                        self->handle_connect(ec);
                    }
                    else {
                        std::cerr << "[Client] TLS handshake failed: " << ec.message() << "\n";

                        // 憑證錯誤不重試
                        if (!self->retry_policy_.should_retry(ec)) {
                            std::cerr << "[Client] Fatal TLS error, not retrying.\n";
                            return;
                        }

                        self->graceful_close();
                        self->schedule_retry(attempt + 1);
                    }
                    });
            }
            else {
                std::cerr << "[Client] Connect failed: " << ec.message() << "\n";
                self->schedule_retry(attempt + 1);
            }
        });
    }

    void schedule_retry(int next_attempt) {
        const int delay = retry_policy_.delay_for(next_attempt, rng_);
        std::cout << "[Client] Scheduling retry #" << next_attempt << " in " << delay << "ms\n";

        timer_.expires_after(std::chrono::milliseconds(delay));
        auto self = shared_from_this();
        timer_.async_wait([self, next_attempt](boost::system::error_code ec) {
            if (ec) return;
            self->reset_connection();
            self->start_with_retry(next_attempt);
            });
    }

    void retry_later(int next_attempt) {
        auto self = shared_from_this();
        timer_.expires_after(std::chrono::milliseconds(500 * next_attempt));
        timer_.async_wait([self, next_attempt](boost::system::error_code) {
            self->reset_connection();
            self->start_with_retry(next_attempt);
        });
    }

    void reset_connection() {
        boost::system::error_code ec;

        boost::asio::io_context io;
        //TLS 1.3 boost
        //建立TLS context
        boost::asio::ssl::context ssl_ctx(boost::asio::ssl::context::tlsv13_client);

        // 關閉舊 socket（若已建立）
        ssl_stream_.lowest_layer().shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
        ssl_stream_.lowest_layer().close(ec);

        // 重新建立 TLS stream（避免 socket 泄漏）
        //ssl_stream_ = boost::asio::ssl::stream<boost::asio::ip::tcp::socket>(io, ssl_ctx);
        std::unique_ptr<boost::asio::ssl::stream<tcp::socket>> ssl_stream_;// 使用智能指標 來賦值
        ssl_stream_ = std::make_unique<boost::asio::ssl::stream<tcp::socket>>(io, ssl_ctx);


        // 設定主機名驗證（SNI + CN/SAN）
        SSL* ssl = ssl_stream_->native_handle();
        SSL_set_tlsext_host_name(ssl, "localhost");
        SSL_set1_host(ssl, "localhost");
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
        auto self = shared_from_this();
        if (!ec) {
            ssl_stream_.async_read_some(boost::asio::buffer(reply_buffer),
                [self](boost::system::error_code ec, std::size_t length) {
                    if (!ec) {
                        // 印出 server 回覆的 ID 驗證結果
                        std::string reply(self->reply_buffer.data(), length);
                       //* std::cout << "Client " << self->id_ << " received ID reply: " << reply << std::endl;

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
        timer_.expires_after(std::chrono::milliseconds(1000)); // 控制 QPS
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
                                if (self->shared_latencies_) {
                                    self->shared_latencies_->push_back(static_cast<int>(duration_us));
                                }

                                qps_counter++;

                                // 每 500 筆輸出一次平均 latency
                                if (self->latency_samples_.size() >= 500) {
                                    int total = 0;
                                    for (int v : self->latency_samples_) total += v;
                                    int avg = total / static_cast<int>(self->latency_samples_.size());
                                    //* std::cout << "Client " << self->id_ << " avg latency: " << avg << "us over " << self->latency_samples_.size() << " samples\n";
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
    boost::asio::ssl::context& ssl_context_;

    bool handshake_ok_ = false;       // 握手是否成功
    bool armed_tcp_ = false;          // 是否已加 active_tcp_connections
    bool armed_tls_ = false;          // 是否已加 active_tls_connections

    std::atomic<bool> closing_{ false };                     // 防止重入關閉
    ClientMode mode_;
    tcp::socket plain_socket_;  // 用於 NoTLS 模式
    RetryPolicy retry_policy_;
    std::mt19937 rng_;              // jitter 用亂數
    std::string sni_host_;
    std::shared_ptr<std::vector<int>> shared_latencies_;
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
            if(behavior_test == true) {
                if (launched_ % 10 == 0) mode = ClientMode::EarlyClose;
                else if (launched_ % 15 == 0) mode = ClientMode::Idle;
                else if (launched_ % 20 == 0) mode = ClientMode::RST;
                else if (launched_ % 25 == 0) mode = ClientMode::NoTLS;
            }
            //std::make_shared<SimulatedClient>(io_, launched_, endpoint_, ssl_ctx_, mode)->start();
            std::make_shared<SimulatedClient>(io_, launched_, endpoint_, ssl_ctx_, mode)->start_with_retry();
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
    bool test_error_ssl = false; //開/關 錯誤的憑證連線，開true /關false
    
    if (test_error_ssl) {
        //取用錯誤的憑證進行連線
        struct TestCase {
            std::string name;
            std::string cert_path;
            std::string key_path;
        };

        std::vector<TestCase> test_cases = {
            { "Expired", "../../../client-certs/public/test_error/expired.crt", "../../../client-certs/public/test_error/expired.key" },
            { "NotYetValid", "../../../client-certs/public/test_error/notyet.crt", "../../../client-certs/public/test_error/notyet.key" },
            { "SelfSigned", "../../../client-certs/public/test_error/selfsigned.crt", "../../../client-certs/public/test_error/selfsigned.key" },
            { "UnknownCA", "../../../client-certs/public/test_error/badclient.crt", "../../../client-certs/public/test_error/badclient.key" },
            { "wronghost", "../../../client-certs/public/test_error/wronghost.crt", "../../../client-certs/public/test_error/wronghost.key" }
        };

        for (const auto& test : test_cases) {
            std::cout << "Testing: " << test.name << "\n";

            boost::asio::io_context io;
            boost::asio::ssl::context ssl_ctx(boost::asio::ssl::context::tlsv13_client);

            try {
                ssl_ctx.use_certificate_chain_file(test.cert_path);
                ssl_ctx.use_private_key_file(test.key_path, boost::asio::ssl::context::pem);
                ssl_ctx.load_verify_file("../../../CA/ca.pem");
                ssl_ctx.set_verify_mode(boost::asio::ssl::verify_peer);

                boost::asio::ssl::stream<boost::asio::ip::tcp::socket> ssl_stream(io, ssl_ctx);
                boost::asio::ip::tcp::resolver resolver(io);
                boost::system::error_code ec;
                auto endpoints = resolver.resolve("127.0.0.1", "12345");

                boost::asio::connect(ssl_stream.lowest_layer(), endpoints);
                ssl_stream.handshake(boost::asio::ssl::stream_base::client, ec);

                if (ec) {
                    std::cerr << "TLS handshake failed: " << ec.message() << "\n";
                }
                else {
                    std::cout << "TLS handshake succeeded---Note: handshake success does not imply certificate trust\n";
                }
            }
            catch (const std::exception& ex) {
                std::cout << "Handshake failed for " << test.name << ": " << ex.what() << "\n";
            }

            std::cout << "----------------------------------\n";
        }
    
    }
    else {
        std::srand(static_cast<unsigned int>(std::time(nullptr))); // 初始化隨機種子

        boost::asio::io_context io;


        // 加入 signal_set 來攔截 Ctrl+C
        boost::asio::signal_set signals(io, SIGINT);
        signals.async_wait([&](boost::system::error_code /*ec*/, int /*signo*/) {
            std::cout << "\n[Client] Ctrl+C detected. Shutting down gracefully...\n";
            stop_requested.store(true);
            stop_qps_monitor.store(true);
            io.stop(); // 這是關鍵，讓 io.run() 結束
            });


        tcp::endpoint endpoint(boost::asio::ip::make_address("127.0.0.1"), 12345); // server 端點

        //TLS 1.3 boost
        //建立TLS context
        boost::asio::ssl::context ssl_ctx(boost::asio::ssl::context::tlsv13_client);
        //ssl_ctx.set_verify_mode(boost::asio::ssl::verify_none); //測試用，不驗證對方，直接接受任何憑證

        //1. 驗證 Server憑證(由CA簽發)
        ssl_ctx.set_verify_mode(boost::asio::ssl::verify_peer);
        ssl_ctx.load_verify_file("../../../CA/ca.pem"); // CA憑證: 用來驗證 server.crt

        //2. 提供Client 的憑證與私鑰(由同一CA簽發)
        ssl_ctx.use_certificate_chain_file("../../../client-certs/public/client.crt");
        ssl_ctx.use_private_key_file("../../../client-certs/private/client.key", boost::asio::ssl::context::pem);

        //計算
        auto all_latencies = std::make_shared<std::vector<int>>();

        // 模擬參數：可依壓測目標調整
        /*
        int total_clients = 5000;   // 總 client 數
        int batch_size = 500;       // 每批啟動數量
        int interval_ms = 100;      // 每批間隔時間（毫秒）
        */

        int total_clients = 300;   // 總 client 數
        int batch_size = 100;       // 每批啟動數量
        int interval_ms = 10000;      // 每批間隔時間（毫秒）

        // 啟動批次啟動器
        //std::make_shared<BatchLauncher>(io, endpoint, ssl_ctx, total_clients, batch_size, interval_ms)->start();

        //發送 瞬間併發連線
        std::vector<std::thread> threads;
        for (int i = 0; i < 10000; i++) {
            threads.emplace_back([&, i]() {
                auto client = std::make_shared<SimulatedClient>(io, i, endpoint, ssl_ctx, ClientMode::Normal, all_latencies);
                client->start_with_retry();
                });
        }
        for (auto& t : threads) t.join();

        //io.run(); // 啟動事件迴圈



        try {

            record_qps(stop_qps_monitor); // 啟動即時 QPS 顯示

            io.run();  // 啟動事件迴圈

            // 分析 latency 分佈
            std::sort(all_latencies->begin(), all_latencies->end());

            int total = all_latencies->size();
            int p95 = all_latencies->at(total * 95 / 100);
            int p99 = all_latencies->at(total * 99 / 100);
            double avg = std::accumulate(all_latencies->begin(), all_latencies->end(), 0.0) / total;

            std::cout << "\n=== Performance Summary ===\n";
            std::cout << "Total Requests: " << total << "\n";
            std::cout << "Average Latency: " << avg / 1000 << " ms\n";
            std::cout << "P95 Latency: " << p95 / 1000 << " ms\n";
            std::cout << "P99 Latency: " << p99 / 1000 << " ms\n";
        }
        catch (const std::exception& e) {
            std::cerr << "[Client] Exception caught: " << e.what() << "\n";
        }

        if (stop_requested.load()) {
            std::cout << "[Client] Graceful shutdown complete. Press Enter to exit.\n";
            std::cin.get();
        }
    }

    return 0;
}
