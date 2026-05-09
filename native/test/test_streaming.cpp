// test_streaming.cpp – Prueba de streaming con inicio automático de VLC cuando hay buffer
#define CSDL_STATIC
#include "streaming.h"
#include "settings.h"          // Para create_streaming_settings()
#include <libtorrent/session.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/file_storage.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/torrent_status.hpp>
#include <iostream>
#include <string>
#include <atomic>
#include <thread>
#include <chrono>
#include <filesystem>
#include <exception>
#include <iomanip>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

// Abre la URL en VLC (multiplataforma)
static void open_in_vlc(const std::string& url) {
    std::cout << "\n[VLC] Abriendo " << url << " ...\n";
#ifdef _WIN32
    std::string cmd = "start vlc " + url;
    system(cmd.c_str());
#else
    std::string cmd = "vlc " + url + " &";
    system(cmd.c_str());
#endif
}

int main(int argc, char* argv[])
{
    if (argc < 2) {
        std::cerr << "Uso: test_streaming <archivo.torrent> [puerto] [directorio_descarga]\n";
        std::cin.get();
        return 1;
    }

    std::string torrentFile = argv[1];
    int port = (argc >= 3) ? std::stoi(argv[2]) : 55201;
    std::string downloadDir = (argc >= 4) ? argv[3] : "./downloads";

    try {
        // ── Preparar directorio de descarga ────────────────────────────
        std::error_code ec;
        std::filesystem::remove_all(downloadDir, ec);
        std::filesystem::create_directories(downloadDir, ec);
        std::cout << "Descargas: " << downloadDir << " (limpio)\n";

        // ── Configuración de sesión ────────────────────────────────────
        lt::settings_pack pack = *create_streaming_settings();
        pack.set_int(lt::settings_pack::alert_mask,
            lt::alert_category::error | lt::alert_category::status);
        lt::session ses(pack);

        // ── Cargar torrent ─────────────────────────────────────────────
        std::shared_ptr<lt::torrent_info> ti;
        try {
            ti = std::make_shared<lt::torrent_info>(torrentFile);
        }
        catch (lt::system_error const& e) {
            std::cerr << "Error al cargar torrent: " << e.what() << std::endl;
            std::cin.get();
            return 1;
        }

        // ── Seleccionar el archivo más grande ──────────────────────────
        int videoIdx = 0;
        std::int64_t maxSize = 0;
        for (int i = 0; i < ti->num_files(); ++i) {
            auto sz = ti->files().file_size(lt::file_index_t{ i });
            if (sz > maxSize) {
                maxSize = sz;
                videoIdx = i;
            }
        }
        std::string fileName = std::string(ti->files().file_name(lt::file_index_t{ videoIdx }));
        std::cout << "Archivo: " << fileName << " (" << maxSize / 1024 / 1024 << " MiB)\n";

        // ── Añadir torrent ─────────────────────────────────────────────
        lt::add_torrent_params params;
        params.ti = ti;
        params.save_path = downloadDir;
        params.storage_mode = lt::storage_mode_sparse;
        params.flags &= ~lt::torrent_flags::auto_managed;
        lt::torrent_handle handle = ses.add_torrent(params);
        handle.resume();

        // ── Hilo de alertas (solo errores y finalización) ──────────────
        std::atomic<bool> running = true;
        std::thread alert_thread([&]() {
            while (running) {
                std::vector<lt::alert*> alerts;
                ses.pop_alerts(&alerts);
                for (auto* a : alerts) {
                    if (a->category() & lt::alert_category::error)
                        std::cerr << "[ERROR] " << a->message() << std::endl;
                    if (lt::alert_cast<lt::torrent_finished_alert>(a))
                        std::cout << "\n[COMPLETADO] Descarga finalizada.\n";
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            });

        // ── Iniciar servidor streaming ─────────────────────────────────
        std::string url;
        if (!cs_stream::start_server(&ses, &handle, videoIdx, port, url)) {
            std::cerr << "Error: " << cs_stream::last_error() << std::endl;
            running = false;
            alert_thread.join();
            std::cin.get();
            return 1;
        }

        std::cout << ">>> SERVIDOR LISTO <<<\n"
            << "URL: " << url << "\n"
            << "Status: http://127.0.0.1:" << port << "/status\n";

        // ── Esperar buffer inicial antes de abrir VLC ──────────────────
        constexpr std::int64_t MIN_BUFFER_BYTES = 5 * 1024 * 1024;  // 5 MiB
        constexpr double MIN_BUFFER_PCT = 2.0;                       // 2%

        std::cout << "Esperando buffer inicial (" << MIN_BUFFER_BYTES / 1024 / 1024 << " MiB o "
            << MIN_BUFFER_PCT << "%) para abrir VLC...\n";

        bool vlc_opened = false;
        std::thread vlc_thread([&]() {
            while (running && !vlc_opened) {
                std::vector<std::int64_t> fprog;
                handle.file_progress(fprog);
                std::int64_t downloaded = (videoIdx < (int)fprog.size()) ? fprog[videoIdx] : 0;
                double pct = (maxSize > 0) ? 100.0 * downloaded / maxSize : 0.0;
                if (downloaded >= MIN_BUFFER_BYTES || pct >= MIN_BUFFER_PCT) {
                    open_in_vlc(url);
                    vlc_opened = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
            });
        vlc_thread.detach(); // no necesitamos esperarlo

        // ── Hilo de progreso ───────────────────────────────────────────
        std::thread progress_thread([&]() {
            while (running) {
                std::vector<std::int64_t> fprog;
                handle.file_progress(fprog);
                std::int64_t dl = (videoIdx < (int)fprog.size()) ? fprog[videoIdx] : 0;
                double pct = (maxSize > 0) ? 100.0 * dl / maxSize : 0.0;
                std::cout << "\rProgreso: " << std::fixed << std::setprecision(1)
                    << pct << "% (" << dl / 1024 / 1024 << " MiB)  " << std::flush;
                std::this_thread::sleep_for(std::chrono::seconds(2));
            }
            std::cout << std::endl;
            });

        std::cout << "\nPresiona ENTER para detener...\n";
        std::cin.get();

        running = false;
        progress_thread.join();
        alert_thread.join();

        cs_stream::stop_server();
        std::cout << "Servidor detenido.\n";
        ses.pause();
        std::cout << "Sesión pausada.\n";
    }
    catch (const std::exception& e) {
        std::cerr << "Excepción: " << e.what() << std::endl;
        std::cin.get();
        return 1;
    }

    return 0;
}