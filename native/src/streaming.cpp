// streaming.cpp – RAM-first streaming server using libtorrent 2.0 and cpp-httplib
//
// Multiplatform: Windows, Linux, macOS
// Requires CMake to link pthread (Linux) and ws2_32 (Windows).

#include "streaming.h"

#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/torrent_status.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/file_storage.hpp>
#include <libtorrent/download_priority.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/session.hpp>

#include <atomic>
#include <thread>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
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
#include <set>
#include <functional>
#include "lib_export.h"

// httplib requires these defines on Windows before inclusion
#ifdef _WIN32
#  ifndef _WIN32_WINNT
#    define _WIN32_WINNT 0x0A00   // Windows 10+
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#endif

#define CPPHTTPLIB_OPENSSL_SUPPORT 0
#include <httplib.h>

#ifdef _DEBUG
#  define STREAM_LOG(fmt, ...) fprintf(stderr, "[CSDL-STREAM] " fmt "\n", ##__VA_ARGS__)
#else
#  define STREAM_LOG(...) ((void)0)
#endif

namespace cs_stream {

    // ════════════════════════════════════════════════════════════════════════
    // Default constants (used when configuration is not provided)
    // ════════════════════════════════════════════════════════════════════════
    constexpr int DEFAULT_CACHE_LIMIT_MB = 512;
    constexpr int DEFAULT_MIN_AHEAD = 24;
    constexpr int DEFAULT_MAX_AHEAD = 192;
    constexpr int DEFAULT_BACK_WINDOW = 6;
    constexpr int DEFAULT_DEADLINE_BASE_MS = 1200;
    constexpr int DEFAULT_DEADLINE_STEP_MS = 800;
    constexpr int DEFAULT_READ_TIMEOUT_MS = 15000;
    constexpr int DEFAULT_ALERT_PUMP_INTERVAL_MS = 15;
    constexpr int DEFAULT_WINDOW_UPDATE_THROTTLE_MS = 250;
    constexpr int64_t DEFAULT_READAHEAD_BYTES = 32LL * 1024 * 1024;
    constexpr int DEFAULT_TAIL_PIECES = 3;
    constexpr int DEFAULT_PIECE_POLL_INTERVAL_MS = 50;
    constexpr int DEFAULT_PIECE_POLL_MAX_ATTEMPTS = 300;
    constexpr int DEFAULT_ANCHOR_PIECE_POLL_MAX_ATTEMPTS = 1200;
    constexpr int DEFAULT_ENSURE_PIECE_MAX_RETRIES = 3;
    constexpr int DEFAULT_DEADLINE_REEMIT_INTERVAL_MS = 2000;
    constexpr int DEFAULT_STARTUP_BUFFER_PIECES = 12;
    constexpr bool DEFAULT_ENABLE_TAIL_PREFETCH = true;

    // ════════════════════════════════════════════════════════════════════════
    // Runtime configuration (set before start_server, thread-safe read)
    // ════════════════════════════════════════════════════════════════════════
    struct StreamingConfig {
        int cache_limit_mb = DEFAULT_CACHE_LIMIT_MB;
        int min_readahead = DEFAULT_MIN_AHEAD;
        int max_readahead = DEFAULT_MAX_AHEAD;
        int back_window = DEFAULT_BACK_WINDOW;
        int deadline_base_ms = DEFAULT_DEADLINE_BASE_MS;
        int deadline_step_ms = DEFAULT_DEADLINE_STEP_MS;
        int window_update_throttle_ms = DEFAULT_WINDOW_UPDATE_THROTTLE_MS;
        int piece_poll_interval_ms = DEFAULT_PIECE_POLL_INTERVAL_MS;
        int piece_poll_max_attempts = DEFAULT_PIECE_POLL_MAX_ATTEMPTS;
        int anchor_piece_poll_max_attempts = DEFAULT_ANCHOR_PIECE_POLL_MAX_ATTEMPTS;
        int ensure_piece_max_retries = DEFAULT_ENSURE_PIECE_MAX_RETRIES;
        int deadline_reemit_interval_ms = DEFAULT_DEADLINE_REEMIT_INTERVAL_MS;
        int startup_buffer_pieces = DEFAULT_STARTUP_BUFFER_PIECES;
        int tail_pieces = DEFAULT_TAIL_PIECES;
        bool enable_tail_prefetch = DEFAULT_ENABLE_TAIL_PREFETCH;
    };

    static StreamingConfig g_config;              // default-init
    static std::mutex g_config_mutex;

    // Public function to update configuration (must be called before start_server)
    void configure_streaming(const StreamingConfig& cfg) {
        std::lock_guard<std::mutex> lk(g_config_mutex);
        g_config = cfg;
    }

    // Thread-safe copy of the current configuration
    static StreamingConfig get_config() {
        std::lock_guard<std::mutex> lk(g_config_mutex);
        return g_config;
    }

    // ════════════════════════════════════════════════════════════════════════
    // StreamState — explicit streaming lifecycle
    // ════════════════════════════════════════════════════════════════════════
    enum class StreamState : int {
        Bootstrap,  // prebuffering front + tail in parallel
        Streaming   // sliding window mode
    };

    // ════════════════════════════════════════════════════════════════════════
    // PieceCache — LRU cache of fully downloaded pieces in RAM
    // ════════════════════════════════════════════════════════════════════════
    class PieceCache {
    public:
        struct Entry {
            int piece;
            std::shared_ptr<std::vector<char>> data;
        };

        explicit PieceCache(int limit_mb) : limit_bytes_(static_cast<size_t>(limit_mb) * 1024 * 1024) {}

        std::shared_ptr<std::vector<char>> get(int piece) {
            std::lock_guard<std::mutex> lk(mu);
            auto it = map.find(piece);
            if (it == map.end()) return nullptr;
            lru.splice(lru.begin(), lru, it->second);
            return it->second->data;
        }

