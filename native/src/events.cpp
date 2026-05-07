// events.cpp - Sistema de alertas por sesión, sin variables globales estáticas
#include "events.h"
#include <libtorrent/session.hpp>
#include <libtorrent/alert_types.hpp>
#include <ctime>
#include <condition_variable>
#include <atomic>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <vector>

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

// ─── estructura por sesión ──────────────────────────────────────────────
struct cs_event_handler {
    lt::session* session = nullptr;
    cs_alert_callback callback = nullptr;
    bool include_unmapped = false;

    std::mutex mtx;
    std::condition_variable cv;
    bool notify_flag = false;
    std::thread thread;
    std::atomic<bool> running{ false };
};

// Mapa global protegido para almacenar los handlers activos (uno por sesión)
static std::mutex g_handlers_mutex;
static std::unordered_map<lt::session*, std::unique_ptr<cs_event_handler>> g_handlers;

// ─── procesamiento de alertas ──────────────────────────────────────────
static void process_alerts(cs_event_handler* handler) {
    std::vector<lt::alert*> events;
    handler->session->pop_alerts(&events);

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
            handler->callback(&status_alert);
            break;
        }
        case lt::torrent_removed_alert::alert_type: {
            auto* removed_alert = lt::alert_cast<lt::torrent_removed_alert>(alert);
            cs_torrent_remove_alert removed_torrent{};
            fill_info_hash(removed_alert->info_hashes, removed_torrent.info_hash);
            fill_event_info(&removed_torrent.alert, alert, cs_alert_type::alert_torrent_removed, message);
            handler->callback(&removed_torrent);
            break;
        }
        case lt::performance_alert::alert_type: {
            auto* perf_alert = lt::alert_cast<lt::performance_alert>(alert);
            cs_client_performance_alert perf_warning{};
            perf_warning.warning_type = perf_alert->warning_code;
            fill_event_info(&perf_warning.alert, alert, cs_alert_type::alert_client_performance, message);
            handler->callback(&perf_warning);
            break;
        }
        case lt::peer_connect_alert::alert_type: {
            auto* peer_alert = lt::alert_cast<lt::peer_connect_alert>(alert);
            auto direction = (peer_alert->direction == lt::peer_connect_alert::direction_t::in)
                ? cs_peer_alert_type::connected_in : cs_peer_alert_type::connected_out;
            cs_peer_alert peer_connected{};
            populate_peer_alert(&peer_connected, peer_alert, direction, message);
            handler->callback(&peer_connected);
            break;
        }
        case lt::peer_disconnected_alert::alert_type: {
            auto* peer_alert = lt::alert_cast<lt::peer_disconnected_alert>(alert);
            cs_peer_alert peer_disconnected{};
            populate_peer_alert(&peer_disconnected, peer_alert, cs_peer_alert_type::disconnected, message);
            handler->callback(&peer_disconnected);
            break;
        }
        case lt::peer_ban_alert::alert_type: {
            auto* peer_alert = lt::alert_cast<lt::peer_ban_alert>(alert);
            cs_peer_alert peer_banned{};
            populate_peer_alert(&peer_banned, peer_alert, cs_peer_alert_type::banned, message);
            handler->callback(&peer_banned);
            break;
        }
        case lt::peer_snubbed_alert::alert_type: {
            auto* peer_alert = lt::alert_cast<lt::peer_snubbed_alert>(alert);
            cs_peer_alert peer_snubbed{};
            populate_peer_alert(&peer_snubbed, peer_alert, cs_peer_alert_type::snubbed, message);
            handler->callback(&peer_snubbed);
            break;
        }
        case lt::peer_unsnubbed_alert::alert_type: {
            auto* peer_alert = lt::alert_cast<lt::peer_unsnubbed_alert>(alert);
            cs_peer_alert peer_unsnubbed{};
            populate_peer_alert(&peer_unsnubbed, peer_alert, cs_peer_alert_type::unsnubbed, message);
            handler->callback(&peer_unsnubbed);
            break;
        }
        case lt::peer_error_alert::alert_type: {
            auto* peer_alert = lt::alert_cast<lt::peer_error_alert>(alert);
            cs_peer_alert peer_errored{};
            populate_peer_alert(&peer_errored, peer_alert, cs_peer_alert_type::errored, message);
            handler->callback(&peer_errored);
            break;
        }
        default: {
            if (!handler->include_unmapped) break;
            cs_alert generic_alert{};
            fill_event_info(&generic_alert, alert, cs_alert_type::alert_generic, message);
            handler->callback(&generic_alert);
            break;
        }
        }
    }
}

// Hilo persistente para un handler específico
static void alert_thread_func(cs_event_handler* handler) {
    while (handler->running) {
        std::unique_lock<std::mutex> lock(handler->mtx);
        handler->cv.wait(lock, [handler] { return handler->notify_flag || !handler->running; });
        handler->notify_flag = false;
        lock.unlock();

        if (!handler->running) break;
        process_alerts(handler);
    }
}

// ─── API pública del nuevo sistema por sesión ───────────────────────────
cs_event_handle cs_create_event_handler(lt::session* session, cs_alert_callback callback, bool include_unmapped) {
    if (!session || !callback) return nullptr;

    std::lock_guard<std::mutex> lock(g_handlers_mutex);
    // Si ya hay un handler para esta sesión, lo destruimos primero
    auto it = g_handlers.find(session);
    if (it != g_handlers.end()) {
        cs_destroy_event_handler(it->second.get());
        g_handlers.erase(it);
    }

    auto handler = std::make_unique<cs_event_handler>();
    handler->session = session;
    handler->callback = callback;
    handler->include_unmapped = include_unmapped;
    handler->running = true;

    // Capturamos el puntero crudo para el notificador
    auto* handler_ptr = handler.get();
    handler->thread = std::thread(alert_thread_func, handler_ptr);

    session->set_alert_notify([handler_ptr]() {
        std::lock_guard<std::mutex> lock(handler_ptr->mtx);
        handler_ptr->notify_flag = true;
        handler_ptr->cv.notify_one();
        });

    g_handlers[session] = std::move(handler);
    return handler_ptr;
}

void cs_destroy_event_handler(cs_event_handle handle) {
    if (!handle) return;
    handle->running = false;
    handle->cv.notify_one();
    if (handle->thread.joinable())
        handle->thread.join();
    if (handle->session)
        handle->session->set_alert_notify(nullptr);
}

void cs_destroy_event_handler_for_session(lt::session* session) {
    if (!session) return;
    std::lock_guard<std::mutex> lock(g_handlers_mutex);
    auto it = g_handlers.find(session);
    if (it != g_handlers.end()) {
        cs_destroy_event_handler(it->second.get());
        g_handlers.erase(it);
    }
}