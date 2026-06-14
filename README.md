# DataGuard — Monitoreo IoT de Cuarto de Servidores

Sistema de monitoreo ambiental y de seguridad para un cuarto de servidores.
Un **nodo sensor (ESP8266)** mide temperatura, humedad y humo, ejecuta una alarma
local, y publica la telemetría por WiFi/MQTT a un **centro de control (Raspberry Pi)**,
que la reenvía a **AWS IoT Core** por TLS.

> **Estado de este README:** cubre la **capa física** (nodo + centro de control) y la
> **ingesta a AWS IoT Core** (Thing, certificados, política, verificación end-to-end).
> Las fases posteriores de AWS (IoT Rule, DynamoDB, S3, CloudWatch/SNS, API Gateway,
> dashboard) se documentan más adelante.

---

## Arquitectura

```
   [ Sensores ]                 [ NODO SENSOR ]          [ CENTRO DE CONTROL ]        [ NUBE ]
   DHT22 (temp/hum) -digital-+
   MQ-2  (humo)     -A0------+
                             +-> ESP8266 -WiFi/MQTT-> Raspberry Pi -MQTT/TLS-> AWS IoT Core
   LCD 16x2 (I2C)   <--------+      (Mosquitto + bridge, en Docker)
   Buzzer + LEDs    <--------+
        (alarma local, independiente de la red)
```

- **Nodo sensor (ESP8266):** lectura de sensores + alarma local + publicación MQTT.
- **Centro de control (Raspberry Pi):** broker Mosquitto (recibe del nodo) + bridge
  Python (reenvía a AWS). Ambos corren como contenedores Docker.
- **Sin cables entre nodo y Pi:** se comunican por WiFi en la red local.
- **AWS IoT Core:** punto de entrada seguro (MQTT/TLS con certificados X.509).

**Región AWS del proyecto:** `us-east-2` (Ohio). Todo debe ser consistente con esta región.

---

## Tabla de contenidos

