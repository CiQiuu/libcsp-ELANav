#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <csp/csp_debug.h>
#include <csp/csp.h>
#include <csp/drivers/usart.h>

#include "csp_posix_helper.h"

#define RETRANSMITTER_ADDRESS  30   // Dirección CSP de este retransmisor
#define FORWARD_ADDRESS        10   // Siempre reenviamos a la dirección 10
#define SERVER_PORT            10   // Puerto CSP donde escucha y reenvía
#define DEVICE_NAME            "/dev/ttyACM0"  // Interface KISS (radio)
#define BAUDRATE               57600          // Baudios para KISS
#define STORE_DELAY_SEC         2             // Segundos a esperar antes de reenviar

int main(void) {
    csp_print("=== Retransmisor (30) iniciando CSP ===\n");
    csp_init();

    // 1) Abrir interfaz KISS en DEVICE_NAME con dirección RETRANSMITTER_ADDRESS
    csp_iface_t *default_iface;
    csp_usart_conf_t conf = {
        .device = DEVICE_NAME,
        .baudrate = BAUDRATE,
        .databits = 8,
        .stopbits = 1,
        .paritysetting = 0,
    };

    if (csp_usart_open_and_add_kiss_interface(&conf,
                                              CSP_IF_KISS_DEFAULT_NAME,
                                              RETRANSMITTER_ADDRESS,
                                              &default_iface)
        != CSP_ERR_NONE) {
        csp_print("Error: no se pudo agregar interfaz KISS\n");
        return EXIT_FAILURE;
    }
    default_iface->is_default = 1;

    // 2) Iniciar enrutador (routing)
    router_start();
#if (CSP_USE_RTABLE)
    csp_rtable_set(0, 0, default_iface, CSP_NO_VIA_ADDRESS);
#endif

    csp_print("Retransmisor listo: escuchando en %u:%u\n",
              RETRANSMITTER_ADDRESS, SERVER_PORT);

    // 3) Preparamos el socket y nos quedamos a la escucha
    csp_socket_t sock = { 0 };
    csp_bind(&sock, SERVER_PORT);
    csp_listen(&sock, 10);

    while (1) {
        // 3.1) Aceptar conexión entrante en 30:10
        csp_conn_t *conn = csp_accept(&sock, 1000);
        if (!conn) {
            // timeout, volvemos a csp_accept
            continue;
        }

        // 3.2) Leer todos los paquetes que lleguen por esta conexión
        csp_packet_t *packet;
        while ((packet = csp_read(conn, 500)) != NULL) {
            // Copiamos el payload a un buffer local
            size_t len = packet->length;
            uint8_t data_copy[CSP_BUFFER_SIZE];
            memcpy(data_copy, packet->data, len);

            csp_print("Retransmisor: recibidos %zu bytes de 10:%u — almacenando\n",
                      len, SERVER_PORT);

            // Liberamos el paquete original
            csp_buffer_free(packet);

            // 3.3) Esperar el tiempo de “store”
            sleep(STORE_DELAY_SEC);

            // 3.4) Reenviar el payload idéntico a (10:10)
            csp_print("Retransmisor: reenviando a %u:%u\n",
                      FORWARD_ADDRESS, SERVER_PORT);

            csp_conn_t *fwd_conn = csp_connect(CSP_PRIO_NORM,
                                               FORWARD_ADDRESS,
                                               SERVER_PORT,
                                               500,
                                               CSP_O_RDP);
            if (!fwd_conn) {
                csp_print("Error: no se pudo conectar a %u:%u para reenvío\n",
                          FORWARD_ADDRESS, SERVER_PORT);
                // Intentamos con el siguiente paquete, si existe
                continue;
            }

            csp_packet_t *fwd_pkt = csp_buffer_get(len);
            if (!fwd_pkt) {
                csp_print("Error: no se pudo obtener buffer para reenvío\n");
                csp_close(fwd_conn);
                continue;
            }
            memcpy(fwd_pkt->data, data_copy, len);
            fwd_pkt->length = len;

            csp_send(fwd_conn, fwd_pkt);
            csp_close(fwd_conn);
            csp_print("Reenvío a %u:%u completado\n", FORWARD_ADDRESS, SERVER_PORT);
        }

        // 3.5) Cuando csp_read() retorne NULL ya no hay más paquetes en esta conexión
        csp_close(conn);
    }

    return EXIT_SUCCESS;
}

