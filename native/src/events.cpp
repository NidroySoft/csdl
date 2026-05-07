// events.cpp - Corregido: hilo persistente de alertas, sin creación de hilos por alerta
#include "events.h"
#include <libtorrent/session.hpp>
#include <libtorrent/alert_types.hpp>
#include <ctime>
#include <condition_variable>
#include <atomic>

// ─── helpers existentes (sin cambios) ─────────────────────────────────────
void fill_info_hash(const lt::info_hash_t& hashes, char* buffer) {
    if (hashes.has_v1()) {
        std::copy(hashes.v1.begin(), hashes.v1.end(), buffer);
    }
    else {
        std::fill(buffer, buffer + 20, 0);
    }
}

void fill_event_info(cs_alert* alert, lt::alert* lt_alert, cs_alert_type alert_type, std::string& message) {
    alert->type = alert_type;
    alert->epoch = time(nullptr);
    alert->category = static_cast<int32_t>(static_cast<uint32_t>(lt_alert->category()));
    message = lt_alert->message();
    alert->message = message.c_str();
}

void populate_peer_alert(cs_peer_alert* peer_alert, lt::peer_alert* alert, cs_peer_alert_type alert_type, std::string& message) {
    fill_event_info(&peer_alert->alert, alert, cs_alert_type::alert_peer_notification, message);
    peer_alert->type = alert_type;
    peer_alert->handle = nullptr;

    auto v6_mapped_addr = alert->endpoint.address().to_v6().to_bytes();
    std::copy(v6_mapped_addr.begin(), v6_mapped_addr.end(), peer_alert->ipv6_address);
    fill_info_hash(alert->handle.info_hashes(), peer_alert->info_hash);
}

// ─── nueva infraestructura de alertas ──────────────────────────────────────
static std::mutex g_alert_mutex;
static std::condition_variable g_alert_cv;
static bool g_alert_notify_flag = false;
static std::thread g_alert_thread;
static std::atomic<bool> g_alert_running(false);

static lt::session* g_session = nullptr;
static cs_alert_callback g_callback = nullptr;
static bool g_include_unmapped = false;