1. [Hardware necesario](#1-hardware-necesario)
2. [Cableado del nodo ESP8266](#2-cableado-del-nodo-esp8266)
3. [Parte A — Nodo sensor (ESP8266)](#3-parte-a--nodo-sensor-esp8266)
4. [Parte B — Centro de control (Raspberry Pi)](#4-parte-b--centro-de-control-raspberry-pi)
5. [Parte C — AWS IoT Core (ingesta)](#5-parte-c--aws-iot-core-ingesta)
6. [Verificación de extremo a extremo](#6-verificación-de-extremo-a-extremo)
7. [Solución de problemas](#7-solución-de-problemas)

---

## 1. Hardware necesario

| Componente | Cantidad | Notas |
|---|---|---|
| ESP8266 (NodeMCU) | 1 | Nodo sensor |
| Raspberry Pi | 1 | Centro de control (cualquier modelo con red) |
| Sensor DHT22 | 1 | Temperatura y humedad (digital) |
| Sensor MQ-2 | 1 | Humo / gases combustibles (analógico) |
| LCD 16x2 con módulo I2C | 1 | Display local |
| LED verde / amarillo / rojo | 3 | Semáforo de estado |
| Buzzer activo | 1 | Alarma sonora |
| Resistencias, protoboard, cables | — | Para el armado |

---

## 2. Cableado del nodo ESP8266

| Pin ESP8266 | Conecta a | Función |
|---|---|---|
| `A0` | MQ-2 (salida analógica) | Lectura de humo |
| `D0` | DHT22 (datos) | Temp/humedad — *ver nota* |
| `D1` | LCD SCL | Reloj I2C |
| `D2` | LCD SDA | Datos I2C |
| `D5` | LED verde | Estado normal |
| `D6` | LED amarillo | Advertencia |
| `D7` | LED rojo | Crítico |
| `D8` | Buzzer | Alarma sonora (solo crítico) |

> **Nota sobre D0 (GPIO16):** no soporta interrupciones. Suele funcionar con el DHT22
> en modo polling, pero si obtienes lecturas `NaN`, mueve el dato del DHT22 a **D3 o D4**
> y actualiza `PIN_DHT` en el código.

> **MQ-2:** necesita varios minutos de calentamiento ("burn-in") antes de dar lecturas
> estables. La lectura es cruda (0–1023), no PPM.

---

## 3. Parte A — Nodo sensor (ESP8266)

### 3.1 Entorno de desarrollo

1. Instala el **Arduino IDE**.
2. Soporte para ESP8266: `Archivo → Preferencias → URLs adicionales`, agrega:
   ```
   http://arduino.esp8266.com/stable/package_esp8266com_index.json
   ```
   Luego `Herramientas → Placa → Gestor de tarjetas` → instala **esp8266**.
3. Selecciona la placa (**NodeMCU 1.0**) y el puerto COM.

### 3.2 Librerías requeridas

Desde `Herramientas → Gestor de librerías`:

- **DHT sensor library** (Adafruit) + **Adafruit Unified Sensor**
- **PubSubClient** (Nick O'Leary) — cliente MQTT
- **LiquidCrystal I2C** (Frank de Brabander) — LCD por I2C
- **ArduinoJson** (Benoit Blanchon) — construcción del JSON

> El warning *"LiquidCrystal_I2C claims to run on avr architecture"* es inofensivo:
> la librería solo usa I2C estándar y funciona en ESP8266.

### 3.3 Configuración antes de cargar

```cpp
// WiFi
const char* WIFI_SSID     = "TU_RED_WIFI";
const char* WIFI_PASSWORD = "TU_PASSWORD_WIFI";

// MQTT (IP de la Raspberry Pi en la red local)
const char* MQTT_BROKER   = "192.168.1.XX";   // <-- obtener con: hostname -I  en la Pi
const int   MQTT_PORT     = 1883;
const char* MQTT_TOPIC    = "datacenter/rack/telemetria";

// Umbrales de alarma (ajustar según el entorno)
const float TEMP_WARN  = 26.0;
const float TEMP_CRIT  = 30.0;
const int   SMOKE_WARN = 300;
const int   SMOKE_CRIT = 450;
```

> El LCD trae **escáner I2C automático**: detecta la dirección (`0x27` o `0x3F`) al
> arrancar y la imprime por Serial. No hay que configurarla a mano.

### 3.4 Cargar y verificar

1. Carga el sketch.
2. Abre el **Monitor Serial** a `115200` baudios.
3. Deberías ver: test de LEDs (verde→amarillo→rojo), dirección I2C del LCD, conexión
   WiFi con IP, conexión MQTT, y el JSON publicándose cada 15 s:
   ```json
   {"device":"nodo-rack-01","temp":27.4,"hum":61.2,"smoke":342,"alarm":false,"state":"NORMAL","ts":12345}
   ```

---

## 4. Parte B — Centro de control (Raspberry Pi)

Dos contenedores Docker: **Mosquitto** (broker) y el **bridge** (reenvío a AWS).

### 4.1 Requisitos

```bash
curl -fsSL https://get.docker.com | sh
sudo usermod -aG docker $USER   # opcional: usar docker sin sudo (re-login)
```

### 4.2 Estructura de carpetas

```bash
mkdir -p ~/centro-control/mosquitto/config
mkdir -p ~/certs
```

```
~/centro-control/mosquitto/config/mosquitto.conf
~/certs/
├── certificate.pem.crt
├── private.pem.key
└── AmazonRootCA1.pem
```

### 4.3 Configuración de Mosquitto

`~/centro-control/mosquitto/config/mosquitto.conf`:

```conf
listener 1883
allow_anonymous true

persistence true
persistence_location /mosquitto/data/

log_dest file /mosquitto/log/mosquitto.log
log_dest stdout
log_type all
```

### 4.4 Construir la imagen del bridge (desde tu PC, para arm64)

```bash
# Carpeta con Dockerfile, bridge_aws.py, requirements.txt
docker build --platform=linux/arm64 -t jzuletadev/dataguard-bridge:arm64 --push .
```

> Los **certificados NO van dentro de la imagen**: se montan como volumen en runtime.

### 4.5 Levantar el centro de control (docker run)

> Si tenías Mosquitto por `apt`, desactívalo para liberar el puerto 1883:
> `sudo systemctl stop mosquitto && sudo systemctl disable mosquitto`

```bash
# 1) Red compartida (los contenedores se ven por nombre dentro de ella)
docker network create dataguard-net

# 2) Mosquitto
docker run -d \
  --name dataguard-mosquitto \
  --network dataguard-net \
  --restart unless-stopped \
  -p 1883:1883 \
  -v ~/centro-control/mosquitto/config:/mosquitto/config:ro \
  -v mosquitto_data:/mosquitto/data \
  -v mosquitto_log:/mosquitto/log \
  eclipse-mosquitto:2

# 3) Bridge
#    OJO: LOCAL_BROKER debe ser el NOMBRE DEL CONTENEDOR de Mosquitto
#    (con docker run es 'dataguard-mosquitto', NO 'mosquitto').
docker run -d \
  --name dataguard-bridge \
  --network dataguard-net \
  --restart unless-stopped \
  -e LOCAL_BROKER=dataguard-mosquitto \
  -e LOCAL_PORT=1883 \
  -e LOCAL_TOPIC=datacenter/rack/telemetria \
  -e AWS_ENDPOINT=XXXXXXXX-ats.iot.us-east-2.amazonaws.com \
  -e AWS_CLIENT_ID=rack-gw-01 \
  -e AWS_TOPIC=datacenter/rack/telemetria \
  -v /home/why/certs:/app/certs:ro \
  jzuletadev/dataguard-bridge:arm64
```

> **Ruta de certificados:** ajusta `/home/why/certs` a tu usuario real de la Pi.
> **`AWS_ENDPOINT`:** se obtiene en la Parte C.

---

## 5. Parte C — AWS IoT Core (ingesta)

Objetivo: registrar el dispositivo (Thing), generar y asegurar sus certificados, y
lograr que la Raspberry Pi publique en AWS IoT Core.

### 5.1 Obtener el endpoint de datos

En la consola: **AWS IoT → Connect → Connect one device**. En el primer paso ("Prepare
your device"), AWS muestra el **Device data endpoint**, con forma:

```
XXXXXXXX-ats.iot.us-east-2.amazonaws.com
```

Cópialo: es el valor de `AWS_ENDPOINT` del bridge.

> CloudShell puede estar bloqueado en cuentas nuevas ("account verification in
> progress", hasta 2 días). No es necesario: el endpoint se obtiene desde el asistente.

### 5.2 Crear el Thing y los certificados

Con el asistente **Connect one device**:

1. **Create a new thing** → nombre: `rack-gw-01` (debe coincidir con `AWS_CLIENT_ID`).
2. Plataforma: **Linux**, SDK: **Python**.
3. **Descargar el connection kit** — descarga **todos** los archivos. La llave privada
   **no se puede volver a descargar**.

AWS entrega los archivos con nombres basados en el Thing. Mapeo a lo que espera el bridge:

| AWS entrega | Es el... | Renombrar a |
|---|---|---|
| `rack-gw-01.cert.pem` | Certificado del dispositivo | `certificate.pem.crt` |
| `rack-gw-01.private.key` | Llave privada | `private.pem.key` |
| `rack-gw-01.public.key` | Llave pública | *(no se usa)* |
| `rack-gw-01-Policy` | Política (referencia) | *(no es archivo de cert)* |
| `start.sh` | Script de ejemplo | *(no se usa)* |

> Windows puede mostrar las `.key` como "Keynote Presentation": es solo el icono por
> la extensión; son archivos de texto plano, no están dañados. Renombrar la extensión
> **no** daña el contenido.

### 5.3 Descargar el Amazon Root CA 1

No siempre viene en el kit (es público). En la Pi:

```bash
cd ~/certs
curl -o AmazonRootCA1.pem https://www.amazontrust.com/repository/AmazonRootCA1.pem
```

### 5.4 Colocar y renombrar certificados en la Pi

```bash
cd ~/certs
mv rack-gw-01.cert.pem   certificate.pem.crt
mv rack-gw-01.private.key private.pem.key
# Verificar:
ls -l ~/certs
# Debe mostrar EXACTAMENTE:
#   AmazonRootCA1.pem
#   certificate.pem.crt
#   private.pem.key
```

### 5.5 Activar el certificado y adjuntar la política (¡PASO CLAVE!)

Sin esto, la conexión TLS se establece pero AWS la cierra de golpe
(`AWS_ERROR_MQTT_UNEXPECTED_HANGUP`).

1. **Security → Certificates** → confirmar que el certificado está **Active**
   (si no: `Actions → Activate`).
2. **Security → Policies → Create policy**:
   - Nombre: `rack-gw-policy`
   - Editor en modo **JSON**:
     ```json
     {
       "Version": "2012-10-17",
       "Statement": [
         {
           "Effect": "Allow",
           "Action": ["iot:Connect", "iot:Publish", "iot:Subscribe", "iot:Receive"],
           "Resource": "*"
         }
       ]
     }
     ```
3. **Security → Certificates** → clic en el certificado → `Actions → Attach policy` →
   seleccionar `rack-gw-policy`.

> `"Resource": "*"` sirve para validar la conexión. Para la entrega final conviene la
> versión restringida (mínimo privilegio) limitando a este Thing y tópico.

### 5.6 Levantar el bridge y verificar conexión

```bash
docker restart dataguard-bridge
docker logs -f dataguard-bridge
```

Secuencia esperada:

```
[AWS] Conectado a IoT Core.
[LOCAL] Conectado a Mosquitto (dataguard-mosquitto:1883). Suscribiendo a 'datacenter/rack/telemetria'...
[BRIDGE] Puente activo. Esperando mensajes del ESP8266...
[AWS -> datacenter/rack/telemetria] {"device":"nodo-rack-01","temp":28,...}
```

---

## 6. Verificación de extremo a extremo

Validar por capas, de adentro hacia afuera:

**1. ¿Mosquitto recibe del ESP8266?**
```bash
docker exec -it dataguard-mosquitto mosquitto_sub -t "datacenter/rack/telemetria" -v
```

**2. ¿El bridge reenvía a AWS?**
```bash
docker logs -f dataguard-bridge      # buscar lineas [AWS -> ...]
```

**3. ¿AWS lo recibe?**
En la consola: **AWS IoT Core → MQTT test client → Subscribe to a topic** →
`datacenter/rack/telemetria`. Deben aparecer los JSON en vivo, por ejemplo:
```json
{"device":"nodo-rack-01","temp":28.1,"hum":65,"smoke":130,"alarm":false,
 "state":"ADVERT.","ts_utc":"2026-06-14T01:07:37+00:00","ts_epoch":1781399257}
```

Si los tres pasos funcionan, la **ingesta IoT está validada de punta a punta**.

---

## 7. Solución de problemas

| Síntoma | Causa probable | Solución |
|---|---|---|
| ESP8266: `MQTT fallo, rc=-2` | IP del broker incorrecta | Verificar IP real de la Pi con `hostname -I` (mayúscula) |
| ESP8266: `rc=-2` con IP correcta | Mosquitto no escucha en la red | `listener 1883` en `mosquitto.conf`; `ss -tlnp \| grep 1883` |
| LCD en blanco | Contraste o dirección I2C | Ajustar potenciómetro del módulo I2C; revisar dirección en Serial |
| DHT22 devuelve `NaN` | Pin D0 sin interrupciones | Mover DHT22 a D3 o D4 y actualizar `PIN_DHT` |
| MQ-2 marca muy alto al inicio | Falta calentamiento | Dejar el sensor encendido varios minutos |
| Bridge: `No such file or directory: /app/certs/...` | Certificados mal nombrados o volumen mal montado | Verificar nombres exactos en `~/certs` y la ruta del `-v` |
| Bridge: `AWS_ERROR_MQTT_UNEXPECTED_HANGUP` | Certificado inactivo o **sin política** | Activar certificado y adjuntar `rack-gw-policy` (sección 5.5) |
| Bridge: `[Errno -5] No address associated with hostname` | `LOCAL_BROKER` ≠ nombre del contenedor | Con `docker run`, usar `LOCAL_BROKER=dataguard-mosquitto` |
| `docker compose: command not found` | Plugin compose v2 ausente | Usar `docker run` por separado |

---

## Notas de seguridad

- Los certificados se montan como volumen **solo lectura** (`:ro`) y **nunca** se
  incluyen en la imagen Docker.
- La llave privada (`private.pem.key`) es secreta: **no subir a GitHub** (usar
  `.gitignore` para excluir `certs/`). Si se filtra, revocar el certificado en AWS.
- El bridge es el único componente con acceso a internet/AWS; el ESP8266 solo habla
  MQTT plano en la red local.
- La **alarma local** (LEDs + buzzer) opera independiente de la red: funciona aunque
  se pierda la conexión a la nube.

---

## Estado del proyecto y próximos pasos

- [x] Nodo sensor (ESP8266) leyendo y publicando por MQTT
- [x] Centro de control (Mosquitto + bridge en Docker)
- [x] Ingesta a AWS IoT Core validada end-to-end
- [ ] IoT Rule → almacenamiento (DynamoDB) + histórico (S3)
- [ ] Alertas remotas (CloudWatch + SNS)
- [ ] API de lectura (API Gateway + Lambda)
- [ ] Dashboard (S3 + CloudFront)