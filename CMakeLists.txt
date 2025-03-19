cmake_minimum_required(VERSION 3.10)
project(crypto_arb_bot)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Source files
set(SOURCES
    src/main.cpp
    src/core/orderbook.cpp
    # src/core/trade_utils.cpp
    src/engine/triangle_scanner.cpp
    src/engine/simulator.cpp
)

# Create the executable
add_executable(crypto_arb_bot ${SOURCES})

# Include directories
target_include_directories(crypto_arb_bot PRIVATE
    include
)

# Find Boost
find_package(Boost REQUIRED system thread)
if (Boost_FOUND)
    target_include_directories(crypto_arb_bot PRIVATE ${Boost_INCLUDE_DIRS})
    target_link_libraries(crypto_arb_bot PRIVATE ${Boost_LIBRARIES})
endif()

# Find OpenSSL
find_package(OpenSSL REQUIRED)
if (OPENSSL_FOUND)
    target_link_libraries(crypto_arb_bot PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()

# Link pthread explicitly (Linux only)
target_link_libraries(crypto_arb_bot PRIVATE pthread)
