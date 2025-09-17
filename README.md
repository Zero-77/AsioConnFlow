TCP 非阻塞伺服器
---
專案配置:
   - Client端: src/AsioConnFlow.cpp
   - Server端: server/AsioServer.cpp


目前功能:

  - 10,000 並發連線穩定 (所有 client 成功連線並進入循環)

  - 10,000 QPS 持續輸出，95%/99% (server 每秒處理 10,000+ 筆訊息)

  - Server設置idle timeout 機制，加入 讀取超時 (設定3秒)

  - Client端(AsioConnFlow.cpp)，錯誤憑證模擬功能（test_error_ssl）
  此功能可用於模擬 TLS 憑證錯誤場景，協助驗證 server 端的錯誤處理邏輯與資源釋放流程。透過開關 test_error_ssl=true，系統將自動注入以下類型的錯誤憑證進行測試：
1. 過期憑證（expired）
2. 尚未生效憑證（not-yet-valid）
3. 自簽憑證（self-signed）
4. 未知 CA（unknown CA）
5. 憑證主機名稱與實際連線目標不符(wrong-host)


計算公式:
  - latency = 收到 Echo 回覆 的時間 - 送出訊息 的時間
  - Avg = 所有成功 request 的處理時間總和 / request 數
  - QPS = 成功處理的 request 數 / 秒數
  - active_tcp_connections = 成功建立 TCP socket 的連線數
  - active_tls_connections = 成功完成 TLS handshake 的連線數
  - rejected_connections = TLS handshake 失敗 + 超過 MAX_CONNECTIONS 而拒絕
  - P95 / P99 latency：
		P95：95% 的 request 在此時間內完成
		P99：99% 的 request 在此時間內完成
		計算方式：對所有 request latency 排序後取第 N 百分位
 


相關憑證路徑: (請自行放入)
- client-certs/private/client.key
- client-certs/public/client.crt

- server-certs/private/server.key
- server-certs/public/server.crt

- CA/ca.pem

錯誤憑證 模擬路徑: (請自行放入)
- client-certs/public/test_error/expired.crt
- client-certs/public/test_error/expired.key
- client-certs/public/test_error/notyet.crt
- client-certs/public/test_error/notyet.key
- client-certs/public/test_error/selfsigned.crt
- client-certs/public/test_error/selfsigned.key
- client-certs/public/test_error/badclient.crt
- client-certs/public/test_error/badclient.key
- client-certs/public/test_error/wronghost.crt
- client-certs/public/test_error/wronghost.key