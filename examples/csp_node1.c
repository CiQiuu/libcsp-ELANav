#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <termios.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/select.h>
#include <stdatomic.h>

#include <csp/csp_debug.h>
#include <csp/csp.h>
#include <csp/drivers/usart.h>

#include "csp_posix_helper.h"

#define MY_ADDRESS             10    // Dirección CSP de este nodo (rover+GS unidos)
#define RETRANSMITTER_ADDRESS  30    // Dirección CSP del retransmisor store & forward
#define SERVER_PORT            10    // Puerto CSP único para enviar y recibir
#define DEVICE_NAME            "/dev/ttyACM0"  // Interfaz KISS (radio)
#define ARDUINO_DEVICE         "/dev/ttyACM1"  // Arduino UNO (con sensor)
#define TIMEOUT_MS             1000  // Timeout CSP en ms

// Bandera atómica que indica que se solicitó un dato
static atomic_bool solicitud_dato = ATOMIC_VAR_INIT(false);

// 'T' = temperatura, 'H' = humedad. Por defecto 'T'
static char modo_sensor = 'T';

// Configura stdin en modo no bloqueante, sin eco
void set_input_mode(void) {
    struct termios tattr;
    tcgetattr(STDIN_FILENO, &tattr);
    tattr.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &tattr);
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
}

// Restaura stdin a modo normal
void reset_input_mode(void) {
    struct termios tattr;
    tcgetattr(STDIN_FILENO, &tattr);
    tattr.c_lflag |= (ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &tattr);
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    flags &= ~O_NONBLOCK;
    fcntl(STDIN_FILENO, F_SETFL, flags);
}

// Abre el puerto serial del Arduino a 9600 baudios, 8N1
int open_arduino_serial(const char *device) {
    int fd = open(device, O_RDWR | O_NOCTTY | O_NDELAY);
    if (fd == -1) {
        perror("Error al abrir puerto serial Arduino");
        return -1;
    }

    struct termios options;
    tcgetattr(fd, &options);
    cfsetispeed(&options, B9600);
    cfsetospeed(&options, B9600);

    options.c_cflag &= ~PARENB;
    options.c_cflag &= ~CSTOPB;
    options.c_cflag &= ~CSIZE;
    options.c_cflag |= CS8;
    options.c_cflag |= CREAD | CLOCAL;

    options.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    options.c_iflag &= ~(IXON | IXOFF | IXANY);
    options.c_oflag &= ~OPOST;

    tcsetattr(fd, TCSANOW, &options);
    return fd;
}

// Envía un byte 'modo' al Arduino y lee la respuesta en 'respuesta'
int solicitar_dato_sensor(int fd, char modo, char *respuesta, size_t max_len) {
    write(fd, &modo, 1);
    usleep(100000); // 100 ms para que Arduino responda

    int n = read(fd, respuesta, max_len - 1);
    if (n > 0) {
        respuesta[n] = '\0';
        return 0;
    }
    return -1;
}

