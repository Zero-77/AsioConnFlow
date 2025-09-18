cmake_minimum_required(VERSION 3.15)
project(AsioConnFlow)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

#Windows 專用設定
if (WIN32)
    # 強制 Visual Studio 將輸出到 out/release
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/out/release")

    # 設定 OpenSSL 安裝位置(Windows)
    set(OPENSSL_ROOT_DIR "C:/Program Files/OpenSSL-Win64")
    set(OPENSSL_INCLUDE_DIR "${OPENSSL_ROOT_DIR}/include")
    if(CMAKE_BUILD_TYPE MATCHES Debug)
        set(OPENSSL_LIB_DIR "${OPENSSL_ROOT_DIR}/lib/VC/x64/MDd")
    else()
        set(OPENSSL_LIB_DIR "${OPENSSL_ROOT_DIR}/lib/VC/x64/MD")
    endif()
    link_directories(${OPENSSL_LIB_DIR})

    # Boost 設定(Windows)
    set(BOOST_ROOT "D:/Env_set/boost_1_89_0")
    set(BOOST_INCLUDEDIR "${BOOST_ROOT}")
    find_package(Boost 1.89 REQUIRED)

    include_directories(${BOOST_INCLUDEDIR} ${OPENSSL_INCLUDE_DIR})

    # 建立 client 可執行檔
    #add_executable(AsioConnClient src/AsioConnFlow.cpp)

    # 建立 server 可執行檔
    #add_executable(AsioConnServer server/AsioServer.cpp)

else() # Linux / WSL
    find_package(OpenSSL REQUIRED)
    find_package(Boost REQUIRED COMPONENTS system filesystem)
endif()


# Include 兩個資料夾
include_directories(${CMAKE_SOURCE_DIR}/src)
include_directories(${CMAKE_SOURCE_DIR}/server)

# 如果你用 Boost 非 header-only 模組，才需要這行
add_executable(AsioConnClient src/AsioConnFlow.cpp)
target_link_libraries(
    AsioConnClient
    ${Boost_LIBRARIES}
    "$<$<PLATFORM_ID:Windows>:libssl;libcrypto>"  # Windows 下連結這兩個
    "$<$<NOT:$<PLATFORM_ID:Windows>>:OpenSSL::SSL;OpenSSL::Crypto>" # Linux 下用 target
)

add_executable(AsioConnServer server/AsioServer.cpp)
target_link_libraries(
    AsioConnServer
    ${Boost_LIBRARIES}
    "$<$<PLATFORM_ID:Windows>:libssl;libcrypto>"
    "$<$<NOT:$<PLATFORM_ID:Windows>>:OpenSSL::SSL;OpenSSL::Crypto>"
)

# 安裝規則
install(TARGETS AsioConnClient AsioConnServer
        RUNTIME DESTINATION bin)

# 如果有設定檔或資源檔
# install(FILES config.yaml DESTINATION etc/asioapp)

# 打包設定
set(CPACK_PACKAGE_NAME "AsioConnFlow-Linux")
set(CPACK_PACKAGE_VERSION "1.0.0")
set(CPACK_GENERATOR "DEB;TGZ")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "nk")
include(CPack)