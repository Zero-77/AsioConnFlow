#include <boost/asio.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/ssl.hpp>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>
#include <atomic>
#include <csignal>
#include <chrono>
#include <set>


using boost::asio::ip::tcp;

// 提高最大連線數以符合壓測目標
constexpr int MAX_CONNECTIONS = 11000;

// 全域統計變數：追蹤總連線數、處理訊息數、拒絕連線數、TCP連線數、TLS連線數、thread處理QPS監控
std::atomic<int> total_connections{ 0 };
std::atomic<int> messages_processed{ 0 };
std::atomic<int> rejected_connections{ 0 };
std::atomic<int> active_tcp_connections{ 0 };
std::atomic<int> active_tls_connections{ 0 };
std::atomic<bool> stop_stats_thread{ false };

// 效能統計和互斥鎖
std::vector<int> latency_samples;    // 儲存每筆處理耗時（微秒）
std::mutex latency_mutex;   // 保護 latency_samples 的存取

class Server;

// Session 類別：代表一個 client 連線，負責處理讀寫
class Session : public std::enable_shared_from_this<Session> {
public:
    // Session 類別的建構子:負責初始化 SSL 流物件 ssl_stream_，並綁定底層的 io_context 和 SSL context
    // io_context: 負責管理所有非同步 I/O 操作的事件循環
    // ssl_ctx: 用於設定 SSL/TLS 參數（憑證、私鑰、加密套件等）
    Session(boost::asio::io_context& io_context, boost::asio::ssl::context& ssl_ctx, Server& server) : 
        ssl_stream_(io_context, ssl_ctx),       // 初始化 TLS 封裝的 socket，使用 io_context 與 SSL context
        server_ref_(server),                    // 儲存 Server 參照，用於回報連線狀態與移除 Session
        read_deadline_(io_context)              // 初始化讀取超時計時器，用於 idle timeout 控制
    {  

    }
    
    // 取得底層的 ssl_stream_ 物件的引用，以便外部存取，如讀寫資料或關閉連線
    boost::asio::ssl::stream<tcp::socket>& stream() {
        return ssl_stream_;
    }

    // Session結束時，自動更新 TCP/TLS 連線的統計數量，確保系統目前有的活躍連線
    ~Session() {
        // armed_tcp_ bool表示這個 Session 是否曾使用過 TCP
        //if (armed_tcp_) active_tcp_connections--;

        // armed_tls_ bool表示這個 Session 是否曾使用過 TCP
        //if (armed_tls_) active_tls_connections--;
    }

    void start() {

        armed_tcp_ = true; //TCP 連線建立成功
        active_tcp_connections++;  // 新連線建立時，TCP活躍連線數加一

        auto self = shared_from_this();
        ssl_stream_.async_handshake(boost::asio::ssl::stream_base::server,
            [self](const boost::system::error_code& ec) {
                if (!ec) {
                    // 確認伺服器連線是否超過上限，超過即拒絕連線
                    if (active_tls_connections.load() >= MAX_CONNECTIONS) {
                        boost::system::error_code close_ec;

                        //拒絕連線處理
                        //self->stream().lowest_layer().close(close_ec);  // 執行關閉 socket 的動作            
                        std::cout << "[Server] TLS handshake rejected (limit reached)\n";
                        rejected_connections++;    // 拒絕連線數計數
                        self->terminate(); // 關閉與釋放
                        return; // 直接返回，未建立armed_tls_維持false、不計數active_tls_connections
                    }

                    // 接受成功連線TLS， 設定已連線armed_tls_、計數active_tls_connections
                    self->handshake_ok_ = true;
                    self->armed_tls_ = true;
                    active_tls_connections++;

                    // 進入讀寫循環
                    self->do_read();
                }
                else {
                    // TLS握手、憑證驗證  失敗處理
                    std::cerr << "[Server] TLS handshake failed: " << ec.message() << "\n";
                    rejected_connections++; // 拒絕連線數計數

                    //拒絕連線處理，確保 socket 被關閉， client 收到錯誤訊號
                    //boost::system::error_code close_ec;
                    //self->stream().lowest_layer().close(close_ec);  // 執行關閉 socket 的動作 
                    //self->graceful_close();

                    self->terminate();  // 關閉與釋放
                }
            });
    }

