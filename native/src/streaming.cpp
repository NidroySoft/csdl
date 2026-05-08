// streaming.cpp – Servidor de streaming profesional con libtorrent 2.0 y cpp-httplib
// Versión final: precarga mínima (primera y última pieza), readahead dinámico,
// priorización reactiva en cada Range, timeouts desactivados, caché LRU en RAM.

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
#include <unordered_map>
#include <list>
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
#  define STREAM_LOG(fmt, ...) fprintf(stderr, "[CSDL-STREAM] " fmt "\n", ##__VA_ARGS__)
#else
#  define STREAM_LOG(...) ((void)0)
#endif

namespace cs_stream {

    // ── Constantes calibradas ──────────────────────────────────────────
    constexpr int PIECE_DEADLINE_CURRENT_MS = 1500;    // pieza actual
    constexpr int PIECE_DEADLINE_STEP_MS = 2000;    // incremento por pieza de readahead
    constexpr int PIECE_DEADLINE_MAX_MS = 8000;    // plazo máximo absoluto
    constexpr int PIECE_POLL_INTERVAL_MS = 50;      // intervalo de sondeo
    constexpr int DEADLINE_UPDATE_INTERVAL_MS = 500;   // refresco de deadlines
    constexpr size_t CACHE_MAX_MB = 64;
    constexpr size_t CACHE_MAX_BYTES = CACHE_MAX_MB * 1024 * 1024;
    constexpr int READAHEAD_PERCENT = 25;      // % de la caché para read‑ahead
    constexpr size_t READAHEAD_BYTES = CACHE_MAX_BYTES * READAHEAD_PERCENT / 100;
    constexpr int PRECARGA_TIMEOUT_SEC = 30;      // tiempo máximo esperando primera y última pieza

    // ── Estado global ──────────────────────────────────────────────────
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

    static std::unique_ptr<httplib::Server> g_server;
    static std::thread           g_server_thread;

    static std::string           g_url_buffer;
    static std::mutex            g_url_mutex;

    // ── Caché LRU en RAM ──────────────────────────────────────────────
    static std::mutex g_cache_mutex;
    struct CacheEntry {
        int piece_index;
        std::shared_ptr<std::vector<char>> data;
    };
    static std::list<CacheEntry> g_cache_list;
    static std::unordered_map<int, decltype(g_cache_list)::iterator> g_cache_map;
    static size_t g_cache_current_size = 0;

    static void cache_put(int piece_idx, std::shared_ptr<std::vector<char>> data) {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        auto it = g_cache_map.find(piece_idx);
        if (it != g_cache_map.end()) {
            g_cache_current_size -= it->second->data->size();
            g_cache_list.erase(it->second);
            g_cache_map.erase(it);
        }
        g_cache_list.push_front({ piece_idx, data });
        g_cache_map[piece_idx] = g_cache_list.begin();
        g_cache_current_size += data->size();
        while (g_cache_current_size > CACHE_MAX_BYTES && !g_cache_list.empty()) {
            auto last = std::prev(g_cache_list.end());
            g_cache_current_size -= last->data->size();
            g_cache_map.erase(last->piece_index);
            g_cache_list.pop_back();
        }
    }

    static std::shared_ptr<std::vector<char>> cache_get(int piece_idx) {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        auto it = g_cache_map.find(piece_idx);
        if (it == g_cache_map.end()) return nullptr;
        g_cache_list.splice(g_cache_list.begin(), g_cache_list, it->second);
        return it->second->data;
    }

    static void cache_clear() {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        g_cache_list.clear();
        g_cache_map.clear();
        g_cache_current_size = 0;
    }

