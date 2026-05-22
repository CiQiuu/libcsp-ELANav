//ok, vamos a crear la nueva version de csp_rover pero mejorada y adaptada a mi proyecto
//Necesito que mantengas las opciones de los sensores pero incluyendo la opcion de enviar una imagen como ya probamos que se puede, ademas de utilizar el sfp para enviar  y/o recibir, ademas de que los datos que envie el rover de los sensores, que se generen aleatoriamente con la finalidad de demostrar el manejo de paquetes de datos distintos

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <termios.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/select.h>

#include <csp/csp_debug.h>
#include <csp/csp.h>
#include <csp/drivers/usart.h>

#include "csp_posix_helper.h"

#define SERVER_PORT     10
#define SERVER_ADDRESS  10
#define CLIENT_ADDRESS  20
#define DEVICE_NAME     "/dev/ttyACM0"
#define ARDUINO_DEVICE  "/dev/ttyACM1"
#define TIMEOUT_MS      1000

int arduino_fd = -1;
char modo_sensor = 'T'; // 'T' = temperatura, 'H' = humedad

void set_input_mode(void) {
    struct termios tattr;
    tcgetattr(STDIN_FILENO, &tattr);
    tattr.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &tattr);
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
}

void reset_input_mode(void) {
    struct termios tattr;
    tcgetattr(STDIN_FILENO, &tattr);
    tattr.c_lflag |= (ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &tattr);
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    flags &= ~O_NONBLOCK;
    fcntl(STDIN_FILENO, F_SETFL, flags);
}

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

int solicitar_dato_sensor(int fd, char modo, char *respuesta, size_t max_len) {
    write(fd, &modo, 1);
    usleep(100000); // 100 ms para que responda

    int n = read(fd, respuesta, max_len - 1);
    if (n > 0) {
        respuesta[n] = '\0';
        return 0;
    }
    return -1;
}

void *server_thread(void *param) {
    (void)param;
    csp_print("Servidor iniciado, esperando comandos\n");

    csp_socket_t sock = {0};
    csp_bind(&sock, CSP_ANY);
    csp_listen(&sock, 10);

    while (1) {
        csp_conn_t *conn = csp_accept(&sock, 1000);
        if (!conn) continue;

        csp_packet_t *packet;
        while ((packet = csp_read(conn, 500)) != NULL) {

            if (csp_conn_dport(conn) == SERVER_PORT) {
                if (strcmp((char *) packet->data, "modo temperatura") == 0) {
                    modo_sensor = 'T';
                    csp_print("Modo cambiado a temperatura\n");

                    csp_packet_t *resp_pkt = csp_buffer_get(0);
                    if (resp_pkt) {
                        snprintf((char *)resp_pkt->data, CSP_BUFFER_SIZE, "Modo cambiado a temperatura");
                        resp_pkt->length = strlen((char *)resp_pkt->data) + 1;
                        csp_send(conn, resp_pkt);
                        csp_buffer_free(resp_pkt);
                    }

                } else if (strcmp((char *) packet->data, "modo humedad") == 0) {
                    modo_sensor = 'H';
                    csp_print("Modo cambiado a humedad\n");

                    csp_packet_t *resp_pkt = csp_buffer_get(0);
                    if (resp_pkt) {
                        snprintf((char *)resp_pkt->data, CSP_BUFFER_SIZE, "Modo cambiado a humedad");
                        resp_pkt->length = strlen((char *)resp_pkt->data) + 1;
                        csp_send(conn, resp_pkt);
                        csp_buffer_free(resp_pkt);
                    }

                } else if (strcmp((char *) packet->data, "solicitar dato") == 0) {
                csp_print("Dato solicitado");
                    if (arduino_fd != -1) {
                        char respuesta[64];
                        if (solicitar_dato_sensor(arduino_fd, modo_sensor, respuesta, sizeof(respuesta)) == 0) {
                            csp_packet_t *resp_pkt = csp_buffer_get(0);
                            if (resp_pkt) {
                                snprintf((char *) resp_pkt->data, CSP_BUFFER_SIZE, "Dato sensor (%c): %s", modo_sensor, respuesta);
                                resp_pkt->length = strlen((char *) resp_pkt->data) + 1;
                                csp_send(conn, resp_pkt);
                                csp_buffer_free(resp_pkt);
                            } else {
                                csp_print("No se pudo obtener buffer para respuesta\n");
                            }
                        } else {
                            csp_print("Error al leer sensor Arduino\n");
                        }
                    } else {
                        csp_print("Arduino no conectado\n");
                    }
                } else {
                    csp_print("Comando desconocido\n");
                }

                csp_buffer_free(packet);
            } else {
                csp_print("Puerto no esperado (%u), descartado\n", csp_conn_dport(conn));
                csp_buffer_free(packet);
            }
        }

        csp_close(conn);
    }

    return NULL;
}