    void graceful_close() {
        // if (closed_.exchange(true)) return; // 防重入Reentrancy Guard

        boost::system::error_code ec;
        if (handshake_ok_) {
            ssl_stream_.shutdown(ec); // 送出 TLS 協定 close_notify
            if (ec) {
                std::cerr << "[Session] TLS shutdown failed: " << ec.message() << "\n"; //若client端自行終止程式，錯誤訊息 : 連線已被您主機上的軟體中止。
            }
        }
        //針對底層的 TCP 連線進行關閉
        ssl_stream_.lowest_layer().shutdown(tcp::socket::shutdown_both, ec); //shutdown_both 雙向傳輸，同時關閉 讀取和寫入
        ssl_stream_.lowest_layer().close(ec);
    }


private:
    // 非同步讀取 client 資料
    void do_read() {
        auto self = shared_from_this();

        //Server設置idle timeout 機制，加入 讀取超時
        read_deadline_.expires_after(std::chrono::seconds(30));  // 等待如  10 秒。測試瞬間10000筆連線，需等待handshake全部完成(25 秒)
        read_deadline_.async_wait([self](const boost::system::error_code& ec) {
            if (!ec) {
                std::cerr << "[Session] Read timeout, terminating\n";
                self->terminate();  // 清理 zombie session
            }
            });

        ssl_stream_.async_read_some(boost::asio::buffer(data_),
            [self](boost::system::error_code ec, std::size_t length) {
                self->read_deadline_.cancel();  // Client有傳資料就取消 timeout
                if (!ec) {
                    messages_processed++; // 成功處理一筆訊息計數

                    auto start = std::chrono::steady_clock::now();  //記錄開始時間

                    std::string response = "Echo: " + std::string(self->data_, length); //self->data_ 是Session的buffer(data_)中取得從 socket 收到的資料

                    // 非同步回寫 Echo 回應
                    boost::asio::async_write(self->ssl_stream_, boost::asio::buffer(response),
                        [self, start](boost::system::error_code ec2, std::size_t) {
                            if (!ec2) {
                                // 計算處理耗時（微秒）
                                auto end = std::chrono::steady_clock::now();
                                auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

                                // 儲存 latency 統計，互斥鎖std::lock_guard 的作用域
                                {
                                    std::lock_guard<std::mutex> lock(latency_mutex);
                                    latency_samples.push_back(static_cast<int>(duration_us));
                                }
                                self->do_read(); // 形成讀=>寫=>讀循環，回寫成功後繼續下一輪讀取
                            }
                            else { //若寫入失敗後釋放資源
                                //self->graceful_close(); //確保釋放
                                //std::cerr << "[Session] Write error: " << ec2.message() << "\n";
                                self->terminate();  // 呼叫greceful_close() 和 計數遞減
                                return;
                            }
                        });
                }
                else {  //若讀取錯誤後，回應錯誤並關閉連線
                    //self->graceful_close(); //確保釋放
                    //std::cerr << "[Session] Read error: " << ec.message() << "\n";
                    self->terminate();  // 呼叫greceful_close() 和 計數遞減
                    return;
                }
            });
    }

    enum { max_length = 1024 }; // 最大接收緩衝區大小
    char data_[max_length];     // 用於接收資料的緩衝區（最大 1024)
    boost::asio::ssl::stream<tcp::socket> ssl_stream_; //TLS 握手後，通訊過程中使用的加密

