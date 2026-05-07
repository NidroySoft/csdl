// streaming.cpp – Optimizado para streaming con libtorrent 2.0 y cpp-httplib
// Corregido: set_keep_alive_max_count(0) reemplazado por 1'000'000
#include "streaming.h"
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/torrent_status.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/file_storage.hpp>
#include <libtorrent/download_priority.hpp>
#include <libtorrent/alert_types.hpp>

#include <atomic>
#include <thread>
#include <mutex>
#include <chrono>
#include <sstream>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <vector>
#include <memory>
#include <string>
#include <filesystem>
#include "lib_export.h"

#define CPPHTTPLIB_OPENSSL_SUPPORT 0
#include <httplib.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <share.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <unistd.h>
#include <sys/stat.h>
#endif

#ifdef _DEBUG
#  define STREAM_LOG(fmt, ...)  fprintf(stderr, "[CSDL-STREAM] " fmt "\n", ##__VA_ARGS__)
#else
#  define STREAM_LOG(...)  ((void)0)
#endif

namespace cs_stream {

    // ────────────────────────────────────
    // Constantes de configuración
    // ────────────────────────────────────
    constexpr int PIECE_DEADLINE_MS = 2000;          // Tiempo máximo para obtener una pieza
    constexpr int PIECE_DEADLINE_STEP_MS = 2500;     // Incremento por cada pieza de prefetch
    constexpr int PREFETCH_AHEAD = 10;               // Piezas a anticipar más allá de la actual
    constexpr int PREFETCH_BEHIND = 2;               // Piezas por detrás (para rebobinado)
    constexpr int PIECE_POLL_INTERVAL_MS = 20;       // Intervalo de sondeo del estado

    // ────────────────────────────────────
    // Estado global
    // ────────────────────────────────────
    static std::mutex            g_state_mutex;
    static lt::torrent_handle   g_handle;
    static int                   g_file_index = -1;
    static std::string           g_file_path;
    static std::int64_t          g_file_size = 0;
    static std::int64_t          g_file_offset = 0;
    static int                   g_piece_size = 0;
    static std::string           g_mime_type;
    static std::string           g_etag;
    static std::string           g_last_error;
    static std::atomic<bool>     g_running{ false };

    static std::mutex            g_torrent_mutex;
    static std::unique_ptr<httplib::Server> g_server;
    static std::thread           g_server_thread;

    static std::string           g_url_buffer;
    static std::mutex            g_url_mutex;

    // ────────────────────────────────────
    // Helpers
    // ────────────────────────────────────
    static std::string escape_json(const std::string& s) {
        std::ostringstream o;
        for (char c : s) {
            switch (c) {
            case '"':  o << "\\\""; break;
            case '\\': o << "\\\\"; break;
            case '\b': o << "\\b";  break;
            case '\f': o << "\\f";  break;
            case '\n': o << "\\n";  break;
            case '\r': o << "\\r";  break;
            case '\t': o << "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20)
                    o << "\\u00" << std::hex
                    << static_cast<int>(static_cast<unsigned char>(c))
                    << std::dec;
                else
                    o << c;
            }
        }
        return o.str();
    }

    static std::string detect_mime(const std::string& path) {
        std::string lower = path;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        if (lower.ends_with(".mp4"))  return "video/mp4";
        if (lower.ends_with(".mkv"))  return "video/x-matroska";
        if (lower.ends_with(".avi"))  return "video/x-msvideo";
        if (lower.ends_with(".mov"))  return "video/quicktime";
        if (lower.ends_with(".wmv"))  return "video/x-ms-wmv";
        if (lower.ends_with(".webm")) return "video/webm";
        if (lower.ends_with(".flv"))  return "video/x-flv";
        if (lower.ends_with(".m4v"))  return "video/mp4";
        if (lower.ends_with(".mpg") || lower.ends_with(".mpeg"))
            return "video/mpeg";
        return "application/octet-stream";
    }

