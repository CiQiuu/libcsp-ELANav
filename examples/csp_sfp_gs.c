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
#include "csp_posix_helper.h"

#define SERVER_PORT  10
#define ROVER_ADDR   20
#define CONN_TIMEOUT 10000   /* ms */

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

    printf("=== GS START ===\n");
    printf("Device: %s  addr=10  rover=%d  port=%d\n",
           device, ROVER_ADDR, SERVER_PORT);

    /*
     * window_size=4:      BDP = 960 B/s × 0.838 s ≈ 804 B
     *                     804 / 162 B por trama ≈ 5 → usar 4 (probado)
     *                     window=8 genera retransmisiones masivas porque
     *                     supera el BDP del canal half-duplex: el patron
     *                     observado es 8 buenos + 8 duplicados ciclicamente.
     *
     * packet_timeout=15000ms: debe ser > RTT bajo congestion (2000-4000 ms).
     *                         Valor validado: mejora de 6.74× sobre linea base.
     *                         Con 30000 ms no mejora la transferencia y solo
     *                         alarga el tiempo de deteccion de fallos reales.
     *
     * conn_timeout=60000ms:   cubre transferencias de imagen de larga duracion.
     *                         El valor anterior (10000 ms) causaba cierre de
     *                         sesion durante transferencias largas.
     *
     * delayed_acks=0:     ACK inmediato por fragmento. Critico en half-duplex:
     *                     si el ACK espera en cola, el packet_timeout expira
     *                     antes y genera retransmisiones innecesarias.
     */
    csp_dbg_rdp_print = 2;
    csp_rdp_set_opt(4,      /* window_size      */
                    300000,  /* conn_timeout_ms  */
                    25000,  /* packet_timeout_ms */
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

    if (csp_usart_open_and_add_kiss_interface(&conf, "KISS", 10, &iface) != CSP_ERR_NONE) {
        printf("ERROR: no se pudo abrir KISS en %s\n", device);
        return 1;
    }
    iface->is_default = 1;
    csp_rtable_set(0, 0, iface, CSP_NO_VIA_ADDRESS);



    /* ── Conexion con backoff ──────────────────────────── */
    
    printf("Conectando con rover (addr=%d)...\n", ROVER_ADDR);
    int wait = 2;
    csp_conn_t *conn = csp_connect(CSP_PRIO_NORM, ROVER_ADDR,
                                   SERVER_PORT, CONN_TIMEOUT, CSP_O_RDP);
    while (!conn) {
        printf("Retry en %d s...\n", wait);
        sleep(wait);
        wait = (wait * 2 > 12) ? 12 : wait * 2;
        conn = csp_connect(CSP_PRIO_NORM, ROVER_ADDR,
                           SERVER_PORT, CONN_TIMEOUT, CSP_O_RDP);
    }

    printf("CONNECTED\n");
    printf("Comandos: t=temperatura  h=humedad  s=sensor  i=imagen  q=salir\n");



    /* ── Loop principal ─────────────────────────────────────────────── */
    
    while (1) {
        char cmd[32];
        printf("cmd> ");
        fflush(stdout);
        if (scanf("%31s", cmd) != 1) continue;
        if (cmd[0] == 'q') break;

        csp_packet_t *p = csp_buffer_get(0);
        if (!p) {
            printf("ERROR: sin buffer CSP\n");
            continue;
        }
        snprintf((char *)p->data, CSP_BUFFER_SIZE, "%s", cmd);
        p->length = strlen((char *)p->data) + 1;
        csp_send(conn, p);



        /* ── Respuesta segun tipo de comando ───────────────────────── */
        
        if (strcmp(cmd, "i") == 0) {

            printf("Esperando imagen SFP (timeout=2h)...\n");
            void *data = NULL;
            int   size = 0;

            struct timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);

            int   err  = csp_sfp_recv(conn, &data, &size, 7200000);

            clock_gettime(CLOCK_MONOTONIC, &t1);
            double elapsed = (t1.tv_sec - t0.tv_sec) +
                             (t1.tv_nsec - t0.tv_nsec) / 1e9;

            if (err == CSP_ERR_NONE && data) {
                printf("Imagen recibida: %d bytes\n", size);
                printf("Tiempo de transferencia: %.2f s\n", elapsed);
                printf("Goodput: %.2f B/s\n", size / elapsed);
                FILE *fp = fopen("gs_received_image.jpg", "wb");
                if (fp) {
                    fwrite(data, 1, (size_t)size, fp);
                    fclose(fp);
                    printf("Guardada en gs_received_image.jpg\n");
                } else {
                    printf("ERROR: no se pudo abrir gs_received_image.jpg\n");
                }
                free(data);
            } else {
                printf("ERROR SFP: %d (%.2f s)\n", err, elapsed);
            }

        } else {

            csp_packet_t *resp = csp_read(conn, 10000);
            if (resp) {
                printf("RESP: %s\n", (char *)resp->data);
                csp_buffer_free(resp);
            } else {
                printf("Sin respuesta (timeout)\n");
            }
        }
    }

    csp_close(conn);
    printf("GS finalizado\n");
    return 0;
}
