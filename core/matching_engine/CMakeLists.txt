add_library(llme_matching
  src/order_book.cpp
  src/occupancy_tree.cpp
)

target_include_directories(llme_matching
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(LLME_BUILD_TESTS)
  add_executable(llme_matching_tests
    tests/order_book_test.cpp
  )
  target_link_libraries(llme_matching_tests PRIVATE llme_matching)
  add_test(NAME llme_matching_tests COMMAND llme_matching_tests)

  find_package(GTest QUIET)
  if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(googletest
      GIT_REPOSITORY https://github.com/google/googletest.git
      GIT_TAG v1.17.0
    )
    # Keep GoogleTest's MSVC CRT option local to its own project.
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
  endif()

  add_executable(llme_matching_gtests
    tests/order_book_add_result_gtest.cpp
  )
  target_link_libraries(llme_matching_gtests PRIVATE llme_matching GTest::gtest_main)

  include(GoogleTest)
  gtest_discover_tests(llme_matching_gtests)
endif()
