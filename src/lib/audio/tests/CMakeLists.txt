# ZannaAUD CTests

add_executable(test_vaud_audit_fixes
        test_vaud_audit_fixes.c
        ../src/vaud_wav.c
)

target_include_directories(test_vaud_audit_fixes PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

if (UNIX AND NOT APPLE)
    target_link_libraries(test_vaud_audit_fixes PRIVATE m)
endif ()

add_test(NAME test_vaud_audit_fixes COMMAND test_vaud_audit_fixes)
set_tests_properties(test_vaud_audit_fixes PROPERTIES LABELS "unit;audio" TIMEOUT 10)

add_executable(test_vaud_core_fixes
        test_vaud_core_fixes.c
        ../src/vaud.c
        ../src/vaud_wav.c
        ../src/vaud_mixer.c
)

target_include_directories(test_vaud_core_fixes PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

# These are white-box tests: they publish music and voice state straight into a
# context and then call vaud_mixer_render() on the test thread, with no lock of
# their own. vaud_create()'s background streamer thread takes ctx->mutex every
# VAUD_STREAM_THREAD_INTERVAL_MS, and the mixer answers a busy state lock with
# its silent fallback period, leaving the state it was asked to clamp untouched.
# Building without the streamer keeps this binary single-threaded and its
# assertions deterministic; vaud_update() is still exercised as the app-thread
# refill pump.
target_compile_definitions(test_vaud_core_fixes PRIVATE VAUD_STREAM_THREAD_ENABLE=0)

if (UNIX AND NOT APPLE)
    target_link_libraries(test_vaud_core_fixes PRIVATE m)
endif ()

add_test(NAME test_vaud_core_fixes COMMAND test_vaud_core_fixes)
set_tests_properties(test_vaud_core_fixes PROPERTIES LABELS "unit;audio" TIMEOUT 10)
