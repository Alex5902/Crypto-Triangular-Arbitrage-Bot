cmake_minimum_required(VERSION 3.10)
project(crypto_arb_bot)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Add include directories
include_directories(
    include
    include/websocketpp
    /usr/include
)

# Find Boost
find_package(Boost REQUIRED system thread)
if (Boost_FOUND)
    include_directories(${Boost_INCLUDE_DIRS})
endif()

# Find OpenSSL
find_package(OpenSSL REQUIRED)

# Add the executable
add_executable(crypto_arb_bot
    src/core/orderbook.cpp
    src/main.cpp
)

# Link libraries (Boost, pthread, OpenSSL)
target_link_libraries(crypto_arb_bot
    ${Boost_LIBRARIES}
    OpenSSL::SSL
    OpenSSL::Crypto
    pthread
)
