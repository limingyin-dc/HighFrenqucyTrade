cmake_minimum_required(VERSION 3.10)
project(SimNowTrader)

# C++17：atomic CAS on double / structured bindings 等更安全
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 编译选项
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -Wall -Wextra -pthread")
# 关掉 CTP 头文件里大量的 unused-parameter 警告
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-unused-parameter")

# 头文件搜索路径
include_directories(
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src/common
    ${PROJECT_SOURCE_DIR}/src/md_engine
    ${PROJECT_SOURCE_DIR}/src/td_engine
    ${PROJECT_SOURCE_DIR}/src/strategy
)

# 库文件路径
link_directories(${PROJECT_SOURCE_DIR}/lib)

# 只收集 .cpp 源文件（头文件不参与编译）
file(GLOB_RECURSE ALL_SRCS "src/*.cpp")

add_executable(SimNowTrader ${ALL_SRCS})

target_link_libraries(SimNowTrader
    thostmduserapi_se   # libthostmduserapi_se.so
    thosttraderapi_sm   # libthosttraderapi_sm.so
    pthread
    dl
)

# 运行时需要的文件复制到 build 目录
add_custom_command(TARGET SimNowTrader POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${PROJECT_SOURCE_DIR}/lib ${CMAKE_BINARY_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy
        ${PROJECT_SOURCE_DIR}/config.xml ${CMAKE_BINARY_DIR}/config.xml
    COMMENT "复制 .so 和 config.xml 到 build 目录"
)
