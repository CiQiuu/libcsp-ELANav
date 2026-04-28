#include <stdio.h>
#include <csp/csp_debug.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>
#include <stdint.h>

#include <csp/csp.h>
#include <csp/csp_sfp.h>
#include <csp/drivers/usart.h>
#include <csp/drivers/can_socketcan.h>
#include <csp/interfaces/csp_if_zmqhub.h>

#include "csp_posix_helper.h"

#define SERVER_PORT     10
#define OUTPUT_FILE     "received_image.jpg"

static uint8_t server_address = 10;
static struct timespec start_time;

extern void csp_rdp_set_opt(unsigned int window_size, unsigned int conn_timeout_ms,
                             unsigned int packet_timeout_ms, unsigned int delayed_acks,
                             unsigned int ack_timeout, unsigned int ack_delay_count);

void * server(void * param) {
    (void)param;
    csp_print("Server task started\n");
    clock_gettime(CLOCK_MONOTONIC, &start_time);
    csp_socket_t sock = {0};
    csp_bind(&sock, SERVER_PORT);
    csp_listen(&sock, 10);
    while (1) {
        csp_conn_t *conn = csp_accept(&sock, 1500);
        if (conn == NULL) {
            csp_print("...\n");
            continue;
        }
        csp_print("Connection accepted\n");
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        void *rx_data = NULL;
        int rx_size = 0;
        int err = csp_sfp_recv(conn, &rx_data, &rx_size, 7200000);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double elapsed = (t1.tv_sec - t0.tv_sec) +
                         (t1.tv_nsec - t0.tv_nsec) / 1e9;
        if (err == CSP_ERR_NONE) {
            csp_print("transfer complete: OK\n");
            csp_print("Bytes: %d (%.2f KB)\n", rx_size, rx_size / 1024.0);
            csp_print("Total time: %.1f s (%.1f min)\n", elapsed, elapsed / 60.0);
            FILE *fp = fopen(OUTPUT_FILE, "wb");
            if (fp != NULL) {
                fwrite(rx_data, 1, (size_t) rx_size, fp);
                fclose(fp);
                csp_print("Saved on file: %s\n", OUTPUT_FILE);
            } else {
                csp_print("Warning: couldn't save output file\n");
            }
            if (rx_data != NULL) free(rx_data);
        } else {
            csp_print("Error on transfer: %d\n", err);
            csp_print("Total time: %.1f s\n", elapsed);
        }
        csp_close(conn);
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    const char *device_name = "/dev/ttyACM0";
    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], "-k") == 0) {
            device_name = argv[i + 1];
        }
    }

    csp_print("Server device: %s\n", device_name);
    csp_print("Server address: %u\n", server_address);
    csp_print("Output file: %s\n", OUTPUT_FILE);

    csp_dbg_rdp_print = 2;

     csp_rdp_set_opt(4,      /* window_size       */
                    10000,  /* conn_timeout_ms   */
                    20000,   /* packet_timeout_ms */
                    0,      /* delayed_acks      */
                    2000,   /* ack_timeout_ms    */
                    1);     /* ack_delay_count   */

    csp_init();
    router_start();

    csp_iface_t *iface;
    csp_usart_conf_t conf = {
        .device        = device_name,
        .baudrate      = 57600,
        .databits      = 8,
        .stopbits      = 1,
        .paritysetting = 0,
    };
    int error = csp_usart_open_and_add_kiss_interface(&conf, "KISS", server_address, &iface);
    if (error != CSP_ERR_NONE) {
        csp_print("Failed to init KISS interface on %s\n", device_name);
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
