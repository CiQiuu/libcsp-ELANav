# ELANav 


Fork de **[libcsp](https://github.com/libcsp/libcsp)** desarrollado en el Laboratorio de Sistemas Espaciales (**SETECLab**) del Instituto Tecnológico de Costa Rica para el proyecto **ELANav**, orientado a robótica de exploración lunar.

Este repositorio extiende la pila CSP con el módulo **SFP (Small Fragmentation Protocol)** para transferencia transparente de bloques grandes, e integra una caracterización experimental completa del transporte confiable (**RDP**) sobre un canal de radio half-duplex real. El objetivo es una arquitectura de comunicaciones **multi-salto** que enlace un rover sin línea de vista directa con una estación de tierra a través de un nodo retransmisor.

---

## Arquitectura del sistema

```
        Retransmisor (CubeSat)
        almacena y reenvía (store-and-forward, SFP)
                 ▲           │
   UHF 436,5 MHz │           │ UHF 436,5 MHz
   CSP/AX.25     │           ▼
   9600 baud     │       Estación de tierra
                 │
              Rover
   (trayecto directo obstruido por el terreno —
    sin línea de vista hacia la estación)
```

El rover no tiene línea de vista directa con la estación de tierra. El **retransmisor** termina la sesión SFP del rover, reensambla el bloque completo en memoria y abre una sesión SFP nueva hacia la estación (esquema *store-and-forward*), permitiendo transferencias mayores que un único paquete.

### Pila de protocolos

| Capa | Protocolo |
|------|-----------|
| Transporte confiable | **RDP** (RFC 908/1151) |
| Fragmentación | **SFP** (Small Fragmentation Protocol) |
| Red | **CSP** (CubeSat Space Protocol) |
| Enlace | **AX.25 / KISS** |
| Físico | **GFSK 9600 baud, half-duplex, 436,5 MHz UHF** |

### Hardware validado

| | Rover | Estación de tierra |
|---|-------|---------------------|
| Cómputo | Raspberry Pi 4 (Yocto Linux, *scarthgap*) | Laptop |
| Transceptor | Kenwood TS-2000 (5–50 W, −121,9 dBm) | Kenwood TM-D710G (−122,9 dBm) |
| TNC | NinoTNC 9600A | NinoTNC 9600A |
| Antena | Yagi 436CP16 (13,3 dBi) | Eggbeater EB-432 (5,5 dBi) |

El nodo del rover arranca de forma autónoma mediante un servicio `systemd` a partir de una imagen Yocto reproducible. La estación de tierra incluye una interfaz web (Flask/SocketIO) para operar el sistema, ver el log en tiempo real y visualizar las imágenes recibidas.

---

## Logros y capacidad del sistema

Resultados obtenidos sobre el enlace de radiofrecuencia real, a partir de **dos campañas experimentales sistemáticas** (cable puente entre NinoTNC y enlace RF con los Kenwood) que sumaron 60 transferencias instrumentadas. Todos los valores están anclados a logs y mediciones documentadas.

### Rendimiento de envío

| Métrica | Valor | Notas |
|---------|------:|-------|
| **Goodput máximo** | **196,7 B/s** | configuración óptima (C1, `packet_timeout` = 5000 ms) |
| Mejora por ajuste de `packet_timeout` | **6,74×** | sobre la línea base, sin tocar la capa física |
| Mejora RF vs. óptimo por cable | **2,26×** | tras calibrar el TXDELAY del TNC |
| RTT fluido mediano (RF) | **2,02 s** (σ = 0,77 s) | con TXDELAY al 75 % del potenciómetro |
| Reducción de RTT por TXDELAY | **73 %** | de ≈8,5 s a 2,02 s — hallazgo de mayor impacto |
| Ventana RDP óptima | `window_size` = **4** | `window_size` = 6 degrada el goodput ~42 % |
| ACK retardados | `delayed_acks` = **0** | validado experimentalmente (D2 colapsó con 664 timeouts) |

> **Hallazgo principal:** el **TXDELAY del NinoTNC** (capa física), no el `packet_timeout`, es el contribuyente dominante de la latencia del ciclo RDP sobre RF. Reducirlo del 100 % al 75 % bajó la mediana del RTT de ≈8,5 s a 2,02 s. No era observable en el banco de pruebas por cable.

### Estabilidad e integridad

- **Prueba de larga duración:** ≈**13,25 horas** de operación continua, **3 ciclos** consecutivos de un video de **2,68 MB** sobre RF, **integridad MD5 verificada** y **0 % de degradación** acumulativa del goodput.
- **Multi-salto:** transferencia validada extremo a extremo a través del retransmisor *store-and-forward* con **integridad bit a bit** (corrección de un *bug* de split-horizon en libcsp asignando direcciones distintas a cada interfaz del relay).

### Límites de capacidad del canal

- **Techo de capa física:** ≈**1200 B/s** (caso ideal) a 9600 baud GFSK half-duplex; el goodput empírico queda muy por debajo por sobrecarga de protocolo y RTT.
- **Uso recomendado:** el enlace está caracterizado para **telemetría y comandos**, no para datos masivos. Una transferencia de 10 MB tomaría ~4 días continuos al goodput validado.

---

## Repositorio

- Rama de trabajo: `develop`
- Imagen del rover: Yocto *scarthgap*, `core-image-minimal`, capa `meta-csp`
- Servicio: `csp-rover.service` (systemd) → `/usr/bin/csp_sfp_rover`
- Estación de tierra: interfaz web en `localhost:5000`

---

## Sobre libcsp (proyecto base)

CSP (CubeSat Space Protocol) es una pila de protocolos ligera escrita en C, diseñada para facilitar la comunicación entre sistemas embebidos distribuidos en redes pequeñas como CubeSats. Sigue el modelo TCP/IP e incluye protocolo de transporte, enrutamiento e interfaces MAC, con un encabezado muy liviano que combina información de transporte y de red. Está portada a FreeRTOS, Zephyr y Linux (POSIX).

### Características de libcsp

- API de sockets *thread-safe*
- Tarea de enrutamiento con calidad de servicio
- Operación orientada a conexión (RFC 908 y 1151) y sin conexión (similar a UDP)
- Peticiones tipo ICMP (ping, estado de buffers)
- Interfaz de *loopback*, sistema modular de interfaces de red
- Huella muy pequeña en código y memoria, sistema de buffers *zero-copy*
- Tráfico *broadcast* y modo promiscuo

Documentación de libcsp: [libcsp.github.io/libcsp/](https://libcsp.github.io/libcsp/)

### Licencia

El código fuente está disponible bajo licencia MIT; ver `LICENSE` para el texto completo.
