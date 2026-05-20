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
    /* Salida sin buffer para que journalctl y SSH redirect vean los logs
     * en tiempo real. Critico durante la campana de medicion en RF.       */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    const char *device = NULL;
    int packet_timeout_ms = 20000;   /* default: configuracion A2 validada  */
    int opt;

    while ((opt = getopt(argc, argv, "k:t:")) != -1) {
        switch (opt) {
            case 'k': device = optarg; break;
            case 't': packet_timeout_ms = atoi(optarg); break;
            default:
                printf("Uso: %s -k <device> [-t <packet_timeout_ms>]\n", argv[0]);
                return 1;
        }
    }
    if (!device) {
        printf("Uso: %s -k <device> [-t <packet_timeout_ms>]\n", argv[0]);
        return 1;
    }
    if (packet_timeout_ms < 1000 || packet_timeout_ms > 120000) {
        printf("ERROR: packet_timeout_ms fuera de rango [1000, 120000]: %d\n",
               packet_timeout_ms);
        return 1;
    }

    srand((unsigned int)time(NULL));

    printf("=== ROVER START ===\n");
    printf("Device: %s  addr=20  port=%d  packet_timeout_ms=%d\n",
           device, SERVER_PORT, packet_timeout_ms);

    /*
     * RDP — configuracion validada para NinoTNC half-duplex a 9600 baud
     *
     * window_size=4:      BDP = 960 B/s * 0.838 s ≈ 804 B
     *                     804 / 162 B por trama ≈ 5 → usar 4 (probado)
     *                     window=8 supera el BDP y genera retransmisiones
     *                     masivas (8 buenos + 8 duplicados ciclicamente).
     *
     * packet_timeout=15000ms: debe ser > RTT bajo congestion (2000-4000 ms).
     *                         Valor validado experimentalmente: mejora 6.74x.
     *
     * conn_timeout=60000ms:   cubre transferencias de imagen de larga duracion.
     *
     * delayed_acks=0:     ACK inmediato. Critico en half-duplex para evitar
     *                     que el timeout expire mientras el ACK espera en cola.
     */
    csp_dbg_rdp_print = 2;
    csp_rdp_set_opt(4,                  /* window_size       */
                    60000,              /* conn_timeout_ms   */
                    packet_timeout_ms,  /* packet_timeout_ms */
                    0,                  /* delayed_acks      */
                    2000,               /* ack_timeout_ms    */
                    1);                 /* ack_delay_count   */

    csp_init();

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
    router_start();

    csp_socket_t sock = {0};
    csp_bind(&sock, CSP_ANY);
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

            /* ── Imagen: bloque SFP ─────────────────────────────────────*/

            if (strcmp(cmd, "i") == 0) {
                printf("Enviando imagen via SFP...\n");

                /* Buscar rover_test.jpg primero en ruta de produccion (Yocto),
                 * luego en cwd (desarrollo en laptop) */
                FILE *f = fopen("/usr/share/elanav/rover_test.jpg", "rb");
                if (!f) {
                    f = fopen("rover_test.jpg", "rb");
                }
                if (!f) {
                    printf("ERROR: rover_test.jpg no encontrado en /usr/share/elanav/ ni en cwd\n");
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
                            uint32_t mtu = 200; /* CSP_BUFFER_SIZE(256) - CSP(4) - SFP(8) - RDP(~44) */ /* AX.25(256) - CSP(4) - SFP(8) */
                            printf("Imagen: %ld B — SFP MTU=%u — "
                                   "fragmentos estimados: %u\n",
                                   img_size, mtu,
                                   (unsigned int)((img_size + mtu - 1) / mtu));

                            int err = csp_sfp_send(conn, img,
                                                   (unsigned int)img_size,
                                                   mtu, 7200000);
                            printf("SFP send result: %d\n", err);
                        } else {
                            printf("ERROR: fread incompleto\n");
                        }
                        free(img);
                    }
                    fclose(f);
                }
                csp_buffer_free(packet);
                continue;
            }

            /* ── Sensoress──────────────────────── */
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
                snprintf(respuesta, sizeof(respuesta), "UNKNOWN: %.118s", cmd);
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
