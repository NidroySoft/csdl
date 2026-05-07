// settings.cpp - Compatible con magic_enum 0.9+ y libtorrent 2.0
#include "settings.h"
#include <magic_enum/magic_enum.hpp>
#include <functional>
#include <string>

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

template <typename T>
typename std::enable_if<std::is_enum<T>::value, bool>::type
set_value(const char* key, std::function<bool(T)> setter) {
    if (!key) return false;
    auto enum_key = magic_enum::enum_cast<T>(key, magic_enum::case_insensitive);
    if (!enum_key.has_value()) return false;
    return setter(enum_key.value());
}

lt::settings_pack* create_settings_pack() {
    return new lt::settings_pack;
}

void destroy_settings_pack(lt::settings_pack* pack) {
    delete pack;
}

uint8_t settings_pack_set_str(lt::settings_pack* pack, const char* key, const char* value) {
    if (!pack) return false;
    return set_value<lt::settings_pack::string_types>(key, [pack, value](lt::settings_pack::string_types k) {
        if (!value) return false;
        pack->set_str(k, std::string(value));
        return true;
        });
}

uint8_t settings_pack_set_bool(lt::settings_pack* pack, const char* key, uint8_t value) {
    if (!pack) return false;
    return set_value<lt::settings_pack::bool_types>(key, [pack, value](lt::settings_pack::bool_types k) {
        pack->set_bool(k, static_cast<bool>(value));
        return true;
        });
}

uint8_t settings_pack_set_int(lt::settings_pack* pack, const char* key, int value) {
    if (!pack) return false;
    return set_value<lt::settings_pack::int_types>(key, [pack, value](lt::settings_pack::int_types k) {
        pack->set_int(k, value);
        return true;
        });
}

// ✏️ Nuevo: función que devuelve un settings_pack optimizado para streaming
lt::settings_pack* create_streaming_settings() {
    auto* pack = new lt::settings_pack;
    // Velocidad de conexión asumida alta => acelera la rotación de pares lentos
    pack->set_int(lt::settings_pack::connection_speed, 200);
    // Impulso extra de conexiones durante el arranque
    pack->set_int(lt::settings_pack::torrent_connect_boost, 100);
    // Reintentos rápidos de conexión
    pack->set_int(lt::settings_pack::min_reconnect_time, 1);
    // Timeout corto para establecer conexión con un par
    pack->set_int(lt::settings_pack::peer_connect_timeout, 3);
    // Timeouts agresivos para detectar pares problemáticos
    pack->set_int(lt::settings_pack::request_timeout, 10);
    pack->set_int(lt::settings_pack::piece_timeout, 10);
    pack->set_int(lt::settings_pack::peer_timeout, 10);
    pack->set_int(lt::settings_pack::urlseed_timeout, 10);
    // Aumentar los hilos de E/S asíncrona (relevante para streaming)
    pack->set_int(lt::settings_pack::aio_threads, 8);
    // Tamaño del pool de archivos abiertos (mejora acceso secuencial)
    pack->set_int(lt::settings_pack::file_pool_size, 41);
    // Búsqueda activa de pares
    pack->set_bool(lt::settings_pack::enable_dht, true);
    pack->set_bool(lt::settings_pack::enable_lsd, true);
    pack->set_bool(lt::settings_pack::enable_upnp, true);
    pack->set_bool(lt::settings_pack::enable_natpmp, true);
    // Desactivar suavizado de conexiones para inicio rápido
    pack->set_bool(lt::settings_pack::smooth_connects, false);
    return pack;
}