    bool handshake_ok_ = false;       // 標識 SSL/TLS 握手是否成功
    bool armed_tcp_ = false;          // 標記 TCP 連線是否已加入連線計數
    bool armed_tls_ = false;          // 標記 TLS 連線是否已加入連線計數
    std::atomic<bool> closed_{ false }; //graceful_close 防止多次關閉

    Server& server_ref_;
    void terminate();
    boost::asio::steady_timer read_deadline_; // 為讀取超時，新增steady_timer類別read_deadline_
};

// Server 類別：負責監聽、接受連線、統計與關閉流程
class Server {
public:
    //Server 類別建構子:初始化 I/O 物件、接收器、SSL context、定時器與訊號監聽
    /*
     初始化:
        io_context，管理非同步 I/O 事件循環
        TCP 接收器，用於監聽客戶端連線
        SSL context，設定 TLS 參數
        定時器，用來定期執行統計任務
        訊號監聽，捕捉系統中斷訊號（Ctrl+C 等）
     */
    Server(boost::asio::io_context& io_context, tcp::endpoint endpoint, boost::asio::ssl::context& ssl_ctx)
        : io_context_(io_context),
        acceptor_(io_context),
        ssl_ctx_(ssl_ctx),
        stats_timer_(io_context),
        signals_(io_context, SIGINT, SIGTERM) {

        // 手動 open/bind/listen
        boost::system::error_code ec; // error_code 物件，用來接收錯誤狀態

        acceptor_.open(endpoint.protocol(), ec); // 開啟 acceptor，指定使用的通訊協定（從 endpoint 取得，例如 TCP v4)
        if (ec)
            // 若開啟失敗，拋出例外
            throw std::runtime_error("acceptor open failed: " + ec.message());

        // 設定 socket 選項：允許地址重用，可讓伺服器重新啟動時，即使前一個 socket 還在 TIME_WAIT 狀態，也能綁定同一個 port
        acceptor_.set_option(boost::asio::socket_base::reuse_address(true), ec);

        if (ec)
            // 只是顯示警告（設定失敗不會中止程式）
            std::cerr << "[Server] set_option(reuse_address) failed: " << ec.message() << "\n";

        acceptor_.bind(endpoint, ec); // 綁定 socket 到指定的 endpoint（IP + Port）
        if (ec)
            // 綁定失敗會拋出例外
            throw std::runtime_error("acceptor bind failed: " + ec.message());

        acceptor_.listen(boost::asio::socket_base::max_listen_connections, ec); // 設定監聽佇列的大小，準備開始接受連線
        if (ec)
            // listen 若失敗將會導致 accept 失敗，顯示錯誤訊息
            std::cerr << "[Server] listen failed: " << ec.message() << "\n";


        start_accept();      // 啟動非同步接受連線
        //start_stats();       // 啟動統計計時器
        start_stats_thread();
        start_signal_wait(); // 啟動訊號監聽:當接收到中斷訊號時進行清理或關閉伺服器
    }

    void remove_session(std::shared_ptr<Session> session) {
        std::lock_guard<std::mutex> lock(session_mutex_);
        active_sessions_.erase(session);
    }
    
private:
    void start_accept() {
        //非同步接受新連線，每次建立新 Socket，避免重複使用
        auto session = std::make_shared<Session>(io_context_, ssl_ctx_, *this);
        auto& socket = session->stream().lowest_layer(); // 取得底層 TCP Socket
        {
            std::lock_guard<std::mutex> lock(session_mutex_);
            active_sessions_.insert(session);   // 建立 Session 並累積
        }

        acceptor_.async_accept(socket,
            [this, session](boost::system::error_code ec) {
                if (!ec) {
                    total_connections++; //統計所有連線嘗試(不論是否成功握手)
                    session->start();// 建立 Session 處理該連線，啟動 TLS session

                }
                else {
                    std::cerr << "[Server] Accept failed: " << ec.message() << "\n";
                }
                start_accept(); // 無條件遞迴呼叫，持續接受下一條連線
            });
    }