    static std::string build_status_json() {
        std::lock_guard<std::mutex> state_lock(g_state_mutex);
        if (!g_handle.is_valid()) return R"({"error":"no handle"})";

        lt::torrent_status st;
        {
            std::lock_guard<std::mutex> tor_lock(g_torrent_mutex);
            st = g_handle.status();
        }
        auto ti_ptr = st.torrent_file.lock();

        std::ostringstream j;
        j << "{\n  \"torrent\": {\n"
            << "    \"progress\": " << st.progress << ",\n"
            << "    \"download_rate\": " << st.download_payload_rate << ",\n"
            << "    \"upload_rate\": " << st.upload_payload_rate << ",\n"
            << "    \"num_peers\": " << st.num_peers << ",\n"
            << "    \"num_seeds\": " << st.num_seeds << ",\n"
            << "    \"total_downloaded\": " << st.total_payload_download << ",\n"
            << "    \"is_finished\": " << (st.is_finished ? "true" : "false") << "\n  }";

        if (ti_ptr && g_file_index >= 0 && g_file_index < ti_ptr->files().num_files()) {
            const auto& files = ti_ptr->files();
            lt::file_index_t fidx{ g_file_index };

            std::vector<std::int64_t> fprog;
            {
                std::lock_guard<std::mutex> tor_lock(g_torrent_mutex);
                g_handle.file_progress(fprog);
            }
            std::int64_t file_down = (g_file_index < (int)fprog.size()) ? fprog[g_file_index] : 0;

            int first_piece = (int)(files.file_offset(fidx) / g_piece_size);
            int last_piece = (int)((files.file_offset(fidx) + g_file_size - 1) / g_piece_size);
            int dl_pieces = 0, first_missing = -1;

            for (int i = first_piece; i <= last_piece; ++i) {
                bool have = (i < (int)st.pieces.size()) && st.pieces[lt::piece_index_t(i)];
                if (have) dl_pieces++;
                else if (first_missing < 0) first_missing = i;
            }
            int first_missing_rel = (first_missing >= 0) ? (first_missing - first_piece) : -1;
            std::int64_t contiguous = (first_missing >= 0)
                ? std::min((std::int64_t)(first_missing - first_piece) * g_piece_size, g_file_size)
                : g_file_size;

            std::string safe_name = escape_json(std::string(files.file_name(fidx)));

            j << ",\n  \"file\": {\n"
                << "    \"index\": " << g_file_index << ",\n"
                << "    \"name\": \"" << safe_name << "\",\n"
                << "    \"size\": " << g_file_size << ",\n"
                << "    \"offset_in_torrent\": " << g_file_offset << ",\n"
                << "    \"downloaded_bytes\": " << file_down << ",\n"
                << "    \"progress\": " << (g_file_size > 0 ? (double)file_down / g_file_size : 0.0) << ",\n"
                << "    \"total_pieces\": " << (last_piece - first_piece + 1) << ",\n"
                << "    \"downloaded_pieces\": " << dl_pieces << ",\n"
                << "    \"first_missing_piece\": " << first_missing_rel << ",\n"
                << "    \"contiguous_completed_bytes\": " << contiguous << "\n  }";
        }
        j << "\n}";
        return j.str();
    }

    // ────────────────────────────────────
    // ContentProvider – Reescrito con deadlines reales y espera eficiente
    // ────────────────────────────────────
    class TorrentContentProvider {
    public:
        TorrentContentProvider(const std::string& path,
            lt::torrent_handle handle,
            std::int64_t file_offset,
            std::int64_t file_size,
            std::int64_t piece_size,
            std::mutex& torrent_mutex,
            int file_index)
            : handle_(std::move(handle)),
            file_offset_(file_offset),
            file_size_(file_size),
            piece_size_(piece_size),
            torrent_mutex_(torrent_mutex),
            file_path_(path),
            file_index_(file_index)
        {
            read_buf_.resize(65536);
            expected_seq_ = 0;
        }

        ~TorrentContentProvider() { if (file_) fclose(file_); }

        TorrentContentProvider(const TorrentContentProvider&) = delete;
        TorrentContentProvider& operator=(const TorrentContentProvider&) = delete;
        TorrentContentProvider(TorrentContentProvider&&) = default;
        TorrentContentProvider& operator=(TorrentContentProvider&&) = default;