    // ── Helpers ───────────────────────────────────────────────────────
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
                    o << "\\u00" << std::hex << static_cast<int>(static_cast<unsigned char>(c)) << std::dec;
                else o << c;
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
        if (lower.ends_with(".mpg") || lower.ends_with(".mpeg")) return "video/mpeg";
        return "application/octet-stream";
    }

    static std::string build_status_json() {
        try {
            lt::torrent_handle h;
            int f_idx, p_size;
            std::int64_t f_off, f_sz;
            {
                std::lock_guard<std::mutex> lock(g_state_mutex);
                if (!g_handle.is_valid()) return R"({"error":"no handle"})";
                h = g_handle; f_idx = g_file_index; p_size = g_piece_size;
                f_off = g_file_offset; f_sz = g_file_size;
            }
            lt::torrent_status st = h.status();
            auto ti = st.torrent_file.lock();
            if (!ti) return R"({"error":"no metadata"})";

            std::ostringstream j;
            j << "{\n  \"torrent\": {\n"
                << "    \"progress\": " << st.progress << ",\n"
                << "    \"download_rate\": " << st.download_payload_rate << ",\n"
                << "    \"upload_rate\": " << st.upload_payload_rate << ",\n"
                << "    \"num_peers\": " << st.num_peers << ",\n"
                << "    \"num_seeds\": " << st.num_seeds << ",\n"
                << "    \"total_downloaded\": " << st.total_payload_download << ",\n"
                << "    \"is_finished\": " << (st.is_finished ? "true" : "false") << "\n  }";

            if (f_idx >= 0 && f_idx < ti->files().num_files()) {
                const auto& files = ti->files();
                lt::file_index_t fidx{ f_idx };
                std::vector<std::int64_t> fprog; h.file_progress(fprog);
                std::int64_t file_down = (f_idx < (int)fprog.size()) ? fprog[f_idx] : 0;

                int first_piece = static_cast<int>(files.file_offset(fidx) / p_size);
                int last_piece = static_cast<int>((files.file_offset(fidx) + f_sz - 1) / p_size);
                int dl_pieces = 0, first_missing = -1;
                const auto& pieces = st.pieces;
                for (int i = first_piece; i <= last_piece; ++i) {
                    bool have = (i >= 0 && i < (int)pieces.size()) && pieces[lt::piece_index_t(i)];
                    if (have) dl_pieces++;
                    else if (first_missing < 0) first_missing = i;
                }
                int first_missing_rel = (first_missing >= 0) ? (first_missing - first_piece) : -1;
                std::int64_t contiguous = (first_missing >= 0)
                    ? std::min((std::int64_t)(first_missing - first_piece) * p_size, f_sz) : f_sz;

                j << ",\n  \"file\": {\n"
                    << "    \"index\": " << f_idx << ",\n"
                    << "    \"name\": \"" << escape_json(std::string(files.file_name(fidx))) << "\",\n"
                    << "    \"size\": " << f_sz << ",\n"
                    << "    \"offset_in_torrent\": " << f_off << ",\n"
                    << "    \"downloaded_bytes\": " << file_down << ",\n"
                    << "    \"progress\": " << (f_sz > 0 ? (double)file_down / f_sz : 0.0) << ",\n"
                    << "    \"total_pieces\": " << (last_piece - first_piece + 1) << ",\n"
                    << "    \"downloaded_pieces\": " << dl_pieces << ",\n"
                    << "    \"first_missing_piece\": " << first_missing_rel << ",\n"
                    << "    \"contiguous_completed_bytes\": " << contiguous << "\n  }";
            }
            j << "\n}";
            return j.str();
        }
        catch (...) { return R"({"error":"internal error"})"; }
    }

    // ═══════════════════════════════════════════════════════════════════
    // TorrentContentProvider – Versión profesional con priorización por Range
    // ═══════════════════════════════════════════════════════════════════
    class TorrentContentProvider {
    public:
        TorrentContentProvider(const std::string& path,
            lt::torrent_handle handle,
            std::int64_t file_offset,
            std::int64_t file_size,
            std::int64_t piece_size,
            int file_index)
            : handle_(std::move(handle)),
            file_offset_(file_offset),
            file_size_(file_size),
            piece_size_(piece_size),
            file_path_(path),
            file_index_(file_index)
        {
            if (piece_size_ > 0) {
                int64_t total_pieces = (file_size_ + piece_size_ - 1) / piece_size_;
                int64_t ahead_bytes = READAHEAD_BYTES;
                readahead_pieces_ = std::max<int64_t>(1, ahead_bytes / piece_size_);
                if (readahead_pieces_ > total_pieces) readahead_pieces_ = static_cast<int>(total_pieces);
            }
            else {
                readahead_pieces_ = 1;
            }
            last_deadline_update_ = std::chrono::steady_clock::now() - std::chrono::milliseconds(DEADLINE_UPDATE_INTERVAL_MS * 2);
            last_range_start_ = 0;
        }

        ~TorrentContentProvider() { close_file(); }

        bool operator()(uint64_t offset, uint64_t length, httplib::DataSink& sink) {
            try {
                if (offset >= static_cast<uint64_t>(file_size_)) return false;
                uint64_t end_req = std::min(offset + length, static_cast<uint64_t>(file_size_));

                // Priorizar agresivamente al inicio de un nuevo rango (seek)
                if (offset != last_range_start_) {
                    last_range_start_ = offset;
                    int start_piece = static_cast<int>((file_offset_ + offset) / piece_size_);
                    prioritize_seek_range(start_piece);
                }

                uint64_t current = offset;

                while (current < end_req && g_running.load(std::memory_order_acquire)) {
                    std::int64_t abs_pos = file_offset_ + static_cast<std::int64_t>(current);
                    int piece_idx = static_cast<int>(abs_pos / piece_size_);

                    update_deadlines_if_needed(piece_idx);

                    // 1. Intentar servir desde caché RAM
                    if (size_t n = serve_from_cache(piece_idx, current, end_req, sink)) {
                        current += n;
                        continue;
                    }

                    // 2. Si la pieza ya está descargada → cargarla en caché
                    if (is_piece_downloaded(piece_idx)) {
                        if (load_piece_into_cache(piece_idx)) {
                            continue;  // reintentar servir desde caché
                        }
                    }

                    // 3. Pieza no disponible → deadline con alert_when_available y espera
                    handle_.set_piece_deadline(lt::piece_index_t(piece_idx),
                        now_ms() + 3000,
                        lt::torrent_handle::alert_when_available);
                    std::this_thread::sleep_for(std::chrono::milliseconds(PIECE_POLL_INTERVAL_MS));
                }
                return true;
            }
            catch (const std::exception& e) {
                STREAM_LOG("Exception in ContentProvider: %s", e.what());
                return false;
            }
            catch (...) {
                STREAM_LOG("Unknown exception in ContentProvider");
                return false;
            }
        }

    private:
        int64_t now_ms() const {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        void update_deadlines_if_needed(int piece_idx) {
            auto now = std::chrono::steady_clock::now();
            {
                std::lock_guard<std::mutex> lock(provider_mutex_);
                if (piece_idx == last_piece_idx_ &&
                    now - last_deadline_update_ < std::chrono::milliseconds(DEADLINE_UPDATE_INTERVAL_MS))
                    return;
            }

            if (!handle_.is_valid()) return;

            auto ms = now_ms();
            int first = static_cast<int>(file_offset_ / piece_size_);
            int last = static_cast<int>((file_offset_ + file_size_ - 1) / piece_size_);

            // Pieza actual
            handle_.set_piece_deadline(lt::piece_index_t(piece_idx), ms + PIECE_DEADLINE_CURRENT_MS,
                lt::torrent_handle::alert_when_available);

            // Readahead hacia delante
            int max_ahead = std::min(piece_idx + readahead_pieces_, last);
            for (int p = piece_idx + 1; p <= max_ahead; ++p) {
                int64_t dl = ms + PIECE_DEADLINE_CURRENT_MS + (p - piece_idx) * PIECE_DEADLINE_STEP_MS;
                if (dl > ms + PIECE_DEADLINE_MAX_MS) dl = ms + PIECE_DEADLINE_MAX_MS;
                handle_.set_piece_deadline(lt::piece_index_t(p), dl, lt::torrent_handle::alert_when_available);
            }

            // Un par de piezas hacia atrás (por seek)
            for (int p = std::max(first, piece_idx - 2); p < piece_idx; ++p) {
                handle_.set_piece_deadline(lt::piece_index_t(p), ms + 4000,
                    lt::torrent_handle::alert_when_available);
            }

            // Prioridades altas para la ventana
            for (int p = piece_idx; p <= max_ahead && p >= first; ++p) {
                handle_.piece_priority(lt::piece_index_t(p), lt::download_priority_t{ 7 });
            }

            {
                std::lock_guard<std::mutex> lock(provider_mutex_);
                last_piece_idx_ = piece_idx;
                last_deadline_update_ = now;
            }
        }

        void prioritize_seek_range(int piece_idx) {
            if (!handle_.is_valid()) return;
            auto ms = now_ms();
            int first = static_cast<int>(file_offset_ / piece_size_);
            int last = static_cast<int>((file_offset_ + file_size_ - 1) / piece_size_);

            // Pieza objetivo con prioridad máxima y deadline muy corto
            handle_.set_piece_deadline(lt::piece_index_t(piece_idx), ms + 500,
                lt::torrent_handle::alert_when_available);
            handle_.piece_priority(lt::piece_index_t(piece_idx), lt::download_priority_t{ 7 });

            // Las 2 siguientes piezas con prioridad alta y plazos progresivos
            for (int i = 1; i <= 2; ++i) {
                int p = piece_idx + i;
                if (p <= last) {
                    handle_.set_piece_deadline(lt::piece_index_t(p), ms + 1000 + i * 500,
                        lt::torrent_handle::alert_when_available);
                    handle_.piece_priority(lt::piece_index_t(p), lt::download_priority_t{ 7 });
                }
            }
        }

        bool is_piece_downloaded(int piece_idx) {
            if (!handle_.is_valid()) return false;
            lt::torrent_status st = handle_.status(lt::torrent_handle::query_pieces);
            return piece_idx >= 0 && piece_idx < static_cast<int>(st.pieces.size()) &&
                st.pieces[lt::piece_index_t(piece_idx)];
        }

        size_t serve_from_cache(int piece_idx, uint64_t current, uint64_t end, httplib::DataSink& sink) {
            auto data = cache_get(piece_idx);
            if (!data) return 0;
            int64_t piece_start = static_cast<int64_t>(piece_idx) * piece_size_;
            uint64_t off = current - piece_start;
            if (off >= data->size()) return 0;
            uint64_t to_copy = std::min<uint64_t>(end - current, data->size() - off);
            if (to_copy == 0) return 0;
            if (!sink.write(&(*data)[off], static_cast<size_t>(to_copy))) return 0;
            return static_cast<size_t>(to_copy);
        }

        bool load_piece_into_cache(int piece_idx) {
            if (cache_get(piece_idx)) return true;   // ya está en caché
            if (!open_file_if_needed()) return false;

            int64_t piece_start = static_cast<int64_t>(piece_idx) * piece_size_;
            int64_t piece_end = std::min(file_offset_ + file_size_, piece_start + piece_size_);
            int64_t piece_length = piece_end - piece_start;
            if (piece_length <= 0) return false;

            // Reintentos ante fallos de lectura (archivos sparse)
            for (int attempt = 0; attempt < 3; ++attempt) {
                auto buffer = std::make_shared<std::vector<char>>(piece_length);
                if (!seek_file(piece_start)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    continue;
                }
                size_t n = fread(buffer->data(), 1, piece_length, file_);
                if (n == static_cast<size_t>(piece_length)) {
                    cache_put(piece_idx, buffer);
                    return true;
                }
                clearerr(file_);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            return false;
        }

        bool open_file_if_needed() {
            if (!file_) return open_file();
            return true;
        }

        bool open_file() {
            close_file();
#ifdef _WIN32
            int wlen = MultiByteToWideChar(CP_UTF8, 0, file_path_.c_str(), -1, nullptr, 0);
            if (wlen > 0) {
                std::wstring wpath(wlen, L'\0');
                MultiByteToWideChar(CP_UTF8, 0, file_path_.c_str(), -1, &wpath[0], wlen);
                file_ = _wfsopen(wpath.c_str(), L"rb", _SH_DENYNO);
            }
            else {
                return false;
            }
#else
            file_ = fopen(file_path_.c_str(), "rb");
#endif
            return file_ != nullptr;
        }

        void close_file() {
            if (file_) { fclose(file_); file_ = nullptr; }
        }

        bool seek_file(int64_t pos) {
#ifdef _WIN32
            return _fseeki64(file_, static_cast<__int64>(pos), SEEK_SET) == 0;
#else
            return fseeko(file_, static_cast<off_t>(pos), SEEK_SET) == 0;
#endif
        }

        lt::torrent_handle handle_;
        std::int64_t file_offset_, file_size_, piece_size_;
        std::string file_path_;
        int file_index_;
        int readahead_pieces_ = 1;
        int last_piece_idx_ = -1;
        std::chrono::steady_clock::time_point last_deadline_update_;
        std::mutex provider_mutex_;
        FILE* file_ = nullptr;
        uint64_t last_range_start_ = 0;   // inicio del último rango solicitado
    };

    // ── Funciones de seek ─────────────────────────────────────────────
    bool is_byte_available(lt::torrent_handle* handle, int32_t file_index, int64_t byte_position) {
        if (!handle || !handle->is_valid()) return false;
        try {
            auto ti = handle->torrent_file();
            if (!ti || file_index < 0 || file_index >= ti->files().num_files()) return false;
            auto abs = ti->files().file_offset(lt::file_index_t(file_index)) + byte_position;
            int pi = static_cast<int>(abs / ti->piece_length());
            auto st = handle->status(lt::torrent_handle::query_pieces);
            return pi >= 0 && pi < (int)st.pieces.size() && st.pieces[lt::piece_index_t(pi)];
        }
        catch (...) { return false; }
    }

    bool prioritize_seek_range(lt::torrent_handle* handle, int32_t file_index, int64_t byte_position, int64_t piece_size) {
        if (!handle || !handle->is_valid() || piece_size <= 0) return false;
        try {
            auto ti = handle->torrent_file();
            if (!ti || file_index < 0 || file_index >= ti->files().num_files()) return false;
            auto abs = ti->files().file_offset(lt::file_index_t(file_index)) + byte_position;
            int pi = static_cast<int>(abs / piece_size);
            auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            handle->set_piece_deadline(lt::piece_index_t(pi), now + 1500);
            int last = static_cast<int>((ti->files().file_offset(lt::file_index_t(file_index)) + ti->files().file_size(lt::file_index_t(file_index)) - 1) / piece_size);
            for (int p = pi + 1; p <= std::min(pi + 2, last); ++p) {
                handle->set_piece_deadline(lt::piece_index_t(p), now + 3000 + (p - pi) * 2000);
            }
            return true;
        }
        catch (...) { return false; }
    }

    // ── start_server con precarga mínima (primera y última pieza) ─────
    bool start_server(lt::torrent_handle* handle_ptr, int32_t file_index,
        int32_t port, std::string& out_url) {
        try {
            if (g_running.load(std::memory_order_acquire)) {
                g_last_error = "Server already running";
                return false;
            }
            if (!handle_ptr || !handle_ptr->is_valid()) {
                g_last_error = "Invalid torrent handle";
                return false;
            }

            lt::torrent_handle h = *handle_ptr;
            lt::torrent_status st = h.status();
            auto ti = st.torrent_file.lock();
            if (!ti) { g_last_error = "Torrent has no metadata yet"; return false; }
            const auto& files = ti->files();
            if (file_index < 0 || file_index >= files.num_files()) {
                g_last_error = "File index out of range";
                return false;
            }

            lt::file_index_t fidx{ file_index };
            std::filesystem::path save_path(st.save_path);
            std::string full_path = (save_path / files.file_path(fidx)).string();
            int64_t fsize = files.file_size(fidx);
            int64_t foff = files.file_offset(fidx);
            int psize = ti->piece_length();
            std::string mime = detect_mime(full_path);

            {
                std::lock_guard<std::mutex> lock(g_state_mutex);
                g_handle = h; g_file_index = file_index; g_file_path = full_path;
                g_file_size = fsize; g_file_offset = foff; g_piece_size = psize;
                g_mime_type = mime;
                std::ostringstream etag; etag << "\"" << st.info_hashes.get_best() << "-" << file_index << "\"";
                g_etag = etag.str(); g_last_error.clear();
            }

            // ── Prioridades iniciales ─────────────────────────────────
            if (psize > 0 && fsize > 0) {
                int first = static_cast<int>(foff / psize);
                int last = static_cast<int>((foff + fsize - 1) / psize);
                auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();

                // Principio: read‑ahead dinámico
                int ahead_pieces = std::max(1, static_cast<int>(READAHEAD_BYTES / psize));
                int max_ahead = std::min(first + ahead_pieces - 1, last);
                for (int p = first; p <= max_ahead; ++p) {
                    h.piece_priority(lt::piece_index_t(p), lt::download_priority_t{ 7 });
                    h.set_piece_deadline(lt::piece_index_t(p), now + PIECE_DEADLINE_CURRENT_MS + (p - first) * PIECE_DEADLINE_STEP_MS,
                        lt::torrent_handle::alert_when_available);
                }

                // Final: últimas 3 piezas (metadatos)
                for (int i = 0; i < 3 && (last - i) >= first; ++i) {
                    int p = last - i;
                    h.piece_priority(lt::piece_index_t(p), lt::download_priority_t{ 7 });
                    h.set_piece_deadline(lt::piece_index_t(p), now + 5000 + i * 1000,
                        lt::torrent_handle::alert_when_available);
                }

                // ═══════════════════════════════════════════════════════
                //  PRECARGA MÍNIMA: espera solo la primera y última pieza
                // ═══════════════════════════════════════════════════════
                auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(PRECARGA_TIMEOUT_SEC);
                auto check_ready = [&]() -> bool {
                    lt::torrent_status st = h.status(lt::torrent_handle::query_pieces);
                    bool first_ok = (first >= 0 && first < (int)st.pieces.size() && st.pieces[lt::piece_index_t(first)]);
                    bool last_ok = (last >= 0 && last < (int)st.pieces.size() && st.pieces[lt::piece_index_t(last)]);
                    return first_ok && last_ok;
                    };

                STREAM_LOG("Esperando que la primera y última pieza estén descargadas...");
                while (std::chrono::steady_clock::now() < deadline && !check_ready()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                }
            }

            // ── Configurar servidor HTTP ─────────────────────────────
            g_server = std::make_unique<httplib::Server>();
            g_server->set_exception_handler([](const httplib::Request&, httplib::Response& res, std::exception_ptr) {
                res.status = 500; res.set_content("Internal Server Error", "text/plain");
                });
            g_server->set_payload_max_length(512 * 1024 * 1024);
            g_server->set_keep_alive_max_count(1'000'000);
            g_server->set_keep_alive_timeout(300);
            g_server->set_read_timeout(0, 0);     // sin timeout
            g_server->set_write_timeout(0, 0);    // sin timeout
            g_server->set_tcp_nodelay(true);

            auto add_cors = [](httplib::Response& res) {
                res.set_header("Access-Control-Allow-Origin", "*");
                res.set_header("Access-Control-Allow-Headers", "Range");
                res.set_header("Access-Control-Expose-Headers", "Content-Range, Content-Length, Accept-Ranges");
                };

            // Copia segura de variables para las lambdas
            auto file_path = g_file_path;
            auto mime_val = g_mime_type;
            auto etag_val = g_etag;
            auto handle = g_handle;
            int f_idx = g_file_index;
            std::int64_t f_off = g_file_offset, f_sz = g_file_size, p_sz = g_piece_size;

            g_server->Get("/stream", [file_path, handle, f_off, f_sz, p_sz, mime_val, etag_val, f_idx, add_cors]
            (const httplib::Request&, httplib::Response& res) {
                    try {
                        if (!std::filesystem::exists(file_path)) {
                            res.status = 503;
                            res.set_content("File not available yet", "text/plain");
                            return;
                        }
                        auto p = std::make_shared<TorrentContentProvider>(file_path, handle, f_off, f_sz, p_sz, f_idx);
                        res.set_content_provider(f_sz, mime_val, [p](uint64_t off, uint64_t len, httplib::DataSink& sink) {
                            return (*p)(off, len, sink);
                            });
                        res.set_header("Accept-Ranges", "bytes");
                        res.set_header("ETag", etag_val);
                        res.set_header("Cache-Control", "no-cache");
                        add_cors(res);
                    }
                    catch (...) {
                        res.status = 500; res.set_content("Internal Server Error", "text/plain");
                    }
                });

            g_server->Get("/status", [add_cors](const httplib::Request&, httplib::Response& res) {
                try {
                    res.set_content(build_status_json(), "application/json");
                    add_cors(res);
                }
                catch (...) {
                    res.status = 500; res.set_content(R"({"error":"internal error"})", "application/json");
                }
                });

            g_server->Options(R"(.*)", [add_cors](const httplib::Request&, httplib::Response& res) {
                add_cors(res);
                res.status = 204;
                });

            g_running.store(true, std::memory_order_release);
            g_server_thread = std::thread([this_port = port]() {
                try { g_server->listen("127.0.0.1", this_port); }
                catch (...) {}
                g_running.store(false, std::memory_order_release);
                });

            // Esperar inicio del servidor con timeout
            auto begin = std::chrono::steady_clock::now();
            while (!g_server->is_running() && g_running.load(std::memory_order_acquire)) {
                if (std::chrono::steady_clock::now() - begin > std::chrono::seconds(4)) {
                    g_server->stop();
                    if (g_server_thread.joinable()) g_server_thread.join();
                    g_server.reset();
                    g_running.store(false, std::memory_order_release);
                    g_last_error = "Server start timed out";
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            char buf[128];
            snprintf(buf, sizeof(buf), "http://127.0.0.1:%d/stream", port);
            {
                std::lock_guard<std::mutex> lock(g_url_mutex);
                g_url_buffer = buf;
                out_url = g_url_buffer;
            }
            return true;
        }
        catch (const std::exception& e) {
            g_last_error = std::string("start_server: ") + e.what();
            return false;
        }
        catch (...) {
            g_last_error = "start_server unknown exception";
            return false;
        }
    }

    void stop_server() {
        try {
            if (g_server && g_running.load(std::memory_order_acquire)) {
                g_server->stop();
            }
            if (g_server_thread.joinable()) g_server_thread.join();
            g_server.reset();

            // Restaurar prioridades
            lt::torrent_handle h;
            int psize, f_idx;
            std::int64_t foff, fsz;
            {
                std::lock_guard<std::mutex> lock(g_state_mutex);
                h = g_handle;
                psize = g_piece_size;
                f_idx = g_file_index;
                foff = g_file_offset;
                fsz = g_file_size;
            }
            if (h.is_valid() && psize > 0) {
                int first = static_cast<int>(foff / psize);
                int last = static_cast<int>((foff + fsz - 1) / psize);
                for (int p = first; p <= last; ++p) {
                    h.piece_priority(lt::piece_index_t(p), lt::default_priority);
                }
            }

            g_running.store(false, std::memory_order_release);
            cache_clear();
        }
        catch (...) {
            STREAM_LOG("Exception in stop_server");
        }
    }

    bool is_running() {
        return g_running.load(std::memory_order_acquire);
    }

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

} // namespace cs_stream

extern "C" {
    const char* start_stream_server_impl(lt::torrent_handle* handle, int32_t file_index, int32_t port) {
        static std::string url_result;
        static std::mutex result_mutex;
        std::lock_guard<std::mutex> lock(result_mutex);
        if (cs_stream::start_server(handle, file_index, port, url_result))
            return url_result.c_str();
        return nullptr;
    }

    void stop_stream_server_impl() { cs_stream::stop_server(); }
    uint8_t is_stream_server_running_impl() { return cs_stream::is_running() ? 1 : 0; }
    void reset_stream_server_impl() { cs_stream::reset_state(); }
    const char* get_last_stream_error_impl() { return cs_stream::last_error(); }
}