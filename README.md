Termostato Inteligente con NodeMCU y MAX6675

Este proyecto es un termostato de bajo costo y alto rendimiento construido alrededor de un microcontrolador NodeMCU (ESP8266). Su objetivo principal es monitorear y controlar la temperatura ambiente o de un proceso específico, ofreciendo una solución completa con interfaz web, pantalla LCD y control de un relé.

Características Principales
Monitoreo en Tiempo Real: Mide la temperatura con alta precisión usando un sensor MAX6675(o el Ds18b20 y la muestra en una pantalla LCD de 16x2.
*esta tambien el proyecto con sonda ds18b20, que sensa a temperaturas negativas

Interfaz Web Interactiva: Ofrece una página web local con un panel de control que muestra la temperatura actual, estadísticas (mínimo, máximo, promedio) y una gráfica con el historial completo de las mediciones.tambien un menu de configuracion para programar todo desde ahi

Control del Relé: Permite establecer una temperatura objetivo desde la interfaz web. El sistema activa o desactiva un relé automáticamente para mantener la temperatura dentro del rango deseado.

Configuración Persistente: Todos los parámetros de configuración (SSID, contraseña, temperatura objetivo, offset de calibración, etc.) se guardan en la memoria EEPROM del NodeMCU, por lo que no se pierden al reiniciar el dispositivo.

Modo de Punto de Acceso (AP): Si el dispositivo no puede conectarse a la red Wi-Fi configurada, crea su propia red ("NodeMCU-Config") para que puedas acceder a la interfaz web y configurar las credenciales de tu red sin necesidad de reprogramar.

Sincronización de Hora (NTP): Utiliza un servidor de tiempo (NTP) para asegurar que el historial de datos del gráfico tenga marcas de tiempo precisas y confiables.

Autogestión de Red: El código incluye una lógica de reconexión automática de Wi-Fi para garantizar la estabilidad del servicio.