    /*
    // 每秒輸出統計資訊（活躍連線、QPS、拒絕數）
    void start_stats() {

        stats_timer_.expires_after(std::chrono::seconds(1)); // 設定定時器每1秒執行
        stats_timer_.async_wait([this](boost::system::error_code ec) {  // 非同步等待定時器過期事件
            if (!ec) {  // 定時器正常觸發
                int avg_latency = 0;
                int p95_latency = 0;
                int p99_latency = 0;
                {
                    // 鎖住 latency_samples
                    std::lock_guard<std::mutex> lock(latency_mutex);
                    if (!latency_samples.empty()) { // 如果有收集到延遲樣本

                        std::sort(latency_samples.begin(), latency_samples.end());  // 將延遲資料排序，方便計算百分位數

                        int total = static_cast<int>(latency_samples.size());   // 樣本數
                        int sum = 0;

                        for (int v : latency_samples) sum += v; // 計算總和，用於求平均延遲
                        avg_latency = sum / total;  // 平均延遲 (微秒)

                        p95_latency = latency_samples[total * 95 / 100]; // 95th 百分位延遲 (索引為 total * 95%)
                        p99_latency = latency_samples[total * 99 / 100]; // 99th 百分位延遲 (索引為 total * 99%)
                        latency_samples.clear(); // 每秒清空統計的樣本，避免記憶體持續累積
                    }
                }

                std::cout << "[Server] Active TLS: " << active_tls_connections
                    << " | TCP: " << active_tcp_connections
                    << " | Total: " << total_connections
                    << " | QPS: " << messages_processed
                    << " | Rejected: " << rejected_connections
                    << " | Avg: " << avg_latency << "us"
                    << " | P95: " << p95_latency << "us"
                    << " | P99: " << p99_latency << "us\n";


                messages_processed = 0; // 每秒重設 QPS 統計
                start_stats();          // 再次排程下一次統計
            }
            });
    }
    */

    //改用獨立的thread處理QPS統計，或是其他監控


    void start_stats_thread() {
        std::thread([this]() {
            while (!stop_stats_thread.load()) {
               

                std::this_thread::sleep_for(std::chrono::seconds(1));

                int avg_latency = 0;
                int p95_latency = 0;
                int p99_latency = 0;

                {
                    std::lock_guard<std::mutex> lock(latency_mutex);
                    if (!latency_samples.empty()) {
                        std::sort(latency_samples.begin(), latency_samples.end());
                        int total = static_cast<int>(latency_samples.size());
                        int sum = 0;
                        for (int v : latency_samples) sum += v;
                        avg_latency = sum / total;
                        p95_latency = latency_samples[total * 95 / 100];
                        p99_latency = latency_samples[total * 99 / 100];
                        latency_samples.clear();
                    }
                }

                if (stop_stats_thread.load()) break;
                std::cout << "[Server] Active TLS: " << active_tls_connections
                    << " | TCP: " << active_tcp_connections
                    << " | Total: " << total_connections
                    << " | QPS: " << messages_processed
                    << " | Rejected: " << rejected_connections
                    << " | Avg: " << avg_latency << "us"
                    << " | P95: " << p95_latency << "us"
                    << " | P99: " << p99_latency << "us\n";

                messages_processed = 0;
            }
            }).detach(); 
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

                stop_stats_thread.store(true); // 停止thread用於QPS統計

            }
            });
    }


    tcp::acceptor acceptor_;    //TCP 監聽器，監聽指定的端點(IP + port)
    boost::asio::io_context& io_context_;   //管理所有非同步操作
    boost::asio::steady_timer stats_timer_; //定時器，週期性觸發每秒輸出統計資訊
    boost::asio::signal_set signals_;   //監聽系統訊號（例如 SIGINT、SIGTERM）: 關閉伺服器或做清理工作
    boost::asio::ssl::context& ssl_ctx_;     //建立安全的 SSL/TLS 連線，以是引用方式傳入:SSL/TLS 的上下文物件，儲存憑證、私鑰、加密演算法設定
    
    std::set<std::shared_ptr<Session>> active_sessions_;  // 追蹤活躍 Session
    std::mutex session_mutex_;                            // 保護 active_sessions_
};