// Hilo servidor CSP que escucha en el puerto SERVER_PORT:
// - Cuando recibe "modo temperatura" o "modo humedad": actualiza modo_sensor y responde acuse.
// - Cuando recibe "solicitar dato": pone solicitud_dato = true (sin responder).
// - Cuando recibe "Dato sensor" o "ACK: ...": imprime en consola.
// - Otros mensajes: imprime “desconocido”.
void *server_thread(void *param) {
    (void)param;
    csp_print("Servidor CSP iniciado en puerto %u\n", SERVER_PORT);

    csp_socket_t sock = {0};
    csp_bind(&sock, SERVER_PORT);
    csp_listen(&sock, 10);

    while (1) {
        csp_conn_t *conn = csp_accept(&sock, 1000);
        if (!conn) continue;

        csp_packet_t *packet;
        while ((packet = csp_read(conn, 500)) != NULL) {
            char *msg = (char *) packet->data;
            msg[strcspn(msg, "\r\n")] = '\0';  // Quitar fin de línea

            if (strcmp(msg, "modo temperatura") == 0) {
                modo_sensor = 'T';
                csp_print("[Servidor] Modo cambiado a TEMPERATURA\n");

                // Enviar acuse de recibo al retransmisor
                csp_packet_t *resp_pkt = csp_buffer_get(0);
                if (resp_pkt) {
                    snprintf((char *)resp_pkt->data, CSP_BUFFER_SIZE, "ACK: modo temperatura");
                    resp_pkt->length = strlen((char *)resp_pkt->data) + 1;
                    csp_send(conn, resp_pkt);
                    csp_buffer_free(resp_pkt);
                }

            } else if (strcmp(msg, "modo humedad") == 0) {
                modo_sensor = 'H';
                csp_print("[Servidor] Modo cambiado a HUMEDAD\n");

                // Enviar acuse de recibo al retransmisor
                csp_packet_t *resp_pkt = csp_buffer_get(0);
                if (resp_pkt) {
                    snprintf((char *)resp_pkt->data, CSP_BUFFER_SIZE, "ACK: modo humedad");
                    resp_pkt->length = strlen((char *)resp_pkt->data) + 1;
                    csp_send(conn, resp_pkt);
                    csp_buffer_free(resp_pkt);
                }

            } else if (strcmp(msg, "solicitar dato") == 0) {
                csp_print("[Servidor] Petición de dato recibida, activando bandera\n");
                atomic_store(&solicitud_dato, true);
                // NO enviamos respuesta inmediata

            } else if (strncmp(msg, "Dato sensor", 11) == 0) {
                csp_print("[Servidor] %s\n", msg);

            } else if (strncmp(msg, "ACK: modo", 9) == 0) {
                csp_print("[Servidor] %s\n", msg);

            } else {
                csp_print("[Servidor] Mensaje no reconocido: \"%s\"\n", msg);
                // Opcional: enviar un NACK
                csp_packet_t *resp_pkt = csp_buffer_get(0);
                if (resp_pkt) {
                    snprintf((char *)resp_pkt->data, CSP_BUFFER_SIZE, "NACK: comando desconocido");
                    resp_pkt->length = strlen((char *)resp_pkt->data) + 1;
                    csp_send(conn, resp_pkt);
                    csp_buffer_free(resp_pkt);
                }
            }

            csp_buffer_free(packet);
        }

        csp_close(conn);
    }

    return NULL;
}

