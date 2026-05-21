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
#define CONN_TIMEOUT 1000    /* ms — fail-fast en half-duplex, el backoff reintenta */

/* ── Instrumentacion de tiempos ─────────────────────────────────────── */

static struct timespec g_t_start;

static inline void tmark_start(void) {
    clock_gettime(CLOCK_MONOTONIC, &g_t_start);
}

static inline double t_ms_since_start(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec  - g_t_start.tv_sec)  * 1000.0
         + (now.tv_nsec - g_t_start.tv_nsec) / 1.0e6;
}

#define TMARK(label) printf("  [T+%8.1f ms] %s\n", t_ms_since_start(), label)

int main(int argc, char *argv[]) {
    /* Salida sin buffer para ver logs en tiempo real durante la campana */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    const char *device = NULL;
    int packet_timeout_ms = 20000;   /* default: configuracion A2 validada */
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

    printf("=== GS START ===\n");
    printf("Device: %s  addr=10  rover=%d  port=%d  packet_timeout_ms=%d\n",
           device, ROVER_ADDR, SERVER_PORT, packet_timeout_ms);

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

    if (csp_usart_open_and_add_kiss_interface(&conf, "KISS", 10, &iface) != CSP_ERR_NONE) {
        printf("ERROR: no se pudo abrir KISS en %s\n", device);
        return 1;
    }
    iface->is_default = 1;
    csp_rtable_set(0, 0, iface, CSP_NO_VIA_ADDRESS);
    router_start();

    printf("Comandos: t=temperatura  h=humedad  s=sensor  d=datos  i=imagen  v=video  q=salir\n");

    /* ── Loop principal ─────────────────────────────────────────────── */

    while (1) {
        char cmd[32];
        printf("cmd> ");
        fflush(stdout);
        if (scanf("%31s", cmd) != 1) continue;
        if (cmd[0] == 'q') break;

        /* ── T=0: inicio de medicion ────────────────────────────── */
        tmark_start();
        printf("\n=== Comando '%s' ===\n", cmd);
        TMARK("inicio (T=0)");

        TMARK("csp_connect inicio");
        csp_conn_t *conn = csp_connect(CSP_PRIO_NORM, ROVER_ADDR,
                                       SERVER_PORT, CONN_TIMEOUT, CSP_O_RDP);
        if (!conn) {
            TMARK("csp_connect FAIL");
            printf("ERROR: no se pudo conectar con el rover\n");
            printf("=== Comando '%s' fallido en %.1f ms ===\n\n",
                   cmd, t_ms_since_start());
            continue;
        }
        TMARK("csp_connect OK (handshake completo)");

        csp_packet_t *p = csp_buffer_get(0);
        if (!p) {
            printf("ERROR: sin buffer CSP\n");
            csp_close(conn);
            continue;
        }
        snprintf((char *)p->data, CSP_BUFFER_SIZE, "%s", cmd);
        p->length = strlen((char *)p->data) + 1;

        TMARK("csp_send inicio");
        csp_send(conn, p);
        TMARK("csp_send OK");

        /* ── Respuesta segun tipo de comando ───────────────────────── */

        if (strcmp(cmd, "i") == 0) {

            printf("Esperando imagen SFP (timeout=2h)...\n");
            void *data = NULL;
            int   size = 0;

            TMARK("csp_sfp_recv inicio");
            struct timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);

            int err = csp_sfp_recv(conn, &data, &size, 7200000);

            clock_gettime(CLOCK_MONOTONIC, &t1);
            double elapsed = (t1.tv_sec - t0.tv_sec) +
                             (t1.tv_nsec - t0.tv_nsec) / 1e9;
            TMARK("csp_sfp_recv retorno");

            if (err == CSP_ERR_NONE && data) {
                printf("Imagen recibida: %d bytes\n", size);
                printf("Tiempo de transferencia SFP: %.2f s\n", elapsed);
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

        } else if (strcmp(cmd, "v") == 0) {

            printf("Esperando video SFP (timeout=2h, sesion larga esperada)...\n");
            void *data = NULL;
            int   size = 0;

            TMARK("csp_sfp_recv inicio");
            struct timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);

            int err = csp_sfp_recv(conn, &data, &size, 7200000);

            clock_gettime(CLOCK_MONOTONIC, &t1);
            double elapsed = (t1.tv_sec - t0.tv_sec) +
                             (t1.tv_nsec - t0.tv_nsec) / 1e9;
            TMARK("csp_sfp_recv retorno");

            if (err == CSP_ERR_NONE && data) {
                printf("Video recibido: %d bytes\n", size);
                printf("Tiempo de transferencia SFP: %.2f s (%.2f min)\n",
                       elapsed, elapsed / 60.0);
                printf("Goodput: %.2f B/s\n", size / elapsed);
                FILE *fp = fopen("gs_received_video.mp4", "wb");
                if (fp) {
                    fwrite(data, 1, (size_t)size, fp);
                    fclose(fp);
                    printf("Guardado en gs_received_video.mp4\n");
                } else {
                    printf("ERROR: no se pudo abrir gs_received_video.mp4\n");
                }
                free(data);
            } else {
                printf("ERROR SFP: %d (%.2f s)\n", err, elapsed);
            }

        } else if (strcmp(cmd, "d") == 0) {

            printf("Esperando telemetria completa SFP (timeout=30s)...\n");
            void *data = NULL;
            int   size = 0;

            TMARK("csp_sfp_recv inicio");
            struct timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);

            int err = csp_sfp_recv(conn, &data, &size, 30000);

            clock_gettime(CLOCK_MONOTONIC, &t1);
            double elapsed = (t1.tv_sec - t0.tv_sec) +
                             (t1.tv_nsec - t0.tv_nsec) / 1e9;
            TMARK("csp_sfp_recv retorno");

            if (err == CSP_ERR_NONE && data) {
                printf("Telemetria recibida: %d bytes en %.2f s (%.1f B/s)\n",
                       size, elapsed, size / elapsed);
                printf("--- TELEMETRIA OLYMPUS ---\n");
                fwrite(data, 1, (size_t)size, stdout);
                if (size > 0 && ((char *)data)[size - 1] != '\n') printf("\n");
                printf("--- fin telemetria ---\n");
                free(data);
            } else {
                printf("ERROR SFP: %d (%.2f s)\n", err, elapsed);
            }

        } else {

            TMARK("csp_read esperando respuesta");
            csp_packet_t *resp = csp_read(conn, 10000);
            if (resp) {
                TMARK("csp_read OK");
                printf("RESP: %s\n", (char *)resp->data);
                csp_buffer_free(resp);
            } else {
                TMARK("csp_read TIMEOUT");
                printf("Sin respuesta (timeout)\n");
            }
        }

        TMARK("csp_close inicio");
        csp_close(conn);
        TMARK("csp_close OK");

        printf("=== Comando '%s' total: %.1f ms ===\n\n",
               cmd, t_ms_since_start());
    }

    printf("GS finalizado\n");
    return 0;
}
