#include <stdio.h>
#include <csp/csp_debug.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <getopt.h>
#include <time.h>

#include <csp/csp.h>
#include <csp/drivers/usart.h>
#include <csp/drivers/can_socketcan.h>
#include <csp/interfaces/csp_if_zmqhub.h>

#include "csp_posix_helper.h"

#define SERVER_PORT     10

static uint8_t server_address = 10;
static unsigned int server_received = 0;
static struct timespec start_time;

/* ------------------------------------------------------------------
   NOTE SOBRE RDP
   ---------------------------------------------------------------
   En versiones recientes de libcsp no es necesario (ni existe) la
   llamada `csp_socket_set_options(&sock, CSP_SO_RDP)` para aceptar
   paquetes RDP.  Basta con que el remitente use CSP_O_RDP y que el
   receptor escuche en el puerto adecuado; la capa RDP se encarga de
   reensamblar y entregar el payload al `csp_read()` normal.  Por eso
   se elimina esa línea que causaba error de compilación.
   ------------------------------------------------------------------*/

void * server(void * param) {
    (void)param;
    csp_print("Server task started\n");

    clock_gettime(CLOCK_MONOTONIC, &start_time);

    FILE *log_file = fopen("received_packets.log", "w");
    if (!log_file) {
        perror("fopen");
        return NULL;
    }

    csp_socket_t sock = {0};
    /* Escuchar exclusivamente en SERVER_PORT para evitar ambigüedades */
    csp_bind(&sock, SERVER_PORT);
    csp_listen(&sock, 10);

    while (1) {
        csp_conn_t *conn = csp_accept(&sock, 10000);
        if (conn == NULL) continue;

        csp_packet_t *packet;
        while ((packet = csp_read(conn, 200)) != NULL) { /* 200 ms para tramas grandes */
            if (csp_conn_dport(conn) == SERVER_PORT) {
                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                double elapsed = (now.tv_sec - start_time.tv_sec) +
                                 (now.tv_nsec - start_time.tv_nsec) / 1e9;

                csp_print("Packet #%03u [%.3fs]: %s\n", server_received + 1,
                          elapsed, (char *)packet->data);
                fprintf(log_file, "Packet #%03u [%.3fs]: %s\n",
                        server_received + 1, elapsed, (char *)packet->data);
                fflush(log_file);

                ++server_received;
                csp_buffer_free(packet);
            } else {
                csp_service_handler(packet);
            }
        }
        csp_close(conn);
    }

    fclose(log_file);
    return NULL;
}

int main(int argc, char *argv[]) {
    const char *device_name = "/dev/ttyACM0";  /* Ajusta a tu entorno */
    uint8_t address = 10;

    /* Si tu versión de libcsp soporta cambiar los buffers en tiempo de
       ejecución, podés descomentar y compilar con la opción adecuada.
       Caso contrario, esos valores se fijan en tiempo de compilación.

    // extern csp_conf_t csp_conf;
    // csp_conf.buffer_data_size = 300;
    // csp_conf.buffer_count     = 50;
    */

    csp_init();
    router_start();

    csp_iface_t *iface;
    csp_usart_conf_t conf = {
        .device = device_name,
        .baudrate = 57600,
        .databits = 8,
        .stopbits = 1,
        .paritysetting = 0,
    };

    int error = csp_usart_open_and_add_kiss_interface(&conf, "KISS", address, &iface);
    if (error != CSP_ERR_NONE) {
        csp_print("Failed to init KISS interface\n");
        return 1;
    }

    iface->is_default = 1;
    csp_rtable_set(0, 0, iface, CSP_NO_VIA_ADDRESS);

    csp_pthread_create(server);

    while (1) {
        sleep(10);
    }

    return 0;
}