        void put(int piece, std::shared_ptr<std::vector<char>> data) {
            std::lock_guard<std::mutex> lk(mu);
            if (map.count(piece)) {
                bytes -= map[piece]->data->size();
                lru.erase(map[piece]);
                map.erase(piece);
            }
            lru.push_front({ piece, std::move(data) });
            map[piece] = lru.begin();
            bytes += lru.front().data->size();
            trim();
        }

        bool contains(int piece) const {
            std::lock_guard<std::mutex> lk(mu);
            return map.find(piece) != map.end();
        }

    private:
        void trim() {
            while (bytes > limit_bytes_ && !lru.empty()) {
                auto& back = lru.back();
                bytes -= back.data->size();
                map.erase(back.piece);
                lru.pop_back();
            }
        }

        mutable std::mutex mu;
        std::list<Entry> lru;
        std::unordered_map<int, std::list<Entry>::iterator> map;
        size_t bytes = 0;
        const size_t limit_bytes_;
    };

    // ════════════════════════════════════════════════════════════════════════
    // PieceWaiter — condition-variable-based notification for piece readiness
    // ════════════════════════════════════════════════════════════════════════
    class PieceWaiter {
    public:
        void notify(int piece) {
            {
                std::lock_guard<std::mutex> lk(mu);
                ready.insert(piece);
            }
            cv.notify_all();
        }

        bool wait(int piece, int timeout_ms) {
            std::unique_lock<std::mutex> lk(mu);
            return cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                [&] { return ready.count(piece) > 0; });
        }

        void reset(int piece) {
            std::lock_guard<std::mutex> lk(mu);
            ready.erase(piece);
        }

