# A single static HTTP(S) library is shared by the SP/MP/dedicated targets.
# Keep dependency switches scoped to this function, rather than changing the
# options used by the game's other FetchContent dependencies.
function(kisakcod_add_curl)
    if(TARGET CURL::libcurl)
        return()
    endif()
    include(FetchContent)
    set(BUILD_CURL_EXE OFF)
    set(BUILD_SHARED_LIBS OFF)
    set(BUILD_STATIC_LIBS ON)
    set(BUILD_TESTING OFF)
    set(BUILD_LIBCURL_DOCS OFF)
    set(BUILD_MISC_DOCS OFF)
    set(ENABLE_CURL_MANUAL OFF)
    set(CURL_DISABLE_INSTALL ON)
    set(CURL_STATIC_CRT ON)
    set(ENABLE_THREADED_RESOLVER ON)
    set(CURL_USE_PKGCONFIG OFF)
    set(CURL_USE_LIBPSL OFF)
    set(CURL_USE_LIBSSH2 OFF)
    set(CURL_USE_GSSAPI OFF)
    set(USE_LIBIDN2 OFF)
    # These are tri-state CACHE STRING settings in curl, rather than options.
    # Pin them explicitly so host package discovery cannot add cross-build deps.
    set(CURL_ZLIB OFF CACHE STRING "libcurl zlib support" FORCE)
    set(CURL_BROTLI OFF CACHE STRING "libcurl Brotli support" FORCE)
    set(CURL_ZSTD OFF CACHE STRING "libcurl Zstd support" FORCE)
    set(USE_NGHTTP2 OFF)
    set(USE_NGTCP2 OFF)
    foreach(protocol DICT FILE FTP GOPHER IMAP LDAP LDAPS MQTT POP3 RTSP SMTP TELNET TFTP WEBSOCKETS IPFS)
        set(CURL_DISABLE_${protocol} ON)
    endforeach()
    if(WIN32)
        # Use Windows' certificate store and TLS implementation. No OpenSSL
        # runtime DLLs or separately maintained CA bundle are needed.
        set(CURL_USE_SCHANNEL ON)
        set(CURL_USE_OPENSSL OFF)
    endif()
    FetchContent_Declare(kisakcod_curl
        URL https://github.com/curl/curl/releases/download/curl-8_23_0/curl-8.23.0.tar.xz
        URL_HASH SHA256=8dadd7d2536a314f982ad05a651b713f426bf7d02cc2c76cffb9796f2218e6c5
        TLS_VERIFY TRUE
    )
    FetchContent_MakeAvailable(kisakcod_curl)
endfunction()

kisakcod_add_curl()
target_link_libraries(${PROJECT_NAME} PRIVATE CURL::libcurl)

# Retain the dependency's notice beside every statically linked executable.
# FetchContent properties remain available to sibling SP/MP/dedicated targets.
FetchContent_GetProperties(kisakcod_curl SOURCE_DIR _kisakcod_curl_source_dir)
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${_kisakcod_curl_source_dir}/COPYING"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/curl-COPYING.txt"
    COMMENT "COPYING CURL LICENSE"
    VERBATIM
)