        bool operator()(uint64_t offset, uint64_t length, httplib::DataSink& sink) {
            try {
                if (offset >= static_cast<uint64_t>(file_size_)) {
                    STREAM_LOG("ERROR: Requested offset %llu is beyond file size %lld", offset, file_size_);
                    return false;
                }
                uint64_t end_req = offset + length;
                if (end_req > static_cast<uint64_t>(file_size_)) {
                    length = file_size_ - offset;
                    end_req = offset + length;
                }

                if (!file_ && !open_file()) {
                    STREAM_LOG("FATAL: could not open file %s", file_path_.c_str());
                    return false;
                }

                uint64_t current = offset;
                const uint64_t end = end_req;

                auto now_ms = []() {
                    return std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count();
                    };

                while (current < end && g_running.load(std::memory_order_acquire)) {
                    std::int64_t abs_current_offset = file_offset_ + static_cast<std::int64_t>(current);
                    int piece_idx = static_cast<int>(abs_current_offset / piece_size_);

                    int64_t deadline_ms = now_ms() + PIECE_DEADLINE_MS;

                    // Prefetch: establecer deadlines para piezas futuras y pasadas
                    {
                        std::lock_guard<std::mutex> lock(torrent_mutex_);
                        if (handle_.is_valid()) {
                            int last_piece = static_cast<int>((file_offset_ + file_size_ - 1) / piece_size_);
                            int first_piece = static_cast<int>(file_offset_ / piece_size_);
                            int max_ahead = std::min(piece_idx + PREFETCH_AHEAD, last_piece);
                            for (int p = piece_idx; p <= max_ahead; ++p) {
                                int64_t dl = now_ms() + PIECE_DEADLINE_MS + (p - piece_idx) * PIECE_DEADLINE_STEP_MS;
                                handle_.set_piece_deadline(lt::piece_index_t(p), dl);
                            }
                            int min_behind = std::max(piece_idx - PREFETCH_BEHIND, first_piece);
                            for (int p = min_behind; p < piece_idx; ++p) {
                                int64_t dl = now_ms() + PIECE_DEADLINE_MS + PIECE_DEADLINE_STEP_MS;
                                handle_.set_piece_deadline(lt::piece_index_t(p), dl);
                            }
                        }
                    }

                    // Esperar a que la pieza esté disponible (sin retener el mutex)
                    bool piece_available = false;
                    while (!piece_available && g_running.load(std::memory_order_acquire)) {
                        {
                            std::lock_guard<std::mutex> lock(torrent_mutex_);
                            if (handle_.is_valid()) {
                                lt::torrent_status st = handle_.status(lt::torrent_handle::query_pieces);
                                piece_available = (piece_idx >= 0 && piece_idx < static_cast<int>(st.pieces.size()))
                                    && st.pieces[lt::piece_index_t(piece_idx)];
                            }
                        }
                        if (piece_available) break;

                        if (now_ms() >= deadline_ms) {
                            STREAM_LOG("Timeout waiting for piece %d at offset %llu", piece_idx, current);
                            return false;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(PIECE_POLL_INTERVAL_MS));
                    }

                    if (piece_available) {
                        if (current != expected_seq_) {
                            auto do_seek = [this](uint64_t pos) -> bool {
#ifdef _WIN32
                                return _fseeki64(file_, static_cast<__int64>(pos), SEEK_SET) == 0;
#else
                                return fseeko(file_, static_cast<off_t>(pos), SEEK_SET) == 0;
#endif
                                };
                            if (!do_seek(current)) {
                                STREAM_LOG("ERROR: fseek failed at offset %llu, errno: %d", current, errno);
                                if (!open_file() || !do_seek(current)) {
                                    STREAM_LOG("FATAL: Re-fseek also failed at offset %llu", current);
                                    return false;
                                }
                            }
                            expected_seq_ = current;
                        }

                        size_t to_read = static_cast<size_t>(std::min<uint64_t>(end - current, read_buf_.size()));
                        size_t n = fread(read_buf_.data(), 1, to_read, file_);

                        if (n > 0) {
                            if (!sink.write(read_buf_.data(), n)) {
                                STREAM_LOG("Sink write failed at offset %llu", current);
                                return false;
                            }
                            current += n;
                            expected_seq_ = current;
                            continue;
                        }
                        else {
                            if (feof(file_)) {
                                STREAM_LOG("EOF hit unexpectedly at offset %llu during read of range %llu-%llu", current, offset, end);
                                return false;
                            }
                            else if (ferror(file_)) {
                                STREAM_LOG("ferror at offset %llu, errno: %d", current, errno);
                                clearerr(file_);
                                std::this_thread::sleep_for(std::chrono::milliseconds(25));
                                continue;
                            }
                            STREAM_LOG("fread returned 0 (not EOF/error) at offset %llu. Retrying...", current);
                            continue;
                        }
                    }
                }

                return true;
            }
            catch (const std::exception& e) {
                STREAM_LOG("Unexpected exception in TorrentContentProvider: %s", e.what());
                return false;
            }
            catch (...) {
                STREAM_LOG("Unknown exception in TorrentContentProvider.");
                return false;
            }
        }