    private:
        std::mutex mu;
        std::condition_variable cv;
        std::unordered_set<int> ready;
    };

    // ════════════════════════════════════════════════════════════════════════
    // StreamSession — per-stream state (single file within a torrent)
    // ════════════════════════════════════════════════════════════════════════
    struct StreamSession {
        lt::torrent_handle handle;
        int file_index = -1;
        int piece_size = 0;
        int first_piece = -1;
        int last_piece = -1;
        int64_t file_offset = 0;
        int64_t file_size = 0;
        std::string mime;

        std::unique_ptr<PieceCache> cache;
        PieceWaiter waiter;

        std::mutex inflight_mu;
        std::unordered_set<int> inflight_reads;   // pieces we want to load into cache
        std::unordered_set<int> pending_reads;    // pieces currently being read (single-flight)

        std::atomic<int> read_head = 0;
        int last_window_start = -1;
        int last_window_end = -1;
        std::atomic<int64_t> last_window_update_ms{ 0 };
        std::mutex window_mu;

        std::atomic<int64_t> last_range_pos = 0;
        std::atomic<int64_t> last_range_ts = 0;

        std::atomic<StreamState> state{ StreamState::Bootstrap };
        std::string file_name;
        std::string file_path;

        // Configuration snapshot (captured at start)
        StreamingConfig cfg;

        ~StreamSession() {}
    };

    // ════════════════════════════════════════════════════════════════════════
    // Global state
    // ════════════════════════════════════════════════════════════════════════
    static lt::session* g_session_ptr = nullptr;
    static std::shared_ptr<StreamSession>       g_stream;
    static std::mutex                           g_stream_mutex;
    static std::atomic<bool>                    g_running = false;
    static std::thread                          g_alert_thread;
    static std::unique_ptr<httplib::Server>     g_http_server;
    static std::thread                          g_http_thread;
    static std::string                          g_last_error;

    // ── Helpers ──────────────────────────────────────────────────────────

    static int64_t now_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    static bool piece_available(lt::torrent_handle& h, int piece) {
        auto st = h.status(lt::torrent_handle::query_pieces);
        if (piece < 0) return false;
        if (piece >= static_cast<int>(st.pieces.size())) return false;
        return st.pieces[lt::piece_index_t(piece)];
    }

    static int compute_readahead(const std::shared_ptr<StreamSession>& s) {
        auto st = s->handle.status();
        int rate = st.download_payload_rate;
        if (rate <= 0) return s->cfg.min_readahead;
        int pps = std::max(1, rate / s->piece_size);
        return std::clamp(pps * 8, s->cfg.min_readahead, s->cfg.max_readahead);
    }

    // ── Anchor piece helpers ─────────────────────────────────────────────

    static bool is_anchor_piece(const std::shared_ptr<StreamSession>& s, int piece) {
        if (piece == s->first_piece) return true;
        if (piece >= s->last_piece - s->cfg.tail_pieces + 1) return true;
        return false;
    }

    // ════════════════════════════════════════════════════════════════════════
    // PiecePlan — pure decision, no libtorrent side effects
    // ════════════════════════════════════════════════════════════════════════
    struct PiecePlan {
        std::vector<int> high_priority;
        std::vector<int> low_priority;
        std::vector<std::pair<int, int64_t>> deadlines;   // piece, deadline_ms
    };

    // Build a priority plan for a given playback position and state.
    static PiecePlan build_piece_plan(const std::shared_ptr<StreamSession>& s,
        int current_piece,
        StreamState state,
        int old_read_head = -1)
    {
        PiecePlan plan;
        int ahead = compute_readahead(s);
        int64_t base = now_ms();

        if (state == StreamState::Bootstrap) {
            int front_end = std::min(s->last_piece, s->first_piece + s->cfg.startup_buffer_pieces - 1);
            for (int p = s->first_piece; p <= front_end; ++p)
                plan.high_priority.push_back(p);

            for (int i = 0; i < s->cfg.tail_pieces; ++i) {
                int p = s->last_piece - i;
                if (p > s->first_piece && p > front_end)
                    plan.high_priority.push_back(p);
            }

            for (size_t i = 0; i < plan.high_priority.size(); ++i) {
                int p = plan.high_priority[i];
                if (p <= front_end) {
                    int64_t dl = base + (i == 0 ? 500 : s->cfg.deadline_base_ms + static_cast<int>(i) * s->cfg.deadline_step_ms);
                    plan.deadlines.push_back({ p, dl });
                }
                else {
                    plan.deadlines.push_back({ p, base + 3000 + static_cast<int>(i) * 1000 });
                }
            }
        }
        else {
            int new_start = std::max(s->first_piece, current_piece - s->cfg.back_window);
            int new_end = std::min(s->last_piece, current_piece + ahead);

            for (int p = new_start; p <= new_end; ++p)
                plan.high_priority.push_back(p);

            for (size_t i = 0; i < plan.high_priority.size(); ++i) {
                int p = plan.high_priority[i];
                int64_t dl = base + s->cfg.deadline_base_ms + static_cast<int>(i) * s->cfg.deadline_step_ms;
                plan.deadlines.push_back({ p, dl });
            }

            auto add_anchor = [&](int p, int64_t dl_ms) {
                if (std::find(plan.high_priority.begin(), plan.high_priority.end(), p)
                    == plan.high_priority.end()) {
                    plan.high_priority.push_back(p);
                }
                plan.deadlines.push_back({ p, base + dl_ms });
                };
            add_anchor(s->first_piece, 500);
            for (int i = 0; i < s->cfg.tail_pieces; ++i) {
                int p = s->last_piece - i;
                if (p > s->first_piece)
                    add_anchor(p, 1000 + i * 500);
            }

            int read_head = old_read_head >= 0
                ? old_read_head
                : s->read_head.load(std::memory_order_relaxed);
            int dead_zone_end = std::max(s->first_piece, read_head - s->cfg.back_window * 2);
            for (int p = s->first_piece; p < dead_zone_end; ++p) {
                if (!is_anchor_piece(s, p))
                    plan.low_priority.push_back(p);
            }

            int far_future_start = std::min(s->last_piece, current_piece + ahead * 2);
            for (int p = far_future_start; p <= s->last_piece; ++p) {
                if (!is_anchor_piece(s, p))
                    plan.low_priority.push_back(p);
            }
        }

        return plan;
    }

    // Apply a PiecePlan to the torrent handle.
    static void apply_piece_plan(lt::torrent_handle& h, const PiecePlan& plan) {
        for (int p : plan.high_priority) {
            h.piece_priority(lt::piece_index_t(p), lt::download_priority_t{ 7 });
        }
        for (int p : plan.low_priority) {
            h.piece_priority(lt::piece_index_t(p), lt::download_priority_t{ 1 });
        }
        for (auto& [p, dl] : plan.deadlines) {
            h.set_piece_deadline(lt::piece_index_t(p), dl,
                lt::torrent_handle::alert_when_available);
        }
    }

    // ── Transition check: move from Bootstrap to Streaming ──────────────
    static void check_bootstrap_transition(std::shared_ptr<StreamSession> s) {
        if (s->state.load(std::memory_order_relaxed) != StreamState::Bootstrap)
            return;

        int ready_count = 0;
        int front_end = std::min(s->last_piece, s->first_piece + s->cfg.startup_buffer_pieces - 1);
        for (int p = s->first_piece; p <= front_end; ++p) {
            if (s->cache->contains(p)) ready_count++;
        }

        if (ready_count >= s->cfg.startup_buffer_pieces) {
            s->state.store(StreamState::Streaming, std::memory_order_release);
            STREAM_LOG("Startup buffer complete, entering streaming mode");
            s->last_window_update_ms.store(0, std::memory_order_relaxed);
        }
    }

    // ── Sliding window update (time-throttled, state-aware) ────────────
    static void update_window(std::shared_ptr<StreamSession> s, int current_piece) {
        int64_t now = now_ms();
        int64_t last = s->last_window_update_ms.load(std::memory_order_relaxed);

        if ((now - last) < s->cfg.window_update_throttle_ms)
            return;

        std::lock_guard<std::mutex> lk(s->window_mu);
        s->read_head.store(current_piece, std::memory_order_relaxed);
        s->last_window_update_ms.store(now, std::memory_order_relaxed);

        StreamState current_state = s->state.load(std::memory_order_acquire);
        PiecePlan plan = build_piece_plan(s, current_piece, current_state);

        // Cancel deadlines that are no longer in the active high‑priority set
        if (s->last_window_start != -1 && s->last_window_end != -1) {
            for (int p = s->last_window_start; p <= s->last_window_end; ++p) {
                if (!is_anchor_piece(s, p) &&
                    std::find(plan.high_priority.begin(), plan.high_priority.end(), p)
                    == plan.high_priority.end()) {
                    s->handle.reset_piece_deadline(lt::piece_index_t(p));
                }
            }
        }

        apply_piece_plan(s->handle, plan);

        s->last_window_start = plan.high_priority.empty() ? current_piece : plan.high_priority.front();
        s->last_window_end = plan.high_priority.empty() ? current_piece : plan.high_priority.back();

        if (current_state == StreamState::Bootstrap)
            check_bootstrap_transition(s);
    }

    // ── Background prefetch: only marks pieces as needed, never calls read_piece ──
    static void prefetch_bootstrap_pieces(const std::shared_ptr<StreamSession>& s) {
        if (!s->cfg.enable_tail_prefetch) return;
        int front_end = std::min(s->last_piece, s->first_piece + s->cfg.startup_buffer_pieces - 1);
        // Front buffer
        for (int p = s->first_piece; p <= front_end; ++p) {
            if (!s->cache->contains(p)) {
                std::lock_guard<std::mutex> lk(s->inflight_mu);
                s->inflight_reads.insert(p);   // mark as needed
            }
        }
        // Tail anchors
        for (int i = 0; i < s->cfg.tail_pieces; ++i) {
            int p = s->last_piece - i;
            if (p <= s->first_piece) continue;
            if (!s->cache->contains(p)) {
                std::lock_guard<std::mutex> lk(s->inflight_mu);
                s->inflight_reads.insert(p);
            }
        }
    }

    // ════════════════════════════════════════════════════════════════════════
    // alert_pump – the single owner of read_piece
    // ════════════════════════════════════════════════════════════════════════
