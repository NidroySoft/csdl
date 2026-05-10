// settings.cpp – Configuración nativa de libtorrent optimizada para streaming
#include "settings.h"
#include <magic_enum/magic_enum.hpp>
#include <functional>
#include <string>
#include <libtorrent/alert.hpp>         // para lt::alert_category

#pragma region "enum mapping"
namespace magic_enum::customize {
    template <>
    struct enum_range<libtorrent::settings_pack::int_types> {
        static constexpr int min = libtorrent::settings_pack::int_type_base;
        static constexpr int max = libtorrent::settings_pack::max_int_setting_internal;
    };
    template <>
    struct enum_range<libtorrent::settings_pack::string_types> {
        static constexpr int min = libtorrent::settings_pack::string_type_base;
        static constexpr int max = libtorrent::settings_pack::max_string_setting_internal;
    };
    template <>
    struct enum_range<libtorrent::settings_pack::bool_types> {
        static constexpr int min = libtorrent::settings_pack::bool_type_base;
        static constexpr int max = libtorrent::settings_pack::max_bool_setting_internal;
    };
}
#pragma endregion

// ─── Helper interno (C++ linkage, no exportado) ───────────────────────
template <typename T>
typename std::enable_if<std::is_enum<T>::value, bool>::type
set_value(const char* key, std::function<bool(T)> setter) {
    if (!key) return false;
    auto enum_key = magic_enum::enum_cast<T>(key, magic_enum::case_insensitive);
    if (!enum_key.has_value()) return false;
    return setter(enum_key.value());
}

// ─── Funciones exportadas ─────────────────────────────────────────────
extern "C" {

    CSDL_EXPORT lt::settings_pack* create_settings_pack() {
        return new lt::settings_pack;
    }

    CSDL_EXPORT void destroy_settings_pack(lt::settings_pack* pack) {
        delete pack;
    }

    CSDL_EXPORT uint8_t settings_pack_set_str(lt::settings_pack* pack, const char* key, const char* value) {
        if (!pack) return false;
        return set_value<lt::settings_pack::string_types>(key, [pack, value](lt::settings_pack::string_types k) {
            if (!value) return false;
            pack->set_str(k, std::string(value));
            return true;
            });
    }

    CSDL_EXPORT uint8_t settings_pack_set_bool(lt::settings_pack* pack, const char* key, uint8_t value) {
        if (!pack) return false;
        return set_value<lt::settings_pack::bool_types>(key, [pack, value](lt::settings_pack::bool_types k) {
            pack->set_bool(k, static_cast<bool>(value));
            return true;
            });
    }

    CSDL_EXPORT uint8_t settings_pack_set_int(lt::settings_pack* pack, const char* key, int value) {
        if (!pack) return false;
        return set_value<lt::settings_pack::int_types>(key, [pack, value](lt::settings_pack::int_types k) {
            pack->set_int(k, value);
            return true;
            });
    }

    // ─── Configuración completa de streaming ───────────────────────────────
    CSDL_EXPORT lt::settings_pack* create_streaming_settings() {
        auto* pack = new lt::settings_pack;

        // Alertas
        pack->set_int(lt::settings_pack::alert_mask,
            lt::alert_category::status |
            lt::alert_category::storage |
            lt::alert_category::error |
            lt::alert_category::piece_progress);

        // Conexiones
        pack->set_int(lt::settings_pack::connections_limit, 300);
        pack->set_int(lt::settings_pack::connection_speed, 200);
        pack->set_int(lt::settings_pack::torrent_connect_boost, 100);

        // Reintentos y timeouts
        pack->set_int(lt::settings_pack::min_reconnect_time, 1);
        pack->set_int(lt::settings_pack::peer_connect_timeout, 3);
        pack->set_int(lt::settings_pack::request_timeout, 10);
        pack->set_int(lt::settings_pack::piece_timeout, 10);
        pack->set_int(lt::settings_pack::peer_timeout, 10);
        pack->set_int(lt::settings_pack::urlseed_timeout, 10);

        // Colas de peticiones
        pack->set_int(lt::settings_pack::max_out_request_queue, 2500);
        pack->set_int(lt::settings_pack::max_allowed_in_request_queue, 2500);
        pack->set_int(lt::settings_pack::request_queue_time, 1);

        // Piezas completas
        pack->set_int(lt::settings_pack::whole_pieces_threshold, 0);

        // Sin suavizado de conexiones
        pack->set_bool(lt::settings_pack::smooth_connects, false);

        // E/S de disco
        pack->set_int(lt::settings_pack::disk_io_read_mode, lt::settings_pack::enable_os_cache);
        pack->set_int(lt::settings_pack::disk_io_write_mode, lt::settings_pack::enable_os_cache);
        pack->set_int(lt::settings_pack::max_queued_disk_bytes, 128 * 1024 * 1024);

        // Hilos y caché de archivos
        pack->set_int(lt::settings_pack::aio_threads, 8);
        pack->set_int(lt::settings_pack::file_pool_size, 60);

        // Sin final estricto
        pack->set_bool(lt::settings_pack::strict_end_game_mode, false);

        // Redes de pares
        pack->set_bool(lt::settings_pack::enable_dht, true);
        pack->set_bool(lt::settings_pack::enable_lsd, true);
        pack->set_bool(lt::settings_pack::enable_upnp, true);
        pack->set_bool(lt::settings_pack::enable_natpmp, true);

        return pack;
    }

} // extern "C"