    private:
        bool open_file() {
            if (file_) { fclose(file_); file_ = nullptr; }
#ifdef _WIN32
            int wlen = MultiByteToWideChar(CP_UTF8, 0, file_path_.c_str(), -1, nullptr, 0);
            if (wlen > 0) {
                std::wstring wpath(wlen, L'\0');
                MultiByteToWideChar(CP_UTF8, 0, file_path_.c_str(), -1, &wpath[0], wlen);
                file_ = _wfsopen(wpath.c_str(), L"rb", _SH_DENYNO);
            }
            else {
                STREAM_LOG("ERROR: Could not convert path to wide char.");
                return false;
            }
#else
            file_ = fopen(file_path_.c_str(), "rb");
#endif
            if (!file_) {
                STREAM_LOG("ERROR: fopen failed for %s, errno: %d", file_path_.c_str(), errno);
                return false;
            }
            STREAM_LOG("Opened file handle for %s", file_path_.c_str());
            return file_ != nullptr;
        }

        FILE* file_ = nullptr;
        lt::torrent_handle handle_;
        std::int64_t file_offset_;
        std::int64_t file_size_;
        std::int64_t piece_size_;
        std::mutex& torrent_mutex_;
        std::string file_path_;
        int file_index_;
        std::vector<char> read_buf_;
        uint64_t expected_seq_ = 0;
    };