int main(void) {
    csp_iface_t *default_iface;

    csp_print("Inicializando CSP\n");
    csp_init();

    csp_usart_conf_t conf = {
        .device = DEVICE_NAME,
        .baudrate = 57600,
        .databits = 8,
        .stopbits = 1,
        .paritysetting = 0,
    };

    if (csp_usart_open_and_add_kiss_interface(&conf, CSP_IF_KISS_DEFAULT_NAME, CLIENT_ADDRESS, &default_iface) != CSP_ERR_NONE) {
        csp_print("Error: no se pudo agregar interfaz KISS\n");
        return EXIT_FAILURE;
    }

    default_iface->is_default = 1;

    router_start();

#if (CSP_USE_RTABLE)
    csp_rtable_set(0, 0, default_iface, CSP_NO_VIA_ADDRESS);
#endif

    arduino_fd = open_arduino_serial(ARDUINO_DEVICE);
    if (arduino_fd == -1) {
        csp_print("No se pudo abrir comunicación con Arduino\n");
    }

    pthread_t server_tid;
    pthread_create(&server_tid, NULL, server_thread, NULL);

    set_input_mode();
    csp_print("Rover listo. Presiona una tecla para enviar dato ('q' para salir)\n");

    while (1) {
        fd_set readfds;
        struct timeval tv;

        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);
        tv.tv_sec = 0;
        tv.tv_usec = 100000;

        int ret = select(STDIN_FILENO + 1, &readfds, NULL, NULL, &tv);

        if (ret > 0 && FD_ISSET(STDIN_FILENO, &readfds)) {
            char c;
            ssize_t n = read(STDIN_FILENO, &c, 1);
            if (n > 0) {
                if (c == 'q') break;

                csp_print("Tecla '%c' detectada, solicitando dato al Arduino\n", c);

                csp_conn_t *conn = csp_connect(CSP_PRIO_NORM, SERVER_ADDRESS, SERVER_PORT, TIMEOUT_MS, CSP_O_RDP);
                if (!conn) {
                    csp_print("Error: no se pudo establecer conexión\n");
                    continue;
                }

                csp_packet_t *packet = csp_buffer_get(0);
                if (!packet) {
                    csp_print("Error: no se pudo obtener buffer\n");
                    csp_close(conn);
                    continue;
                }

                if (arduino_fd != -1) {
                    char respuesta[64];
                    if (solicitar_dato_sensor(arduino_fd, modo_sensor, respuesta, sizeof(respuesta)) == 0) {
                        snprintf((char *)packet->data, CSP_BUFFER_SIZE, "Dato sensor (%c): %s", modo_sensor, respuesta);
                    } else {
                        snprintf((char *)packet->data, CSP_BUFFER_SIZE, "Error al leer del sensor");
                    }
                } else {
                    snprintf((char *)packet->data, CSP_BUFFER_SIZE, "Arduino no conectado");
                }

                packet->length = strlen((char *)packet->data) + 1;

                csp_send(conn, packet);
                csp_print("Dato enviado al GS\n");

                csp_close(conn);
            }
        }
    }

    reset_input_mode();
    csp_print("Cliente finalizado\n");
    return EXIT_SUCCESS;
}

