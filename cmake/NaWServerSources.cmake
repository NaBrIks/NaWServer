# Canonical source ownership. Every integration selects these groups instead of
# maintaining another copy of the library's implementation-file inventory.
include_guard(GLOBAL)
get_filename_component(NAWSERVER_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(NAWSERVER_SOURCES_CORE
    src/buffer_memory.cpp
    src/http/http_server.cpp src/http/web_request.cpp src/http/web_response.cpp
    src/http/handler.cpp src/http/handler_static.cpp
    src/http/form_parameters.cpp src/http/url_decode.cpp
    src/routing/pipeline.cpp src/routing/route.cpp)
set(NAWSERVER_SOURCES_API_KEYS src/api_keys/store.cpp)
set(NAWSERVER_SOURCES_AUTH src/auth/basic_auth.cpp src/auth/handler_middleware.cpp)
set(NAWSERVER_SOURCES_WS src/websocket/handler_ws.cpp)
set(NAWSERVER_SOURCES_SSE
    src/sse/backend.cpp src/sse/encoder.cpp
    src/sse/response_bridge.cpp src/sse/event_source.cpp)
set(NAWSERVER_SOURCES_HTTP_WIRE
    src/http/wire/request_parser.cpp src/http/wire/response_writer.cpp)
set(NAWSERVER_SOURCES_TCP_SESSION
    src/backends/tcp/session_io.cpp src/backends/tcp/http_connection.cpp)
set(NAWSERVER_SOURCES_TCP_REGISTRY src/backends/tcp/connection_registry.cpp)
set(NAWSERVER_SOURCES_TCP_RUNTIME
    src/backends/tcp/http_backend.cpp src/backends/tcp/queued_transport.cpp
    src/backends/tcp/platform/worker_signal.cpp)
set(NAWSERVER_SOURCES_TCP_MANAGED src/backends/tcp/http_connection_manager.cpp)
set(NAWSERVER_SOURCES_WS_WIRE
    src/websocket/wire/frame_codec.cpp src/websocket/wire/handshake.cpp)
set(NAWSERVER_SOURCES_TCP_WS
    src/backends/tcp/websocket_connection.cpp src/backends/tcp/websocket_tx_pool.cpp)
set(NAWSERVER_SOURCES_SSE_QUEUED src/sse/queued_adapter.cpp)
set(NAWSERVER_SOURCES_TCP_SSE
    src/backends/tcp/sse_stream.cpp src/backends/tcp/tcp_sse.cpp)

set_property(GLOBAL PROPERTY NAWSERVER_SOURCE_ROOT "${NAWSERVER_SOURCE_ROOT}")
foreach(group CORE AUTH API_KEYS WS SSE HTTP_WIRE TCP_SESSION TCP_REGISTRY TCP_RUNTIME TCP_MANAGED
        WS_WIRE TCP_WS SSE_QUEUED TCP_SSE)
    set_property(GLOBAL PROPERTY NAWSERVER_SOURCES_${group} "${NAWSERVER_SOURCES_${group}}")
endforeach()

function(nawserver_collect_sources result)
    get_property(source_root GLOBAL PROPERTY NAWSERVER_SOURCE_ROOT)
    set(files "")
    foreach(group IN LISTS ARGN)
        get_property(known GLOBAL PROPERTY NAWSERVER_SOURCES_${group} SET)
        if(NOT known)
            message(FATAL_ERROR "Unknown NaWServer source group: ${group}")
        endif()
        get_property(group_sources GLOBAL PROPERTY NAWSERVER_SOURCES_${group})
        foreach(path IN LISTS group_sources)
            list(APPEND files "${source_root}/${path}")
        endforeach()
    endforeach()
    set(${result} "${files}" PARENT_SCOPE)
endfunction()
