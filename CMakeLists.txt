cmake_minimum_required(VERSION 3.15)
project(AsioConnFlow)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 強制 Visual Studio 將輸出到 out/release
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/out/release")

# Boost 設定
set(BOOST_ROOT "D:/Env_set/boost_1_89_0")
set(BOOST_INCLUDEDIR "${BOOST_ROOT}")
find_package(Boost 1.89 REQUIRED)

include_directories(${BOOST_INCLUDEDIR})

# Include 兩個資料夾
include_directories(${CMAKE_SOURCE_DIR}/src)
include_directories(${CMAKE_SOURCE_DIR}/server)

# 建立 client 可執行檔
add_executable(AsioConnClient src/AsioConnFlow.cpp)

# 建立 server 可執行檔
add_executable(AsioConnServer server/AsioServer.cpp)



# 如果你用 Boost 非 header-only 模組，才需要這行
# target_link_libraries(${PROJECT_NAME} ${Boost_LIBRARIES})
