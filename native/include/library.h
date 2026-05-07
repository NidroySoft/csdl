//
// library.h
// Created by Albie on 29/02/2024.
//

#ifndef CSDL_LIBRARY_HPP
#define CSDL_LIBRARY_HPP

#include "events.h"
#include "structs.h"
#include "lib_export.h"

#include <libtorrent/torrent_handle.hpp>

#ifdef __cplusplus
extern "C" {
#endif

    // ── session control ───────────────────────────────────────────────────────
    CSDL_EXPORT lt::session* create_session(lt::settings_pack* pack);
    CSDL_EXPORT void         destroy_session(lt::session* session);

    CSDL_EXPORT void apply_settings(lt::session* session, lt::settings_pack* settings);

    // ── torrent control ───────────────────────────────────────────────────────
    CSDL_EXPORT lt::torrent_info* create_torrent_file(const char* file_path);
    CSDL_EXPORT lt::torrent_info* create_torrent_bytes(const char* data, long length);
    CSDL_EXPORT void              destroy_torrent(lt::torrent_info* torrent);

    CSDL_EXPORT lt::torrent_handle* attach_torrent(lt::session* session, lt::torrent_info* torrent, const char* save_path);
    CSDL_EXPORT void                detach_torrent(lt::session* session, lt::torrent_handle* torrent);

    // ── torrent info ──────────────────────────────────────────────────────────
    CSDL_EXPORT torrent_metadata* get_torrent_info(lt::torrent_info* torrent);
    CSDL_EXPORT int32_t get_piece_length(lt::torrent_handle* torrent);
    CSDL_EXPORT void              destroy_torrent_info(torrent_metadata* info);

    // ── file listing ──────────────────────────────────────────────────────────
    CSDL_EXPORT void get_torrent_file_list(lt::torrent_info* torrent, torrent_file_list* file_list);
    CSDL_EXPORT void destroy_torrent_file_list(torrent_file_list* file_list);

    // ── priority control ──────────────────────────────────────────────────────
    CSDL_EXPORT uint8_t get_file_dl_priority(lt::torrent_handle* torrent, int32_t file_index);
    CSDL_EXPORT void    set_file_dl_priority(lt::torrent_handle* torrent, int32_t file_index, uint8_t priority);

    CSDL_EXPORT void set_event_callback(lt::session* session, cs_alert_callback callback, bool include_unmapped_events);
    CSDL_EXPORT void clear_event_callback(lt::session* session);

    // ── download control ──────────────────────────────────────────────────────
    CSDL_EXPORT void start_torrent(lt::torrent_handle* torrent);
    CSDL_EXPORT void stop_torrent(lt::torrent_handle* torrent);
    CSDL_EXPORT void reannounce_torrent(lt::torrent_handle* torrent, int32_t seconds, uint8_t ignore_min_interval);
    CSDL_EXPORT void get_torrent_status(lt::torrent_handle* torrent, torrent_status* torrent_status);

    // ── streaming server ──────────────────────────────────────────────────────
    // Starts an embedded HTTP server for the given file within the torrent.
    // Returns the base URL (e.g. "http://127.0.0.1:55126/") or nullptr on failure.
    // Call stop_stream_server() before attaching a new torrent.
    CSDL_EXPORT const char* start_stream_server(lt::torrent_handle* torrent, int32_t file_index, int32_t port);
    CSDL_EXPORT void        stop_stream_server();
    CSDL_EXPORT uint8_t     is_stream_server_running();
    CSDL_EXPORT void reset_stream_server();
    CSDL_EXPORT const char* get_last_stream_error();
    CSDL_EXPORT uint8_t have_piece(lt::torrent_handle* torrent, int32_t piece_index);
    CSDL_EXPORT uint8_t is_byte_available_impl(lt::torrent_handle* torrent, int32_t file_index, int64_t byte_position);
    CSDL_EXPORT uint8_t prioritize_seek_range_impl(lt::torrent_handle* torrent, int32_t file_index, int64_t byte_position, int64_t piece_size);

#ifdef __cplusplus
}
#endif
#endif // CSDL_LIBRARY_HPP