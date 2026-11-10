add_library(drivebrain_comms src/FlowmeterComms.cpp)
target_include_directories(drivebrain_comms PUBLIC include ${PROJECT_SOURCE_DIR}/drivebrain_core/include)
target_link_libraries(drivebrain_comms PUBLIC Threads::Threads)
target_compile_options(drivebrain_comms PRIVATE -Wall -Wextra -Wpedantic)
