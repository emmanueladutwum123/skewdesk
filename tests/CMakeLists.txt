include(FetchContent)

set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.15.2
)
FetchContent_MakeAvailable(googletest)

add_executable(skewdesk_tests
  test_black_scholes.cpp
  test_implied_volatility.cpp
  test_parity.cpp
  test_chain.cpp
  test_svi.cpp
  test_surface.cpp
  test_portfolio.cpp
)
target_link_libraries(skewdesk_tests PRIVATE skewdesk GTest::gtest_main)
target_compile_options(skewdesk_tests PRIVATE -Wall -Wextra)

include(GoogleTest)
gtest_discover_tests(skewdesk_tests)
