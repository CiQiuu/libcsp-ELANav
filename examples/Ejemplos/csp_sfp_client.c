#include <stdio.h>
#include <csp/csp_debug.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <getopt.h>
#include <time.h>

#include <csp/csp.h>
#include <csp/csp_sfp.h>
#include <csp/drivers/usart.h>
#include <csp/drivers/can_socketcan.h>
#include <csp/interfaces/csp_if_zmqhub.h>

#include "csp_posix_helper.h"

#define SERVER_PORT     10
#define TEST_MSG_SIZE   16000 //(1024*1024)
#define DEFAULT_MTU     128
#define MAX_CONT        10

static uint8_t server_address = 10;
static uint8_t client_address = 20;
static unsigned int mtu = DEFAULT_MTU;

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
    {"mtu",               required_argument, 0, 'm'},
    {"help",              no_argument,       0, 'h'},
    {0, 0, 0, 0}
};

extern void csp_rdp_set_opt(unsigned int window_size, unsigned int conn_timeout_ms,
                             unsigned int packet_timeout_ms, unsigned int delayed_acks,
                             unsigned int ack_timeout, unsigned int ack_delay_count);

static void print_help(void) {
    csp_print("Usage: csp_sfp_client [options]\n");
    csp_print(" -k <kiss-device>  set KISS device\n");
    csp_print(" -a <address>      set interface address\n");
    csp_print(" -C <address>      connect to server at address\n");
    csp_print(" -m <mtu>          set SFP MTU\n");
    csp_print(" -h                print help\n");
}

static csp_iface_t * add_interface(enum DeviceType type, const char * dev) {
    csp_iface_t *iface = NULL;

    if (type == DEVICE_KISS) {
        csp_usart_conf_t conf = {
            .device        = dev,
            .baudrate      = 57600,
            .databits      = 8,
            .stopbits      = 1,
            .paritysetting = 0,
        };
        int err = csp_usart_open_and_add_kiss_interface(&conf,
                                                        CSP_IF_KISS_DEFAULT_NAME,
                                                        client_address,
                                                        &iface);
        if (err != CSP_ERR_NONE) {
            csp_print("Failed to add KISS interface %s (err=%d)\n", dev, err);
            exit(EXIT_FAILURE);
        }
        iface->is_default = 1;
    }

    return iface;
}

static char * gen_msg(size_t size) {
    char *msg = malloc(size);
    if (msg == NULL) return NULL;
    for (size_t i = 0; i < size - 1; ++i)
        msg[i] = 'A' + (rand() % 26);
    msg[size - 1] = '\0';
    return msg;
}

int main(int argc, char *argv[]) {
    const char *dev_name = NULL;
    enum DeviceType dev_type = DEVICE_UNKNOWN;
    int opt;

    while ((opt = getopt_long(argc, argv, "k:a:C:m:h", long_options, NULL)) != -1) {
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
            case 'm':
                mtu = atoi(optarg);
                if (mtu == 0) {
                    csp_print("Invalid mtu\n");
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

    csp_dbg_rdp_print = 2;

    csp_rdp_set_opt(4,      /* window_size       */
                    10000,  /* conn_timeout_ms   */
                    20000,   /* packet_timeout_ms */
                    0,      /* delayed_acks      */
                    2000,   /* ack_timeout_ms    */
                    1);     /* ack_delay_count   */

    csp_init();
    router_start();

    csp_iface_t *iface = add_interface(dev_type, dev_name);
    csp_rtable_set(0, 0, iface, CSP_NO_VIA_ADDRESS);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    size_t msg_size = TEST_MSG_SIZE;
    char *msg = gen_msg(msg_size);
    if (msg == NULL) {
        csp_print("Error reserving message buffer\n");
        return EXIT_FAILURE;
    }

    csp_print("Generated SFP message: %zu bytes\n", msg_size);
    csp_print("Using MTU: %u\n", mtu);
    csp_print("Client address: %u\n", client_address);
    csp_print("Server address: %u\n", server_address);
    csp_print("Server port: %u\n", SERVER_PORT);

    int flag = 0;
    int wait_sec = 2;

    csp_conn_t *conn = csp_connect(CSP_PRIO_NORM, server_address,
                                   SERVER_PORT, 3000, CSP_O_RDP);

    while (conn == NULL) {
        csp_print("Connection failed, retrying in %d s...\n", wait_sec);
        sleep(wait_sec);
        wait_sec = (wait_sec * 2 > 12) ? 12 : wait_sec * 2;

        conn = csp_connect(CSP_PRIO_NORM, server_address,
                           SERVER_PORT, 3000, CSP_O_RDP);
        if (flag++ == MAX_CONT) {
            csp_print("Connection failed after %d attempts\n", MAX_CONT);
            free(msg);
            return EXIT_FAILURE;
        }
    }

    csp_print("Connection established\n");
    csp_print("Calling csp_sfp_send()...\n");

    int err = csp_sfp_send(conn, msg, (unsigned int) msg_size, mtu, 7200000);

    csp_close(conn);
    free(msg);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

    if (err == CSP_ERR_NONE) {
        csp_print("\nSFP transfer OK\n");
        csp_print("Bytes: %zu\n", msg_size);
        csp_print("Time:  %.3f s\n", elapsed);
        return EXIT_SUCCESS;
    } else {
        csp_print("\nSFP transfer failed: %d\n", err);
        csp_print("Time:  %.3f s\n", elapsed);
        return EXIT_FAILURE;
    }
}
