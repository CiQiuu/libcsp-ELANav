#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <getopt.h>
#include <time.h>

#include <csp/csp.h>
#include <csp/csp_sfp.h>
#include <csp/csp_debug.h>
#include <csp/drivers/usart.h>

#include "csp_posix_helper.h"   /* router_start() */

#define SERVER_PORT 10

int main(int argc, char *argv[]) {
    const char *device = NULL;
    int opt;

    while ((opt = getopt(argc, argv, "k:")) != -1) {
        if (opt == 'k') device = optarg;
    }
    if (!device) {
        printf("Uso: %s -k <device>\n", argv[0]);
        return 1;
    }

    srand((unsigned int)time(NULL));

    printf("=== ROVER START ===\n");
    printf("Device: %s  addr=20  port=%d\n", device, SERVER_PORT);

    /* RDP — timeouts amplios para NinoTNC half-duplex a 9600 baud */
    csp_dbg_rdp_print = 2;
    csp_rdp_set_opt(4,      /* window_size      */
                    60000,  /* conn_timeout_ms  */
                    60000,  /* packet_timeout_ms */
                    0,      /* delayed_acks     */
                    2000,   /* ack_timeout_ms   */
                    1);     /* ack_delay_count  */

    csp_init();
    router_start();

    csp_iface_t *iface;
    csp_usart_conf_t conf = {
        .device        = device,
        .baudrate      = 57600,
        .databits      = 8,
        .stopbits      = 1,
        .paritysetting = 0,
    };

    if (csp_usart_open_and_add_kiss_interface(&conf, "KISS", 20, &iface) != CSP_ERR_NONE) {
        printf("ERROR: no se pudo abrir KISS en %s\n", device);
        return 1;
    }
    iface->is_default = 1;
    csp_rtable_set(0, 0, iface, CSP_NO_VIA_ADDRESS);

    csp_socket_t sock = {0};
    csp_bind(&sock, SERVER_PORT);
    csp_listen(&sock, 10);

    printf("ROVER READY — esperando conexion en puerto %d\n", SERVER_PORT);

    while (1) {
        csp_conn_t *conn = csp_accept(&sock, CSP_MAX_TIMEOUT);
        if (!conn) continue;

        printf("CONNECTED — src=%d\n", csp_conn_src(conn));

        csp_packet_t *packet;
        while ((packet = csp_read(conn, CSP_MAX_TIMEOUT)) != NULL) {

            char *cmd = (char *)packet->data;
            printf("CMD: [%s]\n", cmd);

            /* ── Imagen: bloque SFP ─────────────────────────────── */
            if (strcmp(cmd, "i") == 0) {
                printf("Enviando imagen via SFP...\n");
                FILE *f = fopen("rover_test.jpg", "rb");
                if (!f) {
                    printf("ERROR: rover_test.jpg no encontrado\n");
                    csp_packet_t *resp = csp_buffer_get(0);
                    if (resp) {
                        strcpy((char *)resp->data, "ERR: NO IMG");
                        resp->length = strlen((char *)resp->data) + 1;
                        csp_send(conn, resp);
                    }
                } else {
                    fseek(f, 0, SEEK_END);
                    long img_size = ftell(f);
                    rewind(f);
                    char *img = malloc((size_t)img_size);
                    if (img) {
                        if (fread(img, 1, (size_t)img_size, f) == (size_t)img_size) {
                            printf("Imagen cargada: %ld bytes — enviando SFP MTU=128...\n",
                                   img_size);
                            int err = csp_sfp_send(conn, img, (unsigned int)img_size,
                                                   128, 7200000);
                            printf("SFP send result: %d\n", err);
                        } else {
                            printf("ERROR: fread incompleto\n");
                        }
                        free(img);
                    }
                    fclose(f);
                }
                csp_buffer_free(packet);
                continue;   /* siguiente paquete en la misma conexion */
            }

            /* ── Sensores: paquete CSP simple ───────────────────── */
            char respuesta[128];

            if (strcmp(cmd, "t") == 0) {
                snprintf(respuesta, sizeof(respuesta),
                         "TEMP: %.2f C", 20.0 + (rand() % 100) / 10.0);

            } else if (strcmp(cmd, "h") == 0) {
                snprintf(respuesta, sizeof(respuesta),
                         "HUM: %.2f %%", 40.0 + (rand() % 500) / 10.0);

            } else if (strcmp(cmd, "s") == 0) {
                snprintf(respuesta, sizeof(respuesta),
                         "T=%.2f H=%.2f",
                         20.0 + (rand() % 100) / 10.0,
                         40.0 + (rand() % 500) / 10.0);

            } else {
                snprintf(respuesta, sizeof(respuesta), "UNKNOWN: %s", cmd);
            }

            printf("RSP: %s\n", respuesta);

            csp_packet_t *resp = csp_buffer_get(0);
            if (resp) {
                snprintf((char *)resp->data, CSP_BUFFER_SIZE, "%s", respuesta);
                resp->length = strlen((char *)resp->data) + 1;
                csp_send(conn, resp);
            }

            csp_buffer_free(packet);
        }

        printf("DISCONNECTED\n");
        csp_close(conn);
    }

    return 0;
}
