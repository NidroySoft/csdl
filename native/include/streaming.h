#ifndef CSDL_STREAMING_H
#define CSDL_STREAMING_H
#include <stdint.h>
#include <string>

namespace libtorrent { class torrent_handle; }
namespace lt = libtorrent;

namespace cs_stream {
    bool start_server(lt::torrent_handle* handle, int32_t file_index, int32_t port, std::string& out_url);
    void stop_server();
    bool is_running();
    const char* last_error();
    void reset_server_state();

    // Funciones que requieren el handle del torrent (funcionan sin el servidor HTTP)
    bool is_byte_available(lt::torrent_handle* handle, int32_t file_index, int64_t byte_position);
    bool prioritize_seek_range(lt::torrent_handle* handle, int32_t file_index, int64_t byte_position, int64_t piece_size);
}

// C API exports
extern "C" {
    const char* start_stream_server_impl(lt::torrent_handle* handle, int32_t file_index, int32_t port);
    void        stop_stream_server_impl();
    uint8_t     is_stream_server_running_impl();
    void        reset_stream_server_impl();

    uint8_t is_byte_available_impl(lt::torrent_handle* torrent, int32_t file_index, int64_t byte_position);
    uint8_t prioritize_seek_range_impl(lt::torrent_handle* torrent, int32_t file_index, int64_t byte_position, int64_t piece_size);
}
#endif