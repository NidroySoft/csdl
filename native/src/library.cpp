// library.cpp - Versión final compatible con libtorrent 2.0.11 y el ejecutable de test
#include "library.h"
#include "streaming.h"
#include <libtorrent/torrent_handle.hpp>
#include <algorithm>
#include <cstring>

// ── Prototipos de funciones implementadas en streaming.cpp ──
extern "C" {
    const char* start_stream_server_impl(lt::session* session, lt::torrent_handle* torrent, int32_t file_index, int32_t port);
    void        stop_stream_server_impl();
    uint8_t     is_stream_server_running_impl();
    void        reset_stream_server_impl();
    const char* get_last_stream_error_impl();
}

extern "C" {

    // ── sesión ──────────────────────────────────────────────────────────
    lt::session* create_session(lt::settings_pack* pack) {
        lt::session_params params;
        if (pack) params.settings = *pack;
        return new lt::session(params);
    }

    void destroy_session(lt::session* session) {
        if (!session) return;
        session->abort();
        delete session;
    }

    void apply_settings(lt::session* session, lt::settings_pack* settings) {
        if (!session || !settings) return;
        session->apply_settings(*settings);
    }

    // ── eventos (delegan en events.cpp, ahora por sesión) ─────────────
    void clear_event_callback(lt::session* session) {
        if (!session) return;
        cs_destroy_event_handler_for_session(session);
    }

    void set_event_callback(lt::session* session, cs_alert_callback callback, bool include_unmapped_events) {
        if (!session) return;
        if (!callback) {
            clear_event_callback(session);
            return;
        }
        cs_create_event_handler(session, callback, include_unmapped_events);
    }

    // ── torrent info ───────────────────────────────────────────────────
    lt::torrent_info* create_torrent_bytes(const char* data, long length) {
        const lt::span buffer(data, length);
        lt::load_torrent_limits cfg;
        return new lt::torrent_info(buffer, cfg, lt::from_span);
    }

    lt::torrent_info* create_torrent_file(const char* file_path) {
        return new lt::torrent_info(std::string(file_path));
    }

    void destroy_torrent(lt::torrent_info* torrent) { delete torrent; }

    // ── attach / detach ────────────────────────────────────────────────
    lt::torrent_handle* attach_torrent(lt::session* session, lt::torrent_info* torrent, const char* save_path) {
        if (!session || !torrent) return nullptr;
        lt::add_torrent_params params;
        if (save_path && save_path[0]) params.save_path = save_path;
        params.flags |= lt::torrent_flags::paused;
        params.flags &= ~lt::torrent_flags::auto_managed;
        params.storage_mode = lt::storage_mode_sparse;
        params.ti = std::make_shared<lt::torrent_info>(*torrent);
        auto* handle = new lt::torrent_handle(session->add_torrent(params));
        if (!handle->is_valid()) { delete handle; return nullptr; }
        return handle;
    }

    void detach_torrent(lt::session* session, lt::torrent_handle* torrent) {
        if (!session || !torrent) return;
        torrent->pause();
        session->remove_torrent(*torrent);
    }

    // ── metadatos y ficheros ───────────────────────────────────────────
    torrent_metadata* get_torrent_info(lt::torrent_info* torrent) {
        if (!torrent) return nullptr;
        auto* info = new torrent_metadata();
        info->name = new char[torrent->name().size() + 1](); std::copy(torrent->name().begin(), torrent->name().end(), info->name);
        info->creator = new char[torrent->creator().size() + 1](); std::copy(torrent->creator().begin(), torrent->creator().end(), info->creator);
        info->comment = new char[torrent->comment().size() + 1](); std::copy(torrent->comment().begin(), torrent->comment().end(), info->comment);
        info->total_files = torrent->num_files();
        info->total_size = torrent->total_size();
        info->creation_date = torrent->creation_date();
        auto hash = torrent->info_hashes();
        if (hash.has_v1()) std::copy(hash.v1.begin(), hash.v1.end(), info->info_hash_v1);
        else std::fill_n(info->info_hash_v1, 20, 0);
        if (hash.has_v2()) std::copy(hash.v2.begin(), hash.v2.end(), info->info_hash_v2);
        else std::fill_n(info->info_hash_v2, 32, 0);
        return info;
    }

    void destroy_torrent_info(torrent_metadata* info) {
        if (!info) return;
        delete[] info->name; delete[] info->creator; delete[] info->comment; delete info;
    }

    void get_torrent_file_list(lt::torrent_info* torrent, torrent_file_list* file_list) {
        if (!torrent || !file_list) return;
        const auto& files = torrent->files();
        auto num_files = files.num_files();
        file_list->files = new torrent_file_information[num_files];
        file_list->length = num_files;

        for (lt::file_index_t i(0); i != files.end_file(); ++i) {
            int idx = static_cast<int>(i);
            auto name = files.file_name(i);
            auto path = files.file_path(i);

            file_list->files[idx] = {
                idx,
                files.file_offset(i),
                files.file_size(i),
                files.mtime(i),
                new char[name.size() + 1](),
                new char[path.size() + 1](),
                false,
                files.pad_file_at(i)
            };
            std::copy(name.begin(), name.end(), file_list->files[idx].file_name);
            std::copy(path.begin(), path.end(), file_list->files[idx].file_path);
        }
    }

    void destroy_torrent_file_list(torrent_file_list* file_list) {
        if (!file_list || !file_list->files) return;
        for (int i = 0; i < file_list->length; ++i) {
            delete[] file_list->files[i].file_name;
            delete[] file_list->files[i].file_path;
        }
        delete[] file_list->files;
    }

    // ── prioridades y control ──────────────────────────────────────────
    void set_file_dl_priority(lt::torrent_handle* torrent, int32_t file_index, uint8_t priority) {
        if (!torrent) return;
        torrent->file_priority(lt::file_index_t(file_index), lt::download_priority_t(priority));
    }

    uint8_t get_file_dl_priority(lt::torrent_handle* torrent, int32_t file_index) {
        if (!torrent) return 0;
        return static_cast<uint8_t>(torrent->file_priority(lt::file_index_t(file_index)));
    }

    void start_torrent(lt::torrent_handle* torrent) { if (torrent) torrent->resume(); }
    void stop_torrent(lt::torrent_handle* torrent) { if (torrent) torrent->pause(); }

    void reannounce_torrent(lt::torrent_handle* torrent, int32_t seconds, uint8_t ignore_min_interval) {
        if (!torrent) return;
        lt::reannounce_flags_t flags = {};
        if (ignore_min_interval) flags |= lt::torrent_handle::ignore_min_interval;
        torrent->force_reannounce(seconds, -1, flags);
    }

    void get_torrent_status(lt::torrent_handle* torrent, torrent_status* ts) {
        if (!torrent || !ts) return;
        auto s = torrent->status();
        if (s.errc) ts->state = cs_torrent_state::torrent_error;
        else {
            switch (s.state) {
            case lt::torrent_status::checking_files: ts->state = cs_torrent_state::torrent_checking; break;
            case lt::torrent_status::checking_resume_data: ts->state = cs_torrent_state::torrent_checking_resume; break;
            case lt::torrent_status::downloading_metadata: ts->state = cs_torrent_state::torrent_metadata_downloading; break;
            case lt::torrent_status::downloading: ts->state = cs_torrent_state::torrent_downloading; break;
            case lt::torrent_status::seeding: ts->state = cs_torrent_state::torrent_seeding; break;
            case lt::torrent_status::finished: ts->state = cs_torrent_state::torrent_finished; break;
            default: ts->state = cs_torrent_state::torrent_state_unknown; break;
            }
        }
        ts->progress = s.progress;
        ts->count_peers = s.num_peers;
        ts->count_seeds = s.num_seeds;
        ts->bytes_uploaded = s.total_payload_upload;
        ts->bytes_downloaded = s.total_payload_download;
        ts->upload_rate = s.upload_payload_rate;
        ts->download_rate = s.download_payload_rate;
    }

    // ── API de streaming y seek (implementaciones en streaming.cpp) ───
    const char* start_stream_server(lt::session* session, lt::torrent_handle* torrent, int32_t file_index, int32_t port) {
        return start_stream_server_impl(session, torrent, file_index, port);
    }
    void stop_stream_server() { stop_stream_server_impl(); }
    uint8_t is_stream_server_running() { return is_stream_server_running_impl(); }
    void reset_stream_server() { reset_stream_server_impl(); }
    const char* get_last_stream_error() { return get_last_stream_error_impl(); }

    uint8_t have_piece(lt::torrent_handle* torrent, int32_t piece_index) {
        if (!torrent || !torrent->is_valid()) return 0;
        auto st = torrent->status();
        if (piece_index < 0 || piece_index >= (int32_t)st.pieces.size()) return 0;
        return st.pieces[lt::piece_index_t(piece_index)] ? 1 : 0;
    }

    int32_t get_piece_length(lt::torrent_handle* torrent) {
        if (!torrent || !torrent->is_valid()) return 0;
        auto st = torrent->status();
        auto ti = st.torrent_file.lock();
        if (!ti) return 0;
        return static_cast<int32_t>(ti->piece_length());
    }

#ifndef CSDL_STATIC
    CSDL_EXPORT uint8_t is_byte_available_impl(lt::torrent_handle* torrent, int32_t file_index, int64_t byte_position) {
        return cs_stream::is_byte_available(torrent, file_index, byte_position) ? 1 : 0;
    }

    CSDL_EXPORT uint8_t prioritize_seek_range_impl(lt::torrent_handle* torrent, int32_t file_index, int64_t byte_position, int64_t piece_size) {
        return cs_stream::prioritize_seek_range(torrent, file_index, byte_position, piece_size) ? 1 : 0;
    }
#endif
}