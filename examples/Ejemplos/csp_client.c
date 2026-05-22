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

#define SERVER_PORT       10
#define DEFAULT_PAYLOAD   128   /* bytes */
#define TOTAL_PACKETS     10    /* paquetes válidos a enviar (antes 100) */

static uint8_t server_address = 10;
static uint8_t client_address = 20;
static unsigned int payload_length = DEFAULT_PAYLOAD;

enum DeviceType {
    DEVICE_UNKNOWN,
    DEVICE_CAN,
    DEVICE_KISS,
    DEVICE_ZMQ,
};

static struct option long_options[] = {
    {"kiss-device", required_argument, 0, 'k'},
#if (CSP_HAVE_LIBSOCKETCAN)
    {"can-device",  required_argument, 0, 'c'},
#endif
#if (CSP_HAVE_LIBZMQ)
    {"zmq-device",  required_argument, 0, 'z'},
#endif
#if (CSP_USE_RTABLE)
    {"rtable",      required_argument, 0, 'R'},
#endif
    {"interface-address", required_argument, 0, 'a'},
    {"connect-to",        required_argument, 0, 'C'},
    {"length",            required_argument, 0, 'l'},
    {"help",              no_argument,       0, 'h'},
    {0, 0, 0, 0}
};

static void print_help(void) {
    csp_print("Usage: csp_client [options]\n");
    csp_print(" -k <kiss-device>  set KISS device\n");
    csp_print(" -a <address>      set interface address\n");
    csp_print(" -C <address>      connect to server at address\n");
    csp_print(" -l <length>       payload length in bytes (<= CSP_BUFFER_SIZE)\n");
    csp_print(" -h                print help\n");
}

static csp_iface_t * add_interface(enum DeviceType type, const char * dev) {
    csp_iface_t *iface = NULL;

    if (type == DEVICE_KISS) {
        csp_usart_conf_t conf = {
            .device = dev,
            .baudrate = 57600,
            .databits = 8,
            .stopbits = 1,
            .paritysetting = 0,
        };
        int err = csp_usart_open_and_add_kiss_interface(&conf, CSP_IF_KISS_DEFAULT_NAME,
                                                        client_address, &iface);
        if (err != CSP_ERR_NONE) {
            csp_print("Failed to add KISS interface %s (err=%d)\n", dev, err);
            exit(EXIT_FAILURE);
        }
        iface->is_default = 1;
    }

    return iface;
}

int main(int argc, char *argv[]) {
    const char * dev_name = NULL;
    enum DeviceType dev_type = DEVICE_UNKNOWN;
    int opt;

    while ((opt = getopt_long(argc, argv, "k:a:C:l:h", long_options, NULL)) != -1) {
        switch (opt) {
            case 'k':
                dev_name = optarg;
                dev_type = DEVICE_KISS;
                break;
            case 'a':
                client_address = atoi(optarg);
                break;
            case 'C':
                server_address = atoi(optarg);
                break;
            case 'l':
                payload_length = atoi(optarg);
                if (payload_length > CSP_BUFFER_SIZE) {
                    csp_print("Payload length > CSP_BUFFER_SIZE (%d)\n", CSP_BUFFER_SIZE);
                    return EXIT_FAILURE;
                }
                break;
            case 'h':
                print_help();
                return 0;
            default:
                print_help();
                return EXIT_FAILURE;
        }
    }

    if (dev_type == DEVICE_UNKNOWN) {
        csp_print("No interface specified (-k, -c, -z).\n");
        return EXIT_FAILURE;
    }

    srand(time(NULL));

    csp_init();
    router_start();

    csp_iface_t *iface = add_interface(dev_type, dev_name);
    csp_rtable_set(0, 0, iface, CSP_NO_VIA_ADDRESS);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    int sent_ok = 0, errors = 0;
    while (sent_ok < TOTAL_PACKETS) {
        char *msg = malloc(payload_length);
        for (unsigned int i = 0; i < payload_length - 1; ++i)
            msg[i] = 'A' + (rand() % 26);
        msg[payload_length - 1] = '\0';

        csp_conn_t *conn = csp_connect(CSP_PRIO_NORM, server_address,
                                       SERVER_PORT, 3000, CSP_O_RDP);
        if (!conn) {
            csp_print("[WARN] Connection failed (#%d)\n", sent_ok + errors);
            ++errors;
            free(msg);
            continue;
        }

        csp_packet_t *pkt = csp_buffer_get(payload_length);
        if (!pkt) {
            csp_print("[WARN] Buffer get failed (#%d)\n", sent_ok + errors);
            csp_close(conn);
            ++errors;
            free(msg);
            continue;
        }

        memcpy(pkt->data, msg, payload_length);
        pkt->length = payload_length;

        csp_send(conn, pkt);
        /* Asegurar que la capa RDP tenga tiempo de enviar los ACK/FIN */
        usleep(50000);
        ++sent_ok;

        csp_close(conn);
        free(msg);
        usleep(100000);
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

    csp_print("\nPaquetes OK: %d\nErrores (conn+buf): %d\nTiempo total: %.3f s\n",
              sent_ok, errors, elapsed);

    return 0;
}

