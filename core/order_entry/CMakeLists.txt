add_library(order_entry INTERFACE)
target_include_directories(order_entry INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(LLMES_BUILD_TESTS)
  add_executable(order_entry_tests tests/test.cpp)
  target_link_libraries(order_entry_tests PRIVATE order_entry)
endif()