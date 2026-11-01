cmake_minimum_required(VERSION 3.16)
project(app_audio_fifo_tests LANGUAGES C)

add_executable(app_audio_fifo_tests
    test_app_audio_fifo.c
    ../../components/app_audio/app_audio_fifo.c)
target_include_directories(app_audio_fifo_tests PRIVATE
    ../../components/app_audio/private)
target_compile_features(app_audio_fifo_tests PRIVATE c_std_11)
target_compile_options(app_audio_fifo_tests PRIVATE -Wall -Wextra -Werror)

enable_testing()
add_test(NAME app_audio_fifo_tests COMMAND app_audio_fifo_tests)
