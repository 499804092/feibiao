cmake_minimum_required(VERSION 3.20)
project(view)

file(GLOB_RECURSE ${PROJECT_NAME}_src ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)

find_package(OpenCV REQUIRED)
add_library(${PROJECT_NAME} OBJECT ${${PROJECT_NAME}_src})

target_compile_features(${PROJECT_NAME} PUBLIC c_std_99 cxx_std_20)

target_link_libraries(${PROJECT_NAME} PUBLIC ${OpenCV_LIBS})

target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/inc)

target_link_libraries(${PROJECT_NAME} PRIVATE utility)
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../utility/inc)

target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../threads/inc)

# 掩膜窗口要用同一套 ColorDetector 参数，所以要能看到 detect 模块的头文件
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../detect/inc)
target_link_libraries(${PROJECT_NAME} PRIVATE detect)

# 【新增】显示线程同时负责网页推流，所以要能看到 web 模块的头文件
#   （目标 web 必须已经定义 —— 顶层 CMakeLists 里 add_subdirectory(src/web) 放在 view 之前）
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../web/inc)
target_link_libraries(${PROJECT_NAME} PRIVATE web)
