#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <termios.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/select.h>

#include <csp/csp_debug.h>
#include <csp/csp.h>
#include <csp/drivers/usart.h>

#include "csp_posix_helper.h"

#define GS_ADDRESS       10     // Dirección CSP de la Ground Station
#define ROVER_ADDRESS    20     // Dirección CSP del rover
#define SERVER_PORT      10     // Puerto usado para enviar/recibir datos
#define DEVICE_NAME      "/dev/ttyACM0"
#define TIMEOUT_MS       1000

void set_input_mode(void) {
    struct termios tattr;
    tcgetattr(STDIN_FILENO, &tattr);
    tattr.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &tattr);
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
}

void reset_input_mode(void) {
    struct termios tattr;
    tcgetattr(STDIN_FILENO, &tattr);
    tattr.c_lflag |= (ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &tattr);
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    flags &= ~O_NONBLOCK;
    fcntl(STDIN_FILENO, F_SETFL, flags);
}

void *server_thread(void *param) {
    (void)param;
    csp_print("Servidor iniciado.\n");

    csp_socket_t sock = {0};
    csp_bind(&sock, SERVER_PORT);
    csp_listen(&sock, 10);

    while (1) {
        csp_conn_t *conn = csp_accept(&sock, 1000);
        if (!conn) continue;

        csp_packet_t *packet;
        while ((packet = csp_read(conn, 50)) != NULL) {
            char *msg = (char *) packet->data;
            msg[strcspn(msg, "\r\n")] = '\0';
            csp_print("Respuesta recibida: %s\n", msg);

            csp_buffer_free(packet);
        }

        csp_close(conn);
    }

    return NULL;
}

// ... (mismo código hasta set_input_mode y server_thread)

int main(void) {
    csp_iface_t *default_iface;
    unsigned int count = 'A';

    csp_print("Inicializando CSP\n");
    csp_init();

    csp_usart_conf_t conf = {
        .device = DEVICE_NAME,
        .baudrate = 57600,
        .databits = 8,
        .stopbits = 1,
        .paritysetting = 0,
    };

    if (csp_usart_open_and_add_kiss_interface(&conf, CSP_IF_KISS_DEFAULT_NAME, GS_ADDRESS, &default_iface) != CSP_ERR_NONE) {
        csp_print("Error: no se pudo agregar la interfaz KISS\n");
        return EXIT_FAILURE;
    }

    default_iface->is_default = 1;
    router_start();

#if (CSP_USE_RTABLE)
    csp_rtable_set(0, 0, default_iface, CSP_NO_VIA_ADDRESS);
#endif

    pthread_t server_tid;
    pthread_create(&server_tid, NULL, server_thread, NULL);

    set_input_mode();
    csp_print("GS lista. Presiona tecla:\n"
              "  't' -> temperatura\n"
              "  'h' -> humedad\n"
              "  's' -> solicitar dato\n"
              "  otra -> mensaje genérico\n"
              "  'q' -> salir\n");

    while (1) {
        fd_set readfds;
        struct timeval tv;

        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);
        tv.tv_sec = 0;
        tv.tv_usec = 100000;

        int ret = select(STDIN_FILENO + 1, &readfds, NULL, NULL, &tv);

        if (ret > 0 && FD_ISSET(STDIN_FILENO, &readfds)) {
            char c;
            ssize_t n = read(STDIN_FILENO, &c, 1);
            if (n > 0) {
                if (c == 'q') break;

                const char *mensaje;

                if (c == 't') {
                    mensaje = "modo temperatura";
                } else if (c == 'h') {
                    mensaje = "modo humedad";
                } else if (c == 's') {
                    mensaje = "solicitar dato";
                } else {
                    static char buffer[32];
                    snprintf(buffer, sizeof(buffer), "Hello world %c", count++);
                    mensaje = buffer;
                }

                csp_print("Enviando: %s\n", mensaje);

                csp_conn_t *conn = csp_connect(CSP_PRIO_NORM, ROVER_ADDRESS, SERVER_PORT, TIMEOUT_MS, CSP_O_RDP);
                if (!conn) {
                    csp_print("Error: no se pudo establecer conexión con el rover\n");
                    continue;
                }

                csp_packet_t *packet = csp_buffer_get(0);
                if (!packet) {
                    csp_print("Error: no se pudo obtener buffer\n");
                    csp_close(conn);
                    continue;
                }

                snprintf((char *)packet->data, CSP_BUFFER_SIZE, "%s", mensaje);
                packet->length = strlen((char *)packet->data) + 1;

                csp_send(conn, packet);
                csp_close(conn);
            }
        }
    }

    reset_input_mode();
    csp_print("Cliente finalizado\n");
    return EXIT_SUCCESS;
}

