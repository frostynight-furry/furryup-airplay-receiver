/* Exercise the real mirror thread without requiring an iPhone or OBS. */
#include "../deps/UxPlay/lib/raop_rtp_mirror.c"

static volatile LONG accepted_count;
static void capture(void *unused, int level, const char *message) {
    (void)unused; (void)level;
    if (strstr(message, "video TCP connection accepted"))
        InterlockedIncrement(&accepted_count);
}

int main(void) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) return 1;
    logger_t *log = logger_init();
    logger_set_level(log, LOGGER_INFO);
    logger_set_callback(log, capture, NULL);
    DWORD baseline = 0, final_count = 0;
    GetProcessHandleCount(GetCurrentProcess(), &baseline);
    for (int cycle = 0; cycle < 60; ++cycle) {
        raop_rtp_mirror_t mirror = {0};
        mirror.logger = log;
        mirror.joined = 1;
        mirror.mirror_data_sock = -1;
        mirror.remote_saddr.ss_family = AF_INET;
        MUTEX_CREATE(mirror.run_mutex);
        unsigned short port = 0;
        LONG previous = accepted_count;
        raop_rtp_mirror_start(&mirror, &port, 0);
        if (!port || !mirror.running) return 2;
        SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        struct sockaddr_in address = {0};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port);
        if (connect(client, (struct sockaddr *)&address, sizeof(address))) return 3;
        ULONGLONG deadline = GetTickCount64() + 2000;
        while (accepted_count == previous && GetTickCount64() < deadline) Sleep(1);
        if (accepted_count == previous) return 4;
        if (cycle % 3 == 0) {
            // A partial header must not block stop/join indefinitely.
            char partial[7] = {0};
            send(client, partial, sizeof(partial), 0);
            Sleep(10);
        } else {
            if (cycle % 3 == 1) shutdown(client, SD_SEND);
            else { struct linger reset = {1, 0}; setsockopt(client, SOL_SOCKET, SO_LINGER, (char *)&reset, sizeof(reset)); }
            closesocket(client);
            client = INVALID_SOCKET;
            deadline = GetTickCount64() + 2000;
            for (;;) {
                MUTEX_LOCK(mirror.run_mutex);
                int running = mirror.running;
                MUTEX_UNLOCK(mirror.run_mutex);
                if (!running) break;
                if (GetTickCount64() >= deadline) return 5;
                Sleep(1);
            }
        }
        ULONGLONG start = GetTickCount64();
        raop_rtp_mirror_stop(&mirror);
        if (GetTickCount64() - start > 1000 || !mirror.joined || mirror.mirror_data_sock != -1) return 6;
        // Repeated stop must be harmless even after the worker already exited.
        raop_rtp_mirror_stop(&mirror);
        if (client != INVALID_SOCKET) closesocket(client);
        MUTEX_DESTROY(mirror.run_mutex);
        // Winsock and pthreads initialize a few process-wide handles lazily.
        if (cycle == 2) GetProcessHandleCount(GetCurrentProcess(), &baseline);
    }
    GetProcessHandleCount(GetCurrentProcess(), &final_count);
    logger_destroy(log);
    WSACleanup();
    if (final_count > baseline + 4) {
        fprintf(stderr, "handle leak: %lu -> %lu\n", baseline, final_count);
        return 7;
    }
    printf("PASS: 60 real TCP mirror cycles (EOF, reset, partial header); handles %lu -> %lu\n", baseline, final_count);
    return 0;
}