int main(void) {
    csp_print("=== Nodo único (10) iniciando CSP ===\n");
    csp_init();

    // Abrir interfaz KISS en DEVICE_NAME con dirección MY_ADDRESS
    csp_iface_t *default_iface;
    csp_usart_conf_t conf = {
        .device = DEVICE_NAME,
        .baudrate = 57600,
        .databits = 8,
        .stopbits = 1,
        .paritysetting = 0,
    };

    if (csp_usart_open_and_add_kiss_interface(&conf, CSP_IF_KISS_DEFAULT_NAME,
                                              MY_ADDRESS, &default_iface)
        != CSP_ERR_NONE) {
        csp_print("Error: no se pudo agregar interfaz KISS\n");
        return EXIT_FAILURE;
    }
    default_iface->is_default = 1;

    // Iniciar enrutador
    router_start();
#if (CSP_USE_RTABLE)
    csp_rtable_set(0, 0, default_iface, CSP_NO_VIA_ADDRESS);
#endif

    // Abrir Arduino serial
    int arduino_fd = open_arduino_serial(ARDUINO_DEVICE);
    if (arduino_fd == -1) {
        csp_print("Advertencia: Arduino no conectado o no se pudo abrir %s\n", ARDUINO_DEVICE);
    } else {
        csp_print("Arduino abierto correctamente en %s\n", ARDUINO_DEVICE);
    }

    // Arrancar hilo servidor CSP
    pthread_t server_tid;
    if (pthread_create(&server_tid, NULL, server_thread, NULL) != 0) {
        csp_print("Error: no se pudo crear hilo servidor\n");
        return EXIT_FAILURE;
    }

    // Configurar stdin en modo no bloqueante
    set_input_mode();
    csp_print("\nNodo listo. Pulsa:\n"
              "  't' -> cambiar a modo TEMPERATURA\n"
              "  'h' -> cambiar a modo HUMEDAD\n"
              "  's' -> solicitar dato (activa bandera)\n"
              "  'q' -> salir\n\n");

    // Bucle principal: lectura de teclas + envío de datos si se solicitó
    while (1) {
        // 1) Lectura de teclado (no bloqueante)
        fd_set readfds;
        struct timeval tv;
        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);
        tv.tv_sec = 0;
        tv.tv_usec = 100000;  // 100 ms

        int ret = select(STDIN_FILENO + 1, &readfds, NULL, NULL, &tv);
        if (ret > 0 && FD_ISSET(STDIN_FILENO, &readfds)) {
            char c;
            ssize_t n = read(STDIN_FILENO, &c, 1);
            if (n > 0) {
                if (c == 'q') {
                    break;
                }

                const char *mensaje = NULL;

                if (c == 't') {
                    mensaje = "modo temperatura";
                    csp_print("[Main] Tecla 't': enviando \"%s\" al retransmisor\n", mensaje);

                } else if (c == 'h') {
                    mensaje = "modo humedad";
                    csp_print("[Main] Tecla 'h': enviando \"%s\" al retransmisor\n", mensaje);

                } else if (c == 's') {
                    mensaje = "solicitar dato";
                    csp_print("[Main] Tecla 's': enviando \"%s\" al retransmisor\n", mensaje);

                } else {
                    continue;
                }

                // Enviar mensaje al retransmisor
                csp_conn_t *conn = csp_connect(CSP_PRIO_NORM,
                                               RETRANSMITTER_ADDRESS,
                                               SERVER_PORT,
                                               TIMEOUT_MS,
                                               CSP_O_RDP);
                if (!conn) {
                    csp_print("Error: no se pudo conectar a %u:%u\n",
                              RETRANSMITTER_ADDRESS, SERVER_PORT);
                } else {
                    csp_packet_t *packet = csp_buffer_get(0);
                    if (!packet) {
                        csp_print("Error: no se pudo obtener buffer para paquete\n");
                        csp_close(conn);
                    } else {
                        snprintf((char *)packet->data, CSP_BUFFER_SIZE, "%s", mensaje);
                        packet->length = strlen((char *)packet->data) + 1;
                        csp_send(conn, packet);
                        csp_close(conn);
                    }
                }
            }
        }

        // 2) Si se solicitó dato (servidor puso la bandera), entonces leemos Arduino y enviamos resultado
        if (atomic_load(&solicitud_dato)) {
            atomic_store(&solicitud_dato, false);
            csp_print("[Main] Procesando solicitud de dato: leyendo Arduino...\n");

            if (arduino_fd != -1) {
                char respuesta_arduino[64];
                if (solicitar_dato_sensor(arduino_fd, modo_sensor, respuesta_arduino, sizeof(respuesta_arduino)) == 0) {
                    // Construimos el mensaje de respuesta
                    char msg_resp[CSP_BUFFER_SIZE];
                    snprintf(msg_resp, sizeof(msg_resp), "Dato sensor (%c): %s",
                             modo_sensor, respuesta_arduino);

                    csp_print("[Main] Enviando dato al retransmisor: \"%s\"\n", msg_resp);
                    csp_conn_t *conn2 = csp_connect(CSP_PRIO_NORM,
                                                    RETRANSMITTER_ADDRESS,
                                                    SERVER_PORT,
                                                    TIMEOUT_MS,
                                                    CSP_O_RDP);
                    if (!conn2) {
                        csp_print("Error: no se pudo conectar a %u:%u para enviar dato\n",
                                  RETRANSMITTER_ADDRESS, SERVER_PORT);
                    } else {
                        csp_packet_t *pkt2 = csp_buffer_get(0);
                        if (!pkt2) {
                            csp_print("Error: no se pudo obtener buffer para paquete dato\n");
                            csp_close(conn2);
                        } else {
                            snprintf((char *)pkt2->data, CSP_BUFFER_SIZE, "%s", msg_resp);
                            pkt2->length = strlen((char *)pkt2->data) + 1;
                            csp_send(conn2, pkt2);
                            csp_close(conn2);
                        }
                    }
                } else {
                    csp_print("Error al leer sensor Arduino\n");
                }
            } else {
                csp_print("Arduino no conectado, no se pudo leer dato\n");
            }
        }

        // 3) Pequeña pausa antes de la siguiente iteración
        usleep(100000);  // 100 ms
    }

    // Limpiar y salir
    reset_input_mode();
    csp_print("Saliendo del nodo.\n");
    return EXIT_SUCCESS;
}