//Session::terminate有使用Server類別，其實作位置，放在 Server 類別定義之後
void Session::terminate() {
    if (closed_.exchange(true)) return;

    graceful_close();

    if (armed_tls_) {
        active_tls_connections--;
    }
    if (armed_tcp_) {
        active_tcp_connections--;
    }
    server_ref_.remove_session(shared_from_this());  // Server 類型已定義，釋放 Session shared_ptr
}

int main() {
    try {

        boost::asio::io_context io;

        //TLS 1.3 boost，建立TLS context
        boost::asio::ssl::context ssl_ctx(boost::asio::ssl::context::tlsv13_server);

        //1. 載入Server的憑證與私鑰
        ssl_ctx.use_certificate_chain_file("../../../server-certs/public/server.crt");
        ssl_ctx.use_private_key_file("../../../server-certs/private/server.key", boost::asio::ssl::context::pem);

        //2. 設定驗證模式:要求client提供憑證，並強制驗證
        ssl_ctx.set_verify_mode(boost::asio::ssl::verify_peer | boost::asio::ssl::verify_fail_if_no_peer_cert);
        
        // 3. 載入 CA 憑證：用來驗證 client 的憑證是否由信任的 CA 簽發
        ssl_ctx.load_verify_file("../../../CA/ca.pem");

        //4. 自訂驗證 callback
        ssl_ctx.set_verify_callback([](bool preverified, boost::asio::ssl::verify_context& ctx) {
            X509_STORE_CTX* store = ctx.native_handle();
            int err = X509_STORE_CTX_get_error(store);
            const char* msg = X509_verify_cert_error_string(err);

            if (!preverified) {
                std::cerr << "[TLS Verify] Failed: " << msg << "\n";
                return false;  // 主動拒絕握手，傳送 fatal alert

                switch (err) {
                    case X509_V_ERR_CERT_HAS_EXPIRED:   //憑證已過期
                        std::cerr << "Reason: expired cert\n"; 
                        break;
                    case X509_V_ERR_CERT_NOT_YET_VALID: //憑證尚未生效
                        std::cerr << "Reason: not yet valid\n"; 
                        break;  
                    case X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT:    //自簽憑證且不在信任清單
                        std::cerr << "Reason: self-signed cert\n"; 
                        break;   
                    case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY: //找不到簽發者憑證
                        std::cerr << "Reason: unknown CA\n"; 
                        break;
                    default:
                        std::cerr << "Reason: other TLS error\n"; 
                        break;
                }
            }
            return true;  // 驗證通過，繼續握手
         });

        tcp::endpoint endpoint(tcp::v4(), 12345);
        Server server(io, endpoint, ssl_ctx);


        // 建立 thread pool 處理 io_context 的事件，可固定 thread 數（例如 8 或 16）以利壓測一致性
        unsigned int thread_count = 16;
        /*
        //回傳系統可用的硬體執行緒數量（通常是 CPU 的核心數)
        unsigned int thread_count = std::thread::hardware_concurrency(); 
        */

        std::vector<std::thread> threads;   //存放執行緒物件
        for (unsigned int i = 0; i < thread_count; i++) {
            threads.emplace_back([&io]() { io.run(); }); // 建立一個新執行緒，執行 lambda 函式，lambda 中呼叫 io_context 的 run()
        }
        for (auto& t : threads) t.join(); // 等待所有執行緒結束（阻塞等待）
    }
    catch (std::exception& e) {
        std::cerr << "Server exception: " << e.what() << "\n";
    }

    int ret = system("pause");

    return 0;
}