    // ────────────────────────────────────
    // start_server / stop_server
    // ────────────────────────────────────
    bool start_server(lt::torrent_handle* handle_ptr, int32_t file_index,
        int32_t port, std::string& out_url)
    {
        std::lock_guard<std::mutex> state_lock(g_state_mutex);
        if (g_running.load(std::memory_order_acquire)) {
            g_last_error = "Server already running";
            return false;
        }
        if (!handle_ptr || !handle_ptr->is_valid()) {
            g_last_error = "Invalid torrent handle";
            return false;
        }

        lt::torrent_status st;
        {
            std::lock_guard<std::mutex> tor_lock(g_torrent_mutex);
            st = handle_ptr->status();
        }
        auto ti_ptr = st.torrent_file.lock();
        if (!ti_ptr) {
            g_last_error = "Torrent has no metadata yet";
            return false;
        }

        const auto& files = ti_ptr->files();
        if (file_index < 0 || file_index >= files.num_files()) {
            g_last_error = "File index out of range";
            return false;
        }

        lt::file_index_t fidx{ file_index };
        std::string base = st.save_path;
#ifdef _WIN32
        std::string rel = files.file_path(fidx);
        std::replace(rel.begin(), rel.end(), '/', '\\');
        g_file_path = base + "\\" + rel;
#else
        g_file_path = base + "/" + std::string(files.file_path(fidx));
#endif

        g_file_size = files.file_size(fidx);
        g_file_offset = files.file_offset(fidx);
        g_piece_size = ti_ptr->piece_length();
        g_mime_type = detect_mime(g_file_path);
        g_handle = *handle_ptr;
        g_file_index = file_index;

        {
            std::ostringstream etag_stream;
            etag_stream << "\"" << st.info_hashes.get_best() << "-" << file_index << "\"";
            g_etag = etag_stream.str();
        }

        // Establecer deadlines iniciales para las primeras piezas
        if (g_piece_size > 0 && g_file_size > 0) {
            int first = (int)(g_file_offset / g_piece_size);
            int last = (int)((g_file_offset + g_file_size - 1) / g_piece_size);
            auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            {
                std::lock_guard<std::mutex> tor_lock(g_torrent_mutex);
                for (int p = first; p <= std::min(first + PREFETCH_AHEAD, last); ++p) {
                    g_handle.piece_priority(lt::piece_index_t(p), lt::download_priority_t{ 7 });
                }
                for (int p = first; p <= std::min(first + PREFETCH_AHEAD, last); ++p) {
                    int64_t dl = now_ms + PIECE_DEADLINE_MS + (p - first) * PIECE_DEADLINE_STEP_MS;
                    g_handle.set_piece_deadline(lt::piece_index_t(p), dl);
                }
            }
        }

        g_server = std::make_unique<httplib::Server>();
        g_server->set_payload_max_length(512 * 1024 * 1024);
        g_server->set_keep_alive_max_count(1'000'000);    // ← CORREGIDO
        g_server->set_keep_alive_timeout(300);
        g_server->set_read_timeout(60);
        g_server->set_write_timeout(60);
        g_server->set_tcp_nodelay(true);

        auto add_cors = [](httplib::Response& res) {
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_header("Access-Control-Allow-Headers", "Range");
            res.set_header("Access-Control-Expose-Headers",
                "Content-Range, Content-Length, Accept-Ranges");
            };

        auto file_path = g_file_path;
        auto handle = g_handle;
        auto file_offset = g_file_offset;
        auto file_size = g_file_size;
        auto piece_size = g_piece_size;
        auto mime_type = g_mime_type;
        auto etag = g_etag;
        auto f_index = g_file_index;

        g_server->Get("/stream", [&, file_path, handle, file_offset, file_size,
            piece_size, mime_type, etag, f_index]
            (const httplib::Request&, httplib::Response& res) {
                try {
                    if (!std::filesystem::exists(file_path)) {
                        res.status = 503;
                        res.set_content("File not available yet", "text/plain");
                        return;
                    }
                    auto provider_ptr = std::make_shared<TorrentContentProvider>(
                        file_path, handle, file_offset, file_size, piece_size, g_torrent_mutex, f_index);

                    res.set_content_provider(
                        file_size,
                        mime_type,
                        [provider_ptr](uint64_t offset, uint64_t length, httplib::DataSink& sink) {
                            return (*provider_ptr)(offset, length, sink);
                        });

                    res.set_header("Accept-Ranges", "bytes");
                    res.set_header("ETag", etag);
                    res.set_header("Cache-Control", "no-cache");
                    add_cors(res);
                }
                catch (const std::exception& e) {
                    STREAM_LOG("Exception in /stream: %s", e.what());
                    res.status = 500;
                    res.set_content("Internal Server Error", "text/plain");
                }
                catch (...) {
                    STREAM_LOG("Unknown exception in /stream");
                    res.status = 500;
                    res.set_content("Internal Server Error", "text/plain");
                }
            });

        g_server->Get("/status", [&](const httplib::Request&, httplib::Response& res) {
            try {
                res.set_content(build_status_json(), "application/json");
                add_cors(res);
            }
            catch (const std::exception& e) {
                STREAM_LOG("Exception in /status: %s", e.what());
                res.status = 500;
                res.set_content(R"({"error":"Internal server error"})", "application/json");
            }
            catch (...) {
                STREAM_LOG("Unknown exception in /status");
                res.status = 500;
                res.set_content(R"({"error":"Internal server error"})", "application/json");
            }
            });

        g_server->Options(R"(.*)", [&](const httplib::Request&, httplib::Response& res) {
            add_cors(res);
            res.status = 204;
            });

        g_running.store(true, std::memory_order_release);
        g_last_error.clear();

        g_server_thread = std::thread([this_port = port]() {
            STREAM_LOG("Starting HTTP server on port %d", this_port);
            g_server->listen("127.0.0.1", this_port);
            STREAM_LOG("HTTP server stopped.");
            g_running.store(false, std::memory_order_release);
            });

        // Esperar hasta que el servidor esté realmente listo
        g_server->wait_until_ready();

        char url_buf[128];
        snprintf(url_buf, sizeof(url_buf), "http://127.0.0.1:%d/stream", port);
        {
            std::lock_guard<std::mutex> lock(g_url_mutex);
            g_url_buffer = url_buf;
            out_url = g_url_buffer;
        }
        STREAM_LOG("Server started successfully. URL: %s", out_url.c_str());
        return true;
    }

