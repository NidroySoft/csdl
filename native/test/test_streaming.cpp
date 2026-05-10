// test_streaming_rich.cpp – Prueba profesional con feedback en tiempo real
// Uso: test_streaming_rich <archivo.torrent> [puerto]

#include "streaming.h"
#include "settings.h"
#include <libtorrent/session.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/add_torrent_params.hpp>

#include <iostream>
#include <iomanip>
#include <thread>
#include <chrono>
#include <filesystem>
#include <ctime>
#include <sstream>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

// ── Funciones auxiliares ─────────────────────────────────────────────────

static std::string current_time_str() {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::tm local_tm;
#ifdef _WIN32
    localtime_s(&local_tm, &time_t_now);
#else
    localtime_r(&time_t_now, &local_tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&local_tm, "%H:%M:%S");
    return oss.str();
}

static void open_in_vlc(const std::string& url) {
    std::cout << "\n[VLC] Abriendo " << url << " ...\n";
#ifdef _WIN32
    std::string cmd = "start \"\" \"C:\\Program Files\\VideoLAN\\VLC\\vlc.exe\" "
        + url + " --network-caching=3000 --http-reconnect";
    system(cmd.c_str());
#else
    system(("vlc " + url + " --network-caching=3000 &").c_str());
#endif
}

// ── Programa principal ───────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Uso: test_streaming_rich <archivo.torrent> [puerto]\n";
        return 1;
    }

    std::string torrentFile = argv[1];
    int port = (argc >= 3) ? std::stoi(argv[2]) : 55201;
    std::string downloadDir = "./downloads";

    try {
        // 1. Limpiar directorio de descargas
        std::error_code ec;
        std::filesystem::remove_all(downloadDir, ec);
        std::filesystem::create_directories(downloadDir, ec);
        std::cout << "Directorio de descarga limpio.\n";

        // 2. Crear sesión con los ajustes nativos optimizados para streaming
        lt::session ses(*create_streaming_settings());
        auto ti = std::make_shared<lt::torrent_info>(torrentFile);

        // 3. Seleccionar el archivo más grande (vídeo)
        int videoIdx = 0;
        std::int64_t maxSize = 0;
        for (int i = 0; i < ti->num_files(); ++i) {
            auto sz = ti->files().file_size(lt::file_index_t{ i });
            if (sz > maxSize) {
                maxSize = sz;
                videoIdx = i;
            }
        }

        std::int64_t fileSize = ti->files().file_size(lt::file_index_t{ videoIdx });
        std::int64_t fileOffset = ti->files().file_offset(lt::file_index_t{ videoIdx });
        int pieceSize = ti->piece_length();
        int firstPieceIdx = static_cast<int>(fileOffset / pieceSize);
        int lastPieceIdx = static_cast<int>((fileOffset + fileSize - 1) / pieceSize);
        int totalPieces = lastPieceIdx - firstPieceIdx + 1;

        std::cout << "Archivo: " << ti->files().file_name(lt::file_index_t{ videoIdx })
            << " (" << fileSize / (1024 * 1024) << " MiB)\n";
        std::cout << "Tamaño de pieza: " << pieceSize << " bytes | "
            << "Piezas: " << totalPieces
            << " (offset " << firstPieceIdx << ")\n";
        std::cout << std::string(60, '-') << std::endl;

        // 4. Añadir el torrent (pausado inicialmente)
        lt::add_torrent_params params;
        params.ti = ti;
        params.save_path = downloadDir;
        params.storage_mode = lt::storage_mode_sparse;
        params.flags |= lt::torrent_flags::paused;

        auto handle = ses.add_torrent(params);

        // 5. Iniciar el servidor de streaming ANTES de reanudar la descarga
        std::string url;
        if (!cs_stream::start_server(&ses, &handle, videoIdx, port, url)) {
            std::cerr << "Error iniciando servidor: " << cs_stream::last_error() << std::endl;
            return 1;
        }
        std::cout << "Servidor streaming: " << url << std::endl;

        // 6. Ahora sí, reanudar la descarga
        handle.resume();
        std::cout << "Descarga iniciada.\n" << std::endl;

        // 7. Monitorización en tiempo real
        bool firstPieceReady = false;
        bool lastPieceReady = false;
        bool vlcOpened = false;
        auto startTime = std::chrono::steady_clock::now();
        int lastLoggedProgress = -1;

        while (!firstPieceReady || !lastPieceReady) {
            auto st = handle.status();
            double pct = st.progress * 100.0;
            std::int64_t dlRate = st.download_payload_rate;
            std::int64_t downloaded = st.total_payload_download;
            int peers = st.num_peers;
            int seeds = st.num_seeds;

            // Verificar disponibilidad de primera/última pieza
            bool firstNow = cs_stream::is_byte_available(&handle, videoIdx, 0);
            bool lastNow = cs_stream::is_byte_available(&handle, videoIdx,
                fileSize > 0 ? fileSize - 1 : 0);

            if (firstNow && !firstPieceReady) {
                firstPieceReady = true;
                std::cout << "[" << current_time_str() << "] "
                    << "✅ Primera pieza lista (byte 0 disponible)\n";

                if (!vlcOpened) {
                    open_in_vlc(url);
                    vlcOpened = true;
                }
            }

            if (lastNow && !lastPieceReady) {
                lastPieceReady = true;
                std::cout << "[" << current_time_str() << "] "
                    << "✅ Última pieza lista (byte "
                    << fileSize - 1 << " disponible)\n";
            }

            // Mostrar progreso cada 5% o en eventos
            int currentProgress = static_cast<int>(pct);
            if (currentProgress >= lastLoggedProgress + 5 ||
                firstNow || lastNow) {
                lastLoggedProgress = currentProgress - (currentProgress % 5);
                std::cout << "[" << current_time_str() << "] "
                    << "Progreso: " << std::setw(5) << std::fixed << std::setprecision(1) << pct << "% | "
                    << "DL: " << std::setw(6) << std::setprecision(1) << dlRate / 1024.0 << " KB/s | "
                    << "Peers: " << std::setw(4) << peers << " | "
                    << "Seeds: " << std::setw(4) << seeds << " | "
                    << "Descargado: " << std::setw(6) << std::fixed << std::setprecision(1)
                    << downloaded / (1024.0 * 1024.0) << " MB | "
                    << "Primera: " << (firstPieceReady ? "✅" : "⏳") << " | "
                    << "Última: " << (lastPieceReady ? "✅" : "⏳") << "\n";
            }

            if (firstPieceReady && lastPieceReady)
                break;

            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

        // 8. Resultados finales
        auto endTime = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(endTime - startTime).count();
        auto finalStatus = handle.status();

        std::cout << std::string(60, '-') << std::endl;
        std::cout << "Buffer inicial completado!\n";
        std::cout << "Tiempo transcurrido: " << elapsed << " segundos\n";
        std::cout << "Progreso final: " << finalStatus.progress * 100.0 << "%\n";
        std::cout << "URL: " << url << std::endl;
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