// función interna que procesa las alertas (SE LLAMA SOLO DESDE EL HILO PERSISTENTE)
static void process_available_alerts() {
    std::vector<lt::alert*> events;
    g_session->pop_alerts(&events);

    for (auto* alert : events) {
        std::string message;
        switch (alert->type()) {
        case lt::state_changed_alert::alert_type: {
            auto* state_alert = lt::alert_cast<lt::state_changed_alert>(alert);
            cs_torrent_status_alert status_alert{};
            status_alert.new_state = state_alert->state;
            status_alert.old_state = state_alert->prev_state;
            fill_info_hash(state_alert->handle.info_hashes(), status_alert.info_hash);
            fill_event_info(&status_alert.alert, alert, cs_alert_type::alert_torrent_status, message);
            g_callback(&status_alert);
            break;
        }
        case lt::torrent_removed_alert::alert_type: {
            auto* removed_alert = lt::alert_cast<lt::torrent_removed_alert>(alert);
            cs_torrent_remove_alert removed_torrent{};
            fill_info_hash(removed_alert->info_hashes, removed_torrent.info_hash);
            fill_event_info(&removed_torrent.alert, alert, cs_alert_type::alert_torrent_removed, message);
            g_callback(&removed_torrent);
            break;
        }
        case lt::performance_alert::alert_type: {
            auto* perf_alert = lt::alert_cast<lt::performance_alert>(alert);
            cs_client_performance_alert perf_warning{};
            perf_warning.warning_type = perf_alert->warning_code;
            fill_event_info(&perf_warning.alert, alert, cs_alert_type::alert_client_performance, message);
            g_callback(&perf_warning);
            break;
        }
        case lt::peer_connect_alert::alert_type: {
            auto* peer_alert = lt::alert_cast<lt::peer_connect_alert>(alert);
            auto direction = (peer_alert->direction == lt::peer_connect_alert::direction_t::in)
                ? cs_peer_alert_type::connected_in : cs_peer_alert_type::connected_out;
            cs_peer_alert peer_connected{};
            populate_peer_alert(&peer_connected, peer_alert, direction, message);
            g_callback(&peer_connected);
            break;
        }
        case lt::peer_disconnected_alert::alert_type: {
            auto* peer_alert = lt::alert_cast<lt::peer_disconnected_alert>(alert);
            cs_peer_alert peer_disconnected{};
            populate_peer_alert(&peer_disconnected, peer_alert, cs_peer_alert_type::disconnected, message);
            g_callback(&peer_disconnected);
            break;
        }
        case lt::peer_ban_alert::alert_type: {
            auto* peer_alert = lt::alert_cast<lt::peer_ban_alert>(alert);
            cs_peer_alert peer_banned{};
            populate_peer_alert(&peer_banned, peer_alert, cs_peer_alert_type::banned, message);
            g_callback(&peer_banned);
            break;
        }
        case lt::peer_snubbed_alert::alert_type: {
            auto* peer_alert = lt::alert_cast<lt::peer_snubbed_alert>(alert);
            cs_peer_alert peer_snubbed{};
            populate_peer_alert(&peer_snubbed, peer_alert, cs_peer_alert_type::snubbed, message);
            g_callback(&peer_snubbed);
            break;
        }
        case lt::peer_unsnubbed_alert::alert_type: {
            auto* peer_alert = lt::alert_cast<lt::peer_unsnubbed_alert>(alert);
            cs_peer_alert peer_unsnubbed{};
            populate_peer_alert(&peer_unsnubbed, peer_alert, cs_peer_alert_type::unsnubbed, message);
            g_callback(&peer_unsnubbed);
            break;
        }
        case lt::peer_error_alert::alert_type: {
            auto* peer_alert = lt::alert_cast<lt::peer_error_alert>(alert);
            cs_peer_alert peer_errored{};
            populate_peer_alert(&peer_errored, peer_alert, cs_peer_alert_type::errored, message);
            g_callback(&peer_errored);
            break;
        }
        default: {
            if (!g_include_unmapped) break;
            cs_alert generic_alert{};
            fill_event_info(&generic_alert, alert, cs_alert_type::alert_generic, message);
            g_callback(&generic_alert);
            break;
        }
        }
    }
}

// hilo persistente que procesa alertas
static void alert_thread_func() {
    while (g_alert_running) {
        std::unique_lock<std::mutex> lock(g_alert_mutex);
        g_alert_cv.wait(lock, [] { return g_alert_notify_flag || !g_alert_running; });
        g_alert_notify_flag = false;
        lock.unlock();

        if (!g_alert_running) break;

        process_available_alerts();
    }
}

// ─── API pública para el sistema de eventos ────────────────────────────────
void cs_set_event_callback(lt::session* session, cs_alert_callback callback, bool include_unmapped) {
    if (!session) return;

    // detener cualquier hilo anterior
    if (g_alert_running) {
        g_alert_running = false;
        g_alert_cv.notify_one();
        if (g_alert_thread.joinable()) g_alert_thread.join();
    }

    if (callback == nullptr) {
        // si callback es nulo, solo limpiamos y no creamos hilo
        session->set_alert_notify(nullptr);
        g_session = nullptr;
        g_callback = nullptr;
        return;
    }

    // configurar nuevo hilo
    g_session = session;
    g_callback = callback;
    g_include_unmapped = include_unmapped;
    g_alert_running = true;

    g_alert_thread = std::thread(alert_thread_func);

    // el notificador solo despierta al hilo persistente
    session->set_alert_notify([session]() {
        std::lock_guard<std::mutex> lock(g_alert_mutex);
        g_alert_notify_flag = true;
        g_alert_cv.notify_one();
        });
}

void cs_clear_event_callback(lt::session* session) {
    // lo mismo que detener todo
    cs_set_event_callback(session, nullptr, false);
}