    void stop_server() {
        if (g_server && g_running.load(std::memory_order_acquire)) {
            STREAM_LOG("Stopping HTTP server...");
            g_server->stop();
        }
        if (g_server_thread.joinable()) {
            g_server_thread.join();
        }
        g_server.reset();

        {
            std::lock_guard<std::mutex> state_lock(g_state_mutex);
            if (g_handle.is_valid() && g_piece_size > 0 && g_file_size > 0) {
                int first = (int)(g_file_offset / g_piece_size);
                int last = (int)((g_file_offset + g_file_size - 1) / g_piece_size);
                std::lock_guard<std::mutex> tor_lock(g_torrent_mutex);
                for (int p = first; p <= last; ++p) {
                    g_handle.piece_priority(lt::piece_index_t(p), lt::default_priority);
                }
            }
        }
        g_running.store(false, std::memory_order_release);
    }

    bool is_running() { return g_running.load(std::memory_order_acquire); }

    const char* last_error() {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        return g_last_error.c_str();
    }

    void reset_state() {
        stop_server();
        std::lock_guard<std::mutex> lock(g_state_mutex);
        g_handle = lt::torrent_handle{};
        g_file_index = -1;
        g_file_path.clear();
        g_file_size = 0;
        g_file_offset = 0;
        g_piece_size = 0;
        g_mime_type.clear();
        g_etag.clear();
        g_last_error.clear();
    }

    // Dentro de namespace cs_stream

    bool cs_stream::is_byte_available(lt::torrent_handle* handle, int32_t file_index, int64_t byte_position) {
        if (!handle || !handle->is_valid()) return false;
        auto ti = handle->torrent_file();
        if (!ti) return false;

        if (file_index < 0 || file_index >= ti->files().num_files()) return false;
        lt::file_index_t fidx{ file_index };

        int64_t absolute_pos = ti->files().file_offset(fidx) + byte_position;
        int piece_idx = static_cast<int>(absolute_pos / ti->piece_length());

        auto st = handle->status(lt::torrent_handle::query_pieces);
        if (piece_idx < 0 || piece_idx >= static_cast<int>(st.pieces.size())) return false;

        return st.pieces[lt::piece_index_t(piece_idx)];
    }

    bool cs_stream::prioritize_seek_range(lt::torrent_handle* handle, int32_t file_index, int64_t byte_position, int64_t piece_size) {
        if (!handle || !handle->is_valid() || piece_size <= 0) return false;

        auto ti = handle->torrent_file();
        if (!ti) return false;

        if (file_index < 0 || file_index >= ti->files().num_files()) return false;
        lt::file_index_t fidx{ file_index };

        int64_t absolute_pos = ti->files().file_offset(fidx) + byte_position;
        int piece_idx = static_cast<int>(absolute_pos / piece_size);

        auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();

        handle->set_piece_deadline(lt::piece_index_t(piece_idx), now + 1500);

        int last_piece = static_cast<int>((ti->files().file_offset(fidx) + ti->files().file_size(fidx) - 1) / piece_size);
        for (int p = piece_idx + 1; p <= std::min(piece_idx + 2, last_piece); ++p) {
            handle->set_piece_deadline(lt::piece_index_t(p), now + 2000 + (p - piece_idx) * 2000);
        }

        return true;
    }

}  // namespace cs_stream

extern "C" {

    const char* start_stream_server_impl(lt::torrent_handle* handle,
        int32_t file_index,
        int32_t port)
    {
        static std::string url_result;
        static std::mutex result_mutex;
        {
            std::lock_guard<std::mutex> lock(result_mutex);
            if (cs_stream::start_server(handle, file_index, port, url_result))
                return url_result.c_str();
        }
        return nullptr;
    }

    void stop_stream_server_impl() { cs_stream::stop_server(); }
    uint8_t is_stream_server_running_impl() { return cs_stream::is_running() ? 1 : 0; }
    void reset_stream_server_impl() { cs_stream::reset_state(); }
    const char* get_last_stream_error_impl() { return cs_stream::last_error(); }
   
}  // extern "C"