#pragma warning(push)
#pragma warning(disable : 26110)
#pragma warning(disable : 26117)
    static void alert_pump() {
        while (g_running.load(std::memory_order_acquire)) {
            std::vector<lt::alert*> alerts;
            if (g_session_ptr) g_session_ptr->pop_alerts(&alerts);

            std::shared_ptr<StreamSession> s;
            {
                std::lock_guard<std::mutex> lk(g_stream_mutex);
                s = g_stream;
            }
            if (!s) {
                std::this_thread::sleep_for(std::chrono::milliseconds(15));
                continue;
            }

            for (auto* a : alerts) {
                if (auto* pf = lt::alert_cast<lt::piece_finished_alert>(a)) {
                    int piece = static_cast<int>(pf->piece_index);
                    bool issue_read = false;
                    {
                        std::lock_guard<std::mutex> lk(s->inflight_mu);
                        // Only read if we need this piece, and it's not already being read
                        if (s->inflight_reads.count(piece) > 0 &&
                            s->pending_reads.insert(piece).second) {
                            issue_read = true;
                        }
                    }
                    if (issue_read) {
                        // This is the ONLY place where read_piece is called
                        s->handle.read_piece(lt::piece_index_t(piece));
                    }
                    // Always notify waiters – the piece might already be in cache
                    s->waiter.notify(piece);
                }
                else if (auto* rp = lt::alert_cast<lt::read_piece_alert>(a)) {
                    int piece = static_cast<int>(rp->piece);
                    {
                        std::lock_guard<std::mutex> lk(s->inflight_mu);
                        s->pending_reads.erase(piece);
                        s->inflight_reads.erase(piece);  // no longer needed since it's now in cache (or failed)
                    }
                    if (!rp->buffer || rp->size == 0) {
                        // Read failed – do NOT re-issue immediately; the piece might still be available.
                        // We simply re-insert it into inflight_reads so it can be retried later
                        // when another piece_finished_alert arrives (if still available).
                        // This avoids spawning threads that could duplicate read_piece.
                        std::lock_guard<std::mutex> lk(s->inflight_mu);
                        s->inflight_reads.insert(piece);   // re-mark as needed
                        continue;
                    }
                    auto data = std::make_shared<std::vector<char>>();
                    data->resize(rp->size);
                    std::memcpy(data->data(), rp->buffer.get(), rp->size);
                    s->cache->put(piece, std::move(data));
                }
                else if (lt::alert_cast<lt::torrent_error_alert>(a) ||
                    lt::alert_cast<lt::file_error_alert>(a)) {
                    STREAM_LOG("Critical error, stopping stream");
                    g_running.store(false, std::memory_order_release);
                    break;
                }
            }

            // During Bootstrap, mark front and tail pieces as needed (if not yet in cache)
            if (s && s->state.load(std::memory_order_acquire) == StreamState::Bootstrap) {
                prefetch_bootstrap_pieces(s);
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
    }
#pragma warning(pop)
    // ── ensure_piece – block until piece is in cache (NO read_piece call) ──
    static bool ensure_piece(std::shared_ptr<StreamSession> s, int piece) {
        if (s->cache->contains(piece)) return true;

        bool is_anchor = is_anchor_piece(s, piece);
        int max_attempts = is_anchor ? s->cfg.anchor_piece_poll_max_attempts : s->cfg.piece_poll_max_attempts;

        // Mark as needed – the alert_pump will eventually issue read_piece when available
        {
            std::lock_guard<std::mutex> lk(s->inflight_mu);
            s->inflight_reads.insert(piece);
        }

        // Set high priority and an initial urgent deadline so libtorrent downloads it quickly
        s->handle.piece_priority(lt::piece_index_t(piece), lt::download_priority_t{ 7 });
        s->handle.set_piece_deadline(lt::piece_index_t(piece),
            now_ms() + 500,
            lt::torrent_handle::alert_when_available);

        bool success = false;
        int64_t last_deadline_ms = now_ms();
        for (int attempt = 0;
            attempt < max_attempts && g_running.load(std::memory_order_acquire);
            ++attempt) {
            if (s->cache->contains(piece)) {
                success = true;
                break;
            }

            int64_t now = now_ms();

            // Re-emit deadline if piece hasn't arrived yet (no read_piece involved)
            bool still_needed = false;
            {
                std::lock_guard<std::mutex> lk(s->inflight_mu);
                still_needed = (s->inflight_reads.count(piece) > 0);
            }
            if (still_needed && (now - last_deadline_ms) >= s->cfg.deadline_reemit_interval_ms) {
                s->handle.piece_priority(lt::piece_index_t(piece), lt::download_priority_t{ 7 });
                s->handle.set_piece_deadline(lt::piece_index_t(piece),
                    now + (is_anchor ? 1000 : 500),
                    lt::torrent_handle::alert_when_available);
                last_deadline_ms = now;
                STREAM_LOG("Re-emitting deadline for piece %d (anchor=%d, attempt=%d)",
                    piece, is_anchor, attempt);
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(s->cfg.piece_poll_interval_ms));
        }

        // We no longer erase from inflight_reads here; the alert_pump will do it when the piece
        // arrives (or fails). This keeps the piece in the “wanted” set in case of re-requests.
        return success;
    }

    // ── Seek detection ──────────────────────────────────────────────────
    static bool is_seek(const std::shared_ptr<StreamSession>& s, uint64_t pos) {
        int64_t prev = s->last_range_pos.exchange(static_cast<int64_t>(pos));
        return std::llabs(static_cast<int64_t>(pos) - prev) > (8LL * 1024 * 1024);
    }

    // ── Cache hit ratio ─────────────────────────────────────────────────
    static double cache_hit_ratio(const std::shared_ptr<StreamSession>& s,
        int dest_piece, int ahead) {
        int dest_end = std::min(s->last_piece, dest_piece + ahead);
        int total = dest_end - dest_piece + 1;
        if (total <= 0) return 0.0;
        int cached = 0;
        for (int p = dest_piece; p <= dest_end; ++p) {
            if (s->cache->contains(p)) cached++;
        }
        return static_cast<double>(cached) / total;
    }

    // ── Seek logic ──────────────────────────────────────────────────────
    static void handle_seek(std::shared_ptr<StreamSession> s, int dest_piece) {
        int old_read_head = s->read_head.load(std::memory_order_relaxed);
        StreamState current_state = s->state.load(std::memory_order_acquire);
        PiecePlan plan = build_piece_plan(s, dest_piece, current_state, old_read_head);

        apply_piece_plan(s->handle, plan);

        s->last_window_start = plan.high_priority.empty() ? dest_piece : plan.high_priority.front();
        s->last_window_end = plan.high_priority.empty() ? dest_piece : plan.high_priority.back();
        s->last_window_update_ms.store(now_ms(), std::memory_order_relaxed);
        s->read_head.store(dest_piece, std::memory_order_relaxed);
    }

    // ── stream_range ────────────────────────────────────────────────────
    static bool stream_range(std::shared_ptr<StreamSession> s,
        uint64_t offset, uint64_t length,
        httplib::DataSink& sink) {
        uint64_t end = std::min<uint64_t>(offset + length, s->file_size);
        uint64_t pos = offset;
        bool seeking = is_seek(s, offset);
        int consecutive_failures = 0;

        while (pos < end && g_running.load(std::memory_order_acquire)) {
            int piece = static_cast<int>((s->file_offset + pos) / s->piece_size);
            piece = std::clamp(piece, s->first_piece, s->last_piece);

            if (seeking) {
                int ahead = compute_readahead(s);
                double ratio = cache_hit_ratio(s, piece, ahead);
                if (ratio < 0.8) {
                    handle_seek(s, piece);
                }
                else {
                    update_window(s, piece);
                }
                seeking = false;
            }
            else {
                update_window(s, piece);
            }

            STREAM_LOG("Serving piece %d (offset %llu)", piece,
                static_cast<unsigned long long>(pos));

            if (!ensure_piece(s, piece)) {
                if (++consecutive_failures > s->cfg.ensure_piece_max_retries) {
                    STREAM_LOG("Too many piece failures, aborting stream");
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
            consecutive_failures = 0;

            auto data = s->cache->get(piece);
            if (!data) continue;

            uint64_t piece_start = static_cast<uint64_t>(piece) * s->piece_size;
            uint64_t file_relative = piece_start - s->file_offset;
            uint64_t offset_in_piece = pos - file_relative;

            if (offset_in_piece >= data->size()) continue;

            uint64_t available = data->size() - offset_in_piece;
            uint64_t to_send = std::min<uint64_t>(available, end - pos);

            if (!sink.write(data->data() + offset_in_piece, to_send))
                return false;

            pos += to_send;
        }

        return pos >= end;
    }

    // ── Utility functions (MIME, JSON, etc.) ────────────────────────────
    static std::string detect_mime(const std::string& path) {
        std::string lower = path;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        if (lower.ends_with(".mp4"))                           return "video/mp4";
        if (lower.ends_with(".mkv"))                           return "video/x-matroska";
        if (lower.ends_with(".avi"))                           return "video/x-msvideo";
        if (lower.ends_with(".mov"))                           return "video/quicktime";
        if (lower.ends_with(".wmv"))                           return "video/x-ms-wmv";
        if (lower.ends_with(".webm"))                          return "video/webm";
        if (lower.ends_with(".flv"))                           return "video/x-flv";
        if (lower.ends_with(".m4v"))                           return "video/mp4";
        if (lower.ends_with(".mpg") || lower.ends_with(".mpeg")) return "video/mpeg";
        return "application/octet-stream";
    }

    static std::string escape_json(const std::string& s) {
        std::ostringstream o;
        for (unsigned char c : s) {
            switch (c) {
            case '"':  o << "\\\""; break;
            case '\\': o << "\\\\"; break;
            case '\n': o << "\\n";  break;
            case '\r': o << "\\r";  break;
            case '\t': o << "\\t";  break;
            default:
                if (c < 0x20) o << "\\u00" << std::hex << static_cast<int>(c) << std::dec;
                else          o << static_cast<char>(c);
            }
        }
        return o.str();
    }

    static std::string build_status_json(const std::shared_ptr<StreamSession>& sess) {
        if (!sess || !sess->handle.is_valid())
            return R"({"error":"no active session"})";

        std::shared_ptr<StreamSession> s;
        {
            std::lock_guard<std::mutex> lk(g_stream_mutex);
            s = g_stream;
        }
        if (!s) return R"({"error":"no active stream"})";

        try {
            lt::torrent_status st = s->handle.status(
                lt::torrent_handle::query_pieces |
                lt::torrent_handle::query_accurate_download_counters);

            auto ti = st.torrent_file.lock();   // puede ser null si no hay metadatos

            std::ostringstream j;
            j << "{\n";

            // ── Básicos de la sesión ─────────────────────────────────
            j << "  \"state\":\""
                << (s->state.load() == StreamState::Bootstrap ? "bootstrap" : "streaming")
                << "\",\n";
            j << "  \"read_head\":" << s->read_head.load() << ",\n";
            j << "  \"last_window_start\":" << s->last_window_start << ",\n";
            j << "  \"last_window_end\":" << s->last_window_end << ",\n";
            j << "  \"is_running\":" << (g_running.load() ? "true" : "false") << ",\n";

            // ── Configuración ────────────────────────────────────────
            const StreamingConfig& cfg = s->cfg;
            j << "  \"config\": {\n"
                << "    \"cache_limit_mb\":" << cfg.cache_limit_mb << ",\n"
                << "    \"min_readahead\":" << cfg.min_readahead << ",\n"
                << "    \"max_readahead\":" << cfg.max_readahead << ",\n"
                << "    \"back_window\":" << cfg.back_window << ",\n"
                << "    \"deadline_base_ms\":" << cfg.deadline_base_ms << ",\n"
                << "    \"deadline_step_ms\":" << cfg.deadline_step_ms << ",\n"
                << "    \"window_update_throttle_ms\":" << cfg.window_update_throttle_ms << ",\n"
                << "    \"piece_poll_interval_ms\":" << cfg.piece_poll_interval_ms << ",\n"
                << "    \"piece_poll_max_attempts\":" << cfg.piece_poll_max_attempts << ",\n"
                << "    \"anchor_piece_poll_max_attempts\":" << cfg.anchor_piece_poll_max_attempts << ",\n"
                << "    \"ensure_piece_max_retries\":" << cfg.ensure_piece_max_retries << ",\n"
                << "    \"deadline_reemit_interval_ms\":" << cfg.deadline_reemit_interval_ms << ",\n"
                << "    \"startup_buffer_pieces\":" << cfg.startup_buffer_pieces << ",\n"
                << "    \"tail_pieces\":" << cfg.tail_pieces << ",\n"
                << "    \"enable_tail_prefetch\":" << (cfg.enable_tail_prefetch ? "true" : "false") << "\n"
                << "  },\n";

            // ── Caché ─────────────────────────────────────────────────
            {
                j << "  \"cache\": {\n";
                j << "    \"first_piece_cached\":" << (s->cache->contains(s->first_piece) ? "true" : "false") << ",\n";
                j << "    \"last_piece_cached\":" << (s->cache->contains(s->last_piece) ? "true" : "false") << ",\n";

                int cached_in_window = 0;
                int window_size = 0;
                if (s->last_window_start != -1 && s->last_window_end != -1 &&
                    s->last_window_end >= s->last_window_start) {
                    window_size = s->last_window_end - s->last_window_start + 1;
                    for (int p = s->last_window_start; p <= s->last_window_end; ++p)
                        if (s->cache->contains(p)) cached_in_window++;
                }
                j << "    \"cached_in_window\":" << cached_in_window << ",\n";
                j << "    \"window_size\":" << window_size << "\n";
                j << "  },\n";
            }

            // ── Piezas en vuelo ──────────────────────────────────────
            {
                std::lock_guard<std::mutex> lk(s->inflight_mu);
                j << "  \"inflight\": {\n";
                j << "    \"inflight_count\":" << s->inflight_reads.size() << ",\n";
                j << "    \"pending_count\":" << s->pending_reads.size() << ",\n";
                j << "    \"inflight_pieces\": [";
                bool first = true;
                for (int p : s->inflight_reads) {
                    if (!first) j << ",";
                    j << p;
                    first = false;
                }
                j << "],\n";
                j << "    \"pending_pieces\": [";
                first = true;
                for (int p : s->pending_reads) {
                    if (!first) j << ",";
                    j << p;
                    first = false;
                }
                j << "]\n";
                j << "  },\n";
            }

            // ── Torrent ────────────────────────────────────────────────
            j << "  \"torrent\": {\n"
                << "    \"progress\":" << st.progress << ",\n"
                << "    \"download_rate\":" << st.download_payload_rate << ",\n"
                << "    \"upload_rate\":" << st.upload_payload_rate << ",\n"
                << "    \"num_peers\":" << st.num_peers << ",\n"
                << "    \"num_seeds\":" << st.num_seeds << ",\n"
                << "    \"is_finished\":" << (st.is_finished ? "true" : "false") << ",\n"
                << "    \"is_seeding\":" << (st.is_seeding ? "true" : "false") << ",\n"
                << "    \"total_downloaded\":" << st.total_payload_download << ",\n"
                << "    \"total_uploaded\":" << st.total_payload_upload << ",\n"
                << "    \"state_flags\":\"" << st.state << "\"\n"
                << "  },\n";

            // ── Archivo servido (siempre presente) ─────────────────────
            {
                j << "  \"file\": {\n"
                    << "    \"index\":" << s->file_index << ",\n"
                    << "    \"name\":\"" << escape_json(s->file_name.empty() ? "unknown" : s->file_name) << "\",\n"
                    << "    \"path\":\"" << escape_json(s->file_path.empty() ? "unknown" : s->file_path) << "\",\n"
                    << "    \"size\":" << s->file_size << ",\n";

                // Progreso exacto del archivo (si es posible)
                if (ti) {
                    std::vector<int64_t> fprog;
                    s->handle.file_progress(fprog);
                    int64_t file_down = (s->file_index < static_cast<int>(fprog.size()))
                        ? fprog[s->file_index] : 0;
                    j << "    \"downloaded\":" << file_down << ",\n";
                    j << "    \"progress\":" << (s->file_size > 0 ? static_cast<double>(file_down) / s->file_size : 0.0) << ",\n";
                }
                else {
                    // Estimación basada en el progreso global del torrent
                    int64_t approx_down = static_cast<int64_t>(st.progress * s->file_size);
                    j << "    \"downloaded\":" << approx_down << ",\n";
                    j << "    \"progress\":" << (s->file_size > 0 ? st.progress : 0.0) << ",\n";
                }
                j << "    \"first_piece\":" << s->first_piece << ",\n"
                    << "    \"last_piece\":" << s->last_piece << "\n"
                    << "  },\n";
            }

            // ── Anclas ──────────────────────────────────────────────────
            j << "  \"anchors\": {\n"
                << "    \"first_piece\":" << s->first_piece << ",\n"
                << "    \"first_piece_available\":"
                << (piece_available(s->handle, s->first_piece) ? "true" : "false") << ",\n"
                << "    \"last_pieces\": [";
            {
                bool first_tail = true;
                for (int i = 0; i < s->cfg.tail_pieces; ++i) {
                    int p = s->last_piece - i;
                    if (p <= s->first_piece) break;
                    if (!first_tail) j << ",";
                    j << "{"
                        << "\"piece\":" << p
                        << ",\"available\":" << (piece_available(s->handle, p) ? "true" : "false")
                        << ",\"cached\":" << (s->cache->contains(p) ? "true" : "false")
                        << "}";
                    first_tail = false;
                }
            }
            j << "]\n"
                << "  }\n";

            j << "}";
            return j.str();
        }
        catch (...) {
            return R"({"error":"internal error"})";
        }
    }
    // ════════════════════════════════════════════════════════════════════════
    // Public API
    // ════════════════════════════════════════════════════════════════════════

    bool start_server(lt::session* session, lt::torrent_handle* handle_ptr,
        int32_t file_index, int32_t port, std::string& out_url)
    {
        if (g_running.load(std::memory_order_acquire)) {
            g_running.store(false, std::memory_order_release);
            return false;
        }
        if (!handle_ptr || !handle_ptr->is_valid()) {
            return false;
        }

        g_session_ptr = session;
        lt::torrent_handle h = *handle_ptr;

        // ── Apply streaming-optimized session settings ─────────────────
        if (session) {
            lt::settings_pack sp;

            sp.set_int(lt::settings_pack::alert_mask,
                lt::alert_category::status |
                lt::alert_category::storage |
                lt::alert_category::error |
                lt::alert_category::piece_progress);

            sp.set_int(lt::settings_pack::connections_limit, 300);
            sp.set_int(lt::settings_pack::max_out_request_queue, 2500);
            sp.set_int(lt::settings_pack::max_allowed_in_request_queue, 2500);
            sp.set_int(lt::settings_pack::request_queue_time, 1);

            sp.set_int(lt::settings_pack::whole_pieces_threshold, 0);
            sp.set_bool(lt::settings_pack::smooth_connects, false);

            sp.set_int(lt::settings_pack::disk_io_read_mode, lt::settings_pack::enable_os_cache);
            sp.set_int(lt::settings_pack::disk_io_write_mode, lt::settings_pack::enable_os_cache);
            sp.set_int(lt::settings_pack::max_queued_disk_bytes, 128 * 1024 * 1024);
            sp.set_int(lt::settings_pack::aio_threads, 8);
            sp.set_int(lt::settings_pack::file_pool_size, 60);

            sp.set_bool(lt::settings_pack::strict_end_game_mode, false);

            sp.set_bool(lt::settings_pack::enable_dht, true);
            sp.set_bool(lt::settings_pack::enable_lsd, true);
            sp.set_bool(lt::settings_pack::enable_upnp, true);
            sp.set_bool(lt::settings_pack::enable_natpmp, true);


            session->apply_settings(sp);
            STREAM_LOG("Streaming session configured");
        }

        lt::torrent_status st = h.status();
        auto ti = st.torrent_file.lock();
        if (!ti) return false;

        const auto& files = ti->files();
        if (file_index < 0 || file_index >= files.num_files()) return false;

        lt::file_index_t fidx{ file_index };
        int64_t fsize = files.file_size(fidx);
        int64_t foff = files.file_offset(fidx);
        int psize = ti->piece_length();
        int fp = static_cast<int>(foff / psize);
        int lp = static_cast<int>((foff + fsize - 1) / psize);

        auto s = std::make_shared<StreamSession>();
        s->handle = h;
        s->file_index = file_index;
        s->piece_size = psize;
        s->file_offset = foff;
        s->file_size = fsize;
        s->first_piece = fp;
        s->last_piece = lp;
        s->mime = detect_mime(files.file_path(fidx));
        s->state.store(StreamState::Bootstrap, std::memory_order_release);
        s->cfg = get_config();   // snapshot current configuration
        s->file_name = std::string(files.file_name(fidx));
        s->file_path = std::string(files.file_path(fidx));
        // Create cache with configured limit
        s->cache = std::make_unique<PieceCache>(s->cfg.cache_limit_mb);

        {
            std::lock_guard<std::mutex> lk(g_stream_mutex);
            g_stream = s;
        }

        // Start the alert pump thread before initial window update
        g_running.store(true, std::memory_order_release);
        g_alert_thread = std::thread(alert_pump);

        // Force bootstrap plan update
        s->last_window_update_ms.store(0, std::memory_order_relaxed);
        update_window(s, fp);

        // Mark first piece as needed; alert_pump will handle the actual read_piece
        {
            std::lock_guard<std::mutex> lk(s->inflight_mu);
            s->inflight_reads.insert(fp);
        }

        // Set an urgent deadline so libtorrent downloads it with priority
        h.set_piece_deadline(lt::piece_index_t(fp), now_ms() + 500,
            lt::torrent_handle::alert_when_available);
        h.piece_priority(lt::piece_index_t(fp), lt::download_priority_t{ 7 });

        // ── HTTP server setup ──────────────────────────────────────────
        if (g_http_server) {
            g_http_server->stop();
            if (g_http_thread.joinable()) g_http_thread.join();
        }
        g_http_server = std::make_unique<httplib::Server>();

        g_http_server->set_keep_alive_timeout(300);
        g_http_server->set_keep_alive_max_count(10000);
        g_http_server->set_read_timeout(60, 0);
        g_http_server->set_write_timeout(120, 0);
        g_http_server->set_tcp_nodelay(true);

        auto add_cors = [](httplib::Response& res) {
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_header("Access-Control-Allow-Headers", "Range, If-None-Match");
            res.set_header("Access-Control-Expose-Headers",
                "Content-Range, Content-Length, Accept-Ranges, ETag");
            };

        g_http_server->Get("/stream", [s, add_cors](const httplib::Request&,
            httplib::Response& res) {
                add_cors(res);
                res.set_header("Accept-Ranges", "bytes");
                res.set_content_provider(
                    s->file_size,
                    s->mime,
                    [s](uint64_t off, uint64_t len, httplib::DataSink& sink) -> bool {
                        return stream_range(s, off, len, sink);
                    });
            });

        g_http_server->Get("/status", [s, add_cors](const httplib::Request&,
            httplib::Response& res) {
                res.set_content(build_status_json(s), "application/json");
                add_cors(res);
            });

        g_http_server->Options(R"(.*)", [add_cors](const httplib::Request&,
            httplib::Response& res) {
                add_cors(res);
                res.status = 204;
            });

        g_http_thread = std::thread([port]() {
            try {
                g_http_server->listen("127.0.0.1", port);
            }
            catch (...) {}
            });

        char url_buf[128];
        std::snprintf(url_buf, sizeof(url_buf), "http://127.0.0.1:%d/stream", port);
        out_url = url_buf;
        return true;
    }

    void stop_server() {
        g_running.store(false, std::memory_order_release);

        if (g_alert_thread.joinable()) g_alert_thread.join();

        if (g_http_server) {
            g_http_server->stop();
            if (g_http_thread.joinable()) g_http_thread.join();
        }

        {
            std::lock_guard<std::mutex> lk(g_stream_mutex);
            g_stream.reset();
        }
        g_http_server.reset();
    }

    bool is_running() {
        return g_running.load(std::memory_order_acquire);
    }

    const char* last_error() {
        return g_last_error.c_str();
    }

    void reset_state() {
        stop_server();
        g_session_ptr = nullptr;
    }

    bool is_byte_available(lt::torrent_handle* handle, int32_t file_index,
        int64_t byte_position) {
        if (!handle || !handle->is_valid()) return false;
        try {
            auto ti = handle->torrent_file();
            if (!ti || file_index < 0 || file_index >= ti->files().num_files())
                return false;
            auto abs = ti->files().file_offset(lt::file_index_t(file_index)) + byte_position;
            int pi = static_cast<int>(abs / ti->piece_length());
            auto st = handle->status(lt::torrent_handle::query_pieces);
            return pi >= 0 && pi < static_cast<int>(st.pieces.size()) &&
                st.pieces[lt::piece_index_t(pi)];
        }
        catch (...) {
            return false;
        }
    }

    bool prioritize_seek_range(lt::torrent_handle* handle, int32_t file_index,
        int64_t byte_position, int64_t piece_size) {
        if (!handle || !handle->is_valid() || piece_size <= 0) return false;
        try {
            auto ti = handle->torrent_file();
            if (!ti || file_index < 0 || file_index >= ti->files().num_files())
                return false;
            auto abs = ti->files().file_offset(lt::file_index_t(file_index)) + byte_position;
            int pi = static_cast<int>(abs / piece_size);
            auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            handle->set_piece_deadline(lt::piece_index_t(pi), now + 1500);
            int last = static_cast<int>(
                (ti->files().file_offset(lt::file_index_t(file_index)) +
                    ti->files().file_size(lt::file_index_t(file_index)) - 1) / piece_size);
            for (int p = pi + 1; p <= std::min(pi + 2, last); ++p)
                handle->set_piece_deadline(lt::piece_index_t(p), now + 3000 + (p - pi) * 2000);
            return true;
        }
        catch (...) {
            return false;
        }
    }

    void notify_piece_ready(int piece_idx) {
        std::shared_ptr<StreamSession> s;
        {
            std::lock_guard<std::mutex> lk(g_stream_mutex);
            s = g_stream;
        }
        if (s) s->waiter.notify(piece_idx);
    }

} // namespace cs_stream

// ════════════════════════════════════════════════════════════════════════
// Exported C API (helpers fuera de extern "C")
// ════════════════════════════════════════════════════════════════════════
static std::string  g_url_result;
static std::mutex   g_url_mutex;

extern "C" {

    const char* start_stream_server_impl(lt::session* session,
        lt::torrent_handle* handle,
        int32_t file_index, int32_t port) {
        std::lock_guard<std::mutex> lk(g_url_mutex);
        if (cs_stream::start_server(session, handle, file_index, port, g_url_result))
            return g_url_result.c_str();
        return nullptr;
    }

    void        stop_stream_server_impl() { cs_stream::stop_server(); }
    uint8_t     is_stream_server_running_impl() { return cs_stream::is_running() ? 1 : 0; }
    void        reset_stream_server_impl() { cs_stream::reset_state(); }
    const char* get_last_stream_error_impl() { return cs_stream::last_error(); }

    void        notify_piece_ready_impl(int32_t piece_idx) {
        cs_stream::notify_piece_ready(piece_idx);
    }

    // Streaming configuration (call before start_server)
    void configure_streaming_impl(
        int cache_limit_mb,
        int min_readahead, int max_readahead,
        int back_window,
        int deadline_base_ms, int deadline_step_ms,
        int window_update_throttle_ms,
        int piece_poll_interval_ms,
        int piece_poll_max_attempts,
        int anchor_piece_poll_max_attempts,
        int ensure_piece_max_retries,
        int deadline_reemit_interval_ms,
        int startup_buffer_pieces,
        int tail_pieces,
        uint8_t enable_tail_prefetch) {

        cs_stream::StreamingConfig cfg;
        cfg.cache_limit_mb = cache_limit_mb;
        cfg.min_readahead = min_readahead;
        cfg.max_readahead = max_readahead;
        cfg.back_window = back_window;
        cfg.deadline_base_ms = deadline_base_ms;
        cfg.deadline_step_ms = deadline_step_ms;
        cfg.window_update_throttle_ms = window_update_throttle_ms;
        cfg.piece_poll_interval_ms = piece_poll_interval_ms;
        cfg.piece_poll_max_attempts = piece_poll_max_attempts;
        cfg.anchor_piece_poll_max_attempts = anchor_piece_poll_max_attempts;
        cfg.ensure_piece_max_retries = ensure_piece_max_retries;
        cfg.deadline_reemit_interval_ms = deadline_reemit_interval_ms;
        cfg.startup_buffer_pieces = startup_buffer_pieces;
        cfg.tail_pieces = tail_pieces;
        cfg.enable_tail_prefetch = (enable_tail_prefetch != 0);
        cs_stream::configure_streaming(cfg);
    }
} // extern "C"