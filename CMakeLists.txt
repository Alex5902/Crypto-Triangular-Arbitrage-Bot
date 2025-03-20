cmake_minimum_required(VERSION 3.10)
project(crypto_arb_bot)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# -------------------
# Main Bot Executable
# -------------------
set(SOURCES
    src/main.cpp
    src/core/orderbook.cpp
    src/core/wallet.cpp   
    src/engine/triangle_scanner.cpp
    src/engine/simulator.cpp
    src/exchange/binance_dry_executor.cpp
    src/exchange/binance_real_executor.cpp
    src/exchange/binance_account_sync.cpp
    src/exchange/key_encryptor.cpp         
)

add_executable(crypto_arb_bot ${SOURCES})

target_include_directories(crypto_arb_bot PRIVATE
    include
    src
    /usr/include/x86_64-linux-gnu
)

# -----------------------
# Encrypt Keys Executable
# -----------------------
add_executable(encrypt_keys
    src/tools/encrypt_keys.cpp
    src/exchange/key_encryptor.cpp
)

target_include_directories(encrypt_keys PRIVATE
    include
    src
)

# -----------------------
# External Dependencies
# -----------------------
find_package(Boost REQUIRED system thread)
if (Boost_FOUND)
    target_include_directories(crypto_arb_bot PRIVATE ${Boost_INCLUDE_DIRS})
    target_link_libraries(crypto_arb_bot PRIVATE ${Boost_LIBRARIES})
endif()

find_package(OpenSSL REQUIRED)
if (OPENSSL_FOUND)
    target_link_libraries(crypto_arb_bot PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    target_link_libraries(encrypt_keys PRIVATE OpenSSL::Crypto)
endif()

find_package(CURL REQUIRED)
if (CURL_FOUND)
    message(STATUS "Found CURL: ${CURL_INCLUDE_DIRS}")
    target_include_directories(crypto_arb_bot PRIVATE ${CURL_INCLUDE_DIRS})
    target_link_libraries(crypto_arb_bot PRIVATE ${CURL_LIBRARIES})
endif()

# Linux threading
target_link_libraries(crypto_arb_bot PRIVATE pthread)
target_link_libraries(encrypt_keys PRIVATE pthread)
