// test_streaming.cpp – Abre VLC en cuanto la primera pieza está en RAM
#include "streaming.h"
#include "settings.h"
#include <libtorrent/session.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/add_torrent_params.hpp>
#include <iostream>
#include <atomic>
#include <thread>
#include <chrono>
#include <filesystem>
#include <vector>

static void open_in_vlc(const std::string& url) {
    std::cout << "\n[VLC] Abriendo " << url << " ...\n";
#ifdef _WIN32
    std::string cmd = "start \"\" \"C:\\Program Files\\VideoLAN\\VLC\\vlc.exe\" " + url + " --network-caching=5000 --http-reconnect";
    system(cmd.c_str());
#else
    system(("vlc " + url + " --network-caching=5000 &").c_str());
#endif
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Uso: test_streaming <archivo.torrent> [puerto]\n";
        return 1;
    }

    std::string torrentFile = argv[1];
    int port = (argc >= 3) ? std::stoi(argv[2]) : 55201;
    std::string downloadDir = "./downloads";

    try {
        std::error_code ec;
        std::filesystem::remove_all(downloadDir, ec);
        std::filesystem::create_directories(downloadDir, ec);
        std::cout << "Directorio de descarga limpio.\n";

        lt::session ses(*create_streaming_settings());
        auto ti = std::make_shared<lt::torrent_info>(torrentFile);

        int videoIdx = 0;
        std::int64_t maxSize = 0;
        for (int i = 0; i < ti->num_files(); ++i) {
            auto sz = ti->files().file_size(lt::file_index_t{ i });
            if (sz > maxSize) {
                maxSize = sz;
                videoIdx = i;
            }
        }

        std::cout << "Archivo: " << ti->files().file_name(lt::file_index_t{ videoIdx })
            << " (" << maxSize / (1024 * 1024) << " MiB)\n";

        lt::add_torrent_params params;
        params.ti = ti;
        params.save_path = downloadDir;
        params.storage_mode = lt::storage_mode_sparse;

        auto handle = ses.add_torrent(params);
        handle.resume();

        std::string url;
        if (!cs_stream::start_server(&ses, &handle, videoIdx, port, url)) {
            std::cerr << "Error iniciando servidor: " << cs_stream::last_error() << std::endl;
            return 1;
        }

        std::cout << ">>> SERVIDOR LISTO <<<\nURL: " << url << "\n";

        // ── Abrir VLC en cuanto la primera pieza esté en RAM ──────────
        std::cout << "Esperando buffer inicial (primera pieza)...\n";
        bool vlc_opened = false;
        while (!vlc_opened) {
            // La primera pieza ya está en RAM gracias a la precarga de start_server
            if (cs_stream::is_byte_available(&handle, videoIdx, 0)) {
                open_in_vlc(url);
                vlc_opened = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        std::cout << "\nPresiona ENTER para detener...\n";
        std::cin.get();

        cs_stream::stop_server();
        std::cout << "Servidor detenido.\n";
    }
    catch (const std::exception& e) {
        std::cerr << "Excepción: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}