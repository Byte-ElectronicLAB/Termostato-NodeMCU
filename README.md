Termostato Inteligente Pro V5 (ESP8266 / NodeMCU)
Esta es la versión avanzada y optimizada del termostato inteligente. Se ha evolucionado de un sistema de control simple a una solución robusta de monitoreo dual y gestión de red mejorada, diseñada para entornos que requieren precisión y alta disponibilidad.

🚀 Mejoras y Nuevas Características de la V5
Doble Sensado (DS18B20): Soporte nativo para dos sensores de temperatura independientes. El sistema permite elegir el modo de control: basado en el Sensor 1, en el Sensor 2 o en el Promedio de ambos.

Sistema de Alertas Configurable: Incluye lógica para disparar alertas de temperatura máxima y mínima, con opción de habilitar o deshabilitar la función desde el panel.

Arquitectura Multi-Nodo: Nueva capacidad para gestionar una red de nodos. El sistema puede comunicarse con otras IPs de la red para centralizar o distribuir información de monitoreo.

Actualizaciones Inalámbricas (OTA): Integración de ESP8266HTTPUpdateServer, lo que permite cargar nuevas versiones del firmware directamente desde el navegador sin necesidad de conectar el dispositivo por USB.

Interfaz Web Profesional: Panel interactivo con visualización de estadísticas avanzadas, gráficos en tiempo real mediante Highcharts y un menú de configuración integral para todos los parámetros del sistema.

Gestión de Red Avanzada: Implementación de MDNS para acceder mediante un nombre de dominio local (ej: termostato.local) y lógica de reconexión crítica para entornos industriales o domésticos.

Características Técnicas
Control de Relé por Histéresis: Implementa una ventana de 0.5°C para evitar el "cicleo" rápido del relé, protegiendo la vida útil de los actuadores (compresores, resistencias, etc.).

Pantalla LCD 16x2 I2C: Visualización dinámica que alterna entre las lecturas de ambos sensores y el estado de la conexión (IP del dispositivo).

Configuración Persistente (EEPROM): Almacenamiento seguro de credenciales WiFi, offsets de calibración para cada sensor, intervalos de muestreo y objetivos de temperatura.

Modo de Rescate (AP): Si la red configurada no está disponible, el dispositivo levanta un punto de acceso propio para su reconfiguración.

Sincronización NTP: Registro preciso de eventos y gráficas mediante servidores de tiempo externos.

Comandos de Ejecución y Configuración
El sistema permite la edición de parámetros críticos mediante la interfaz web, eliminando la necesidad de tocar el código fuente para ajustes de terreno.
