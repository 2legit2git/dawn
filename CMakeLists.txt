cmake_minimum_required(VERSION 3.16)
project(dawn C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(USE_LIBAI "Build with Apple Intelligence (macOS 26+)" OFF)

file(GLOB DAWN_SRCS src/dawn*.c)
file(GLOB HIGHLIGHT_SRCS src/highlight/*.c src/highlight/lang/*.c)

set(PCRE2_BUILD_PCRE2GREP OFF CACHE BOOL "" FORCE)
set(PCRE2_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(PCRE2_SUPPORT_JIT ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(third-party/pcre2)

add_subdirectory(third-party/utf8proc)

find_package(CURL REQUIRED)

add_executable(dawn
    ${DAWN_SRCS}
    ${HIGHLIGHT_SRCS}
    src/platform_posix.c
    src/main_term.c
    third-party/cJSON.c
)

target_include_directories(dawn PRIVATE
    src
    third-party
    ${CMAKE_BINARY_DIR}/third-party/pcre2
    third-party/pcre2/src
)

target_compile_options(dawn PRIVATE
    -Wall -Wextra -Wpedantic -Werror -Wno-unused-function
    $<$<C_COMPILER_ID:AppleClang,Clang>:-Wno-keyword-macro -Wno-newline-eof>
)

target_link_libraries(dawn PRIVATE pcre2-8-static utf8proc CURL::libcurl)

if(APPLE)
    target_link_libraries(dawn PRIVATE "-framework ApplicationServices")
endif()

if(APPLE AND USE_LIBAI)
    target_sources(dawn PRIVATE src/search.c)
    target_compile_definitions(dawn PRIVATE USE_LIBAI)
    target_include_directories(dawn PRIVATE libai)
    target_compile_options(dawn PRIVATE -Wno-unused-variable -Wno-unused-but-set-variable)

    add_custom_target(libai_build
        COMMAND make static-rel
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/libai
    )
    add_dependencies(dawn libai_build)

    target_link_libraries(dawn PRIVATE
        ${CMAKE_SOURCE_DIR}/libai/build/static/${CMAKE_SYSTEM_PROCESSOR}/release/libai.a
        "-framework Foundation"
        "-framework FoundationModels"
        -L/usr/lib/swift -lswiftCore
    )
endif()

if(EMSCRIPTEN)
    file(GLOB WEB_SRCS src/dawn*.c)
    list(FILTER WEB_SRCS EXCLUDE REGEX "dawn_args.c")

    add_executable(dawn-web
        ${WEB_SRCS}
        ${HIGHLIGHT_SRCS}
        src/platform_web.c
        src/main_web.c
        third-party/cJSON.c
    )

    target_include_directories(dawn-web PRIVATE
        src
        third-party
        ${CMAKE_BINARY_DIR}/third-party/pcre2
        third-party/pcre2/src
    )

    target_compile_options(dawn-web PRIVATE -O2)
    target_link_libraries(dawn-web PRIVATE pcre2-8-static utf8proc)

    set_target_properties(dawn-web PROPERTIES
        OUTPUT_NAME "dawn"
        SUFFIX ".html"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/web"
    )

    target_link_options(dawn-web PRIVATE
        -sWASM=1
        -sALLOW_MEMORY_GROWTH=1
        -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,UTF8ToString,stringToUTF8,lengthBytesUTF8,setValue,getValue
        -sEXPORTED_FUNCTIONS=_main,_malloc,_free,_web_on_resize,_web_on_key,_web_on_mouse,_dawn_web_load_file,_dawn_web_new_document,_dawn_web_save
        -sFORCE_FILESYSTEM=1
        -lidbfs.js
        --shell-file ${CMAKE_SOURCE_DIR}/src/shell.html
    )
endif()

install(TARGETS dawn RUNTIME DESTINATION bin)
