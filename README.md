# DataGuard — Monitoreo IoT de Cuarto de Servidores

Solución IoT distribuida que monitorea en tiempo casi-real las condiciones ambientales
y de seguridad de un cuarto de servidores: **temperatura, humedad y humo**. Un nodo
sensor publica la telemetría a un centro de control que la reenvía a AWS, donde se
almacena, se visualiza en un dashboard web, se generan alertas automáticas por correo y
se conserva un histórico consultable con SQL.

**Caso de negocio (simulado):** *DataGuard Servicios Tecnológicos, S.A.*, empresa
guatemalteca de hospedaje de servidores, necesita prevenir sobrecalentamiento, humedad
fuera de rango y conatos de incendio en su rack principal, con alertas inmediatas y
evidencia auditable, sin invertir en infraestructura de servidores propia para el
monitoreo.

---

## Arquitectura general

```
   [ Sensores ]              [ NODO SENSOR ]         [ CENTRO DE CONTROL ]       [ AWS ]
   DHT22 (temp/hum) -dig-+
   MQ-2  (humo)     -A0--+
                         +-> ESP8266 -WiFi/MQTT-> Raspberry Pi -MQTT/TLS-> AWS IoT Core
   LCD 16x2 (I2C)   <----+     (Mosquitto +              |
   Buzzer + LEDs    <----+      bridge, Docker)          |
       (alarma local)                                    |
                                                         v
                              +------------------+-------+-------+------------------+
                              |                  |               |                  |
                         Rule->DynamoDB     Rule->S3        Rule->SNS          (lectura)
                         (datos calientes) (histórico)    (alerta correo)          |
                              ^                  |                            Lambda+API GW
                              |                  v                                 |
                         Lambda lectura       Athena                          Dashboard S3
                              ^              (consultas SQL)
                              |
                         API Gateway -> Dashboard web
```

**Patrón de diseño:** arquitectura de dos caminos.
- **Hot path** (datos en vivo): IoT Core → DynamoDB → Lambda → API Gateway → Dashboard.
- **Cold path** (análisis histórico): IoT Core → S3 → Athena (SQL).

**Región AWS:** todo el proyecto vive en `us-east-2` (Ohio).

---

## Servicios y tecnologías

**Capa física (edge):** ESP8266, sensores DHT22 y MQ-2, LCD 16x2 I2C, LEDs, buzzer,
Raspberry Pi con Docker (Mosquitto + bridge Python).

**Capa de nube (AWS):** IoT Core, DynamoDB, Lambda, API Gateway, S3 (dashboard +
histórico), SNS, Athena, IAM.

---

## Tabla de contenidos

1. [Hardware necesario](#1-hardware-necesario)
2. [Cableado del nodo ESP8266](#2-cableado-del-nodo-esp8266)
3. [Parte A — Nodo sensor (ESP8266)](#3-parte-a--nodo-sensor-esp8266)
4. [Parte B — Centro de control (Raspberry Pi)](#4-parte-b--centro-de-control-raspberry-pi)
5. [Parte C — AWS IoT Core (ingesta)](#5-parte-c--aws-iot-core-ingesta)
6. [Parte D — Almacenamiento (DynamoDB)](#6-parte-d--almacenamiento-dynamodb)
7. [Parte E — API de lectura (Lambda + API Gateway)](#7-parte-e--api-de-lectura-lambda--api-gateway)
8. [Parte F — Dashboard web (S3)](#8-parte-f--dashboard-web-s3)
9. [Parte G — Alertas remotas (SNS)](#9-parte-g--alertas-remotas-sns)
10. [Parte H — Histórico y análisis (S3 + Athena)](#10-parte-h--histórico-y-análisis-s3--athena)
11. [Verificación de extremo a extremo](#11-verificación-de-extremo-a-extremo)
12. [Decisiones de diseño y alcance](#12-decisiones-de-diseño-y-alcance)
13. [Solución de problemas](#13-solución-de-problemas)
14. [Seguridad y optimización de costos](#14-seguridad-y-optimización-de-costos)
15. [Resumen de recursos AWS](#15-resumen-de-recursos-aws)

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

> **D0 (GPIO16)** no soporta interrupciones. Suele funcionar con el DHT22 en polling,
> pero si da lecturas `NaN`, mover el dato del DHT22 a **D3 o D4** y actualizar `PIN_DHT`.

> **MQ-2:** requiere varios minutos de calentamiento ("burn-in") antes de dar lecturas
> estables. La lectura es cruda (0–1023), no PPM.

---

## 3. Parte A — Nodo sensor (ESP8266)

### 3.1 Entorno de desarrollo

1. Instalar **Arduino IDE**.
2. Soporte ESP8266: `Archivo → Preferencias → URLs adicionales`:
   ```
   http://arduino.esp8266.com/stable/package_esp8266com_index.json
   ```
   Luego `Herramientas → Placa → Gestor de tarjetas` → instalar **esp8266**.
3. Seleccionar placa (**NodeMCU 1.0**) y puerto COM.

### 3.2 Librerías requeridas

`Herramientas → Gestor de librerías`:

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
const char* MQTT_BROKER   = "192.168.1.XX";   // obtener con: hostname -I  en la Pi
const int   MQTT_PORT     = 1883;
const char* MQTT_TOPIC    = "datacenter/rack/telemetria";

// Umbrales de alarma
const float TEMP_WARN  = 26.0;
const float TEMP_CRIT  = 30.0;
const int   SMOKE_WARN = 300;
const int   SMOKE_CRIT = 450;
```

> El LCD trae **escáner I2C automático**: detecta la dirección (`0x27` o `0x3F`) al
> arrancar y la imprime por Serial.

### 3.4 Cargar y verificar

Monitor Serial a `115200`. Secuencia esperada: test de LEDs (verde→amarillo→rojo),
dirección I2C del LCD, conexión WiFi, conexión MQTT, y el JSON cada 15 s:

```json
{"device":"nodo-rack-01","temp":27.4,"hum":61.2,"smoke":342,"alarm":false,"state":"NORMAL","ts":12345}
```

---

## 4. Parte B — Centro de control (Raspberry Pi)

Dos contenedores Docker: **Mosquitto** (broker) y el **bridge** (reenvío a AWS por TLS).

### 4.1 Requisitos

```bash
curl -fsSL https://get.docker.com | sh
sudo usermod -aG docker $USER   # opcional (re-login)
```

### 4.2 Estructura de carpetas

```bash
mkdir -p ~/centro-control/mosquitto/config
mkdir -p ~/certs
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

### 4.4 Construir la imagen del bridge (desde la PC, para arm64)

```bash
# Carpeta con Dockerfile, bridge_aws.py, requirements.txt
docker build --platform=linux/arm64 -t jzuletadev/dataguard-bridge:arm64 --push .
```

> Los **certificados NO van dentro de la imagen**: se montan como volumen en runtime.

### 4.5 Levantar el centro de control

> Si había Mosquitto por `apt`, desactivarlo para liberar el puerto 1883:
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

> Ajustar `/home/why/certs` al usuario real de la Pi. `AWS_ENDPOINT` se obtiene en la
> Parte C. El bridge agrega el timestamp real (UTC) a cada mensaje, ya que el ESP8266
> no tiene reloj confiable.

---

## 5. Parte C — AWS IoT Core (ingesta)

### 5.1 Obtener el endpoint de datos

**AWS IoT → Connect → Connect one device**. En el primer paso, AWS muestra el
**Device data endpoint**: `XXXXXXXX-ats.iot.us-east-2.amazonaws.com`. Es el `AWS_ENDPOINT`.

> En cuentas nuevas, CloudShell puede estar bloqueado por verificación; el endpoint se
> obtiene igualmente desde el asistente.

### 5.2 Crear el Thing y los certificados

Asistente **Connect one device**:
1. **Create a new thing** → nombre `rack-gw-01` (= `AWS_CLIENT_ID`).
2. Plataforma **Linux**, SDK **Python**.
3. Descargar el connection kit — **todos** los archivos (la llave privada no se puede
   volver a descargar).

Mapeo de archivos:

| AWS entrega | Es el... | Renombrar a |
|---|---|---|
| `rack-gw-01.cert.pem` | Certificado | `certificate.pem.crt` |
| `rack-gw-01.private.key` | Llave privada | `private.pem.key` |
| `rack-gw-01.public.key` | Llave pública | *(no se usa)* |

> Windows muestra las `.key` como "Keynote Presentation": es solo el icono; son texto
> plano. Renombrar la extensión no daña el contenido.

### 5.3 Descargar el Amazon Root CA 1

```bash
cd ~/certs
curl -o AmazonRootCA1.pem https://www.amazontrust.com/repository/AmazonRootCA1.pem
```

### 5.4 Colocar y renombrar en la Pi

```bash
cd ~/certs
mv rack-gw-01.cert.pem    certificate.pem.crt
mv rack-gw-01.private.key private.pem.key
ls -l ~/certs   # debe mostrar: AmazonRootCA1.pem  certificate.pem.crt  private.pem.key
```

### 5.5 Activar el certificado y adjuntar la política (PASO CLAVE)

Sin esto, el TLS conecta pero AWS cierra la sesión (`AWS_ERROR_MQTT_UNEXPECTED_HANGUP`).

1. **Security → Certificates** → certificado **Active** (si no: `Actions → Activate`).
2. **Security → Policies → Create policy** → nombre `rack-gw-policy`, editor JSON:
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
3. **Security → Certificates** → certificado → `Actions → Attach policy` →
   `rack-gw-policy`.

### 5.6 Verificar conexión

```bash
docker restart dataguard-bridge
docker logs -f dataguard-bridge
```

Esperado:
```
[AWS] Conectado a IoT Core.
[LOCAL] Conectado a Mosquitto (dataguard-mosquitto:1883)...
[BRIDGE] Puente activo. Esperando mensajes del ESP8266...
[AWS -> datacenter/rack/telemetria] {"device":"nodo-rack-01","temp":28,...}
```

Confirmar en consola: **IoT Core → MQTT test client** → suscribir a
`datacenter/rack/telemetria`.

---

## 6. Parte D — Almacenamiento (DynamoDB)

### 6.1 Crear la tabla

**DynamoDB → Create table**:
- **Table name:** `telemetria_rack`
- **Partition key:** `device` (String)
- **Sort key:** `ts_epoch` (Number)
- **Settings:** Default (on-demand, serverless)

> El patrón device + ts_epoch permite consultar por dispositivo y rango de tiempo, y
> escala a múltiples nodos sin rediseño.

### 6.2 Regla IoT que guarda los datos

**IoT Core → Message routing → Rules → Create rule**:
- **Rule name:** `rule_telemetria_a_dynamo`
- **SQL:** `SELECT * FROM 'datacenter/rack/telemetria'`
- **Action:** **DynamoDBv2** (mapea cada campo del JSON a una columna; la v1 guardaría
  todo en un solo atributo), tabla `telemetria_rack`, IAM role nuevo `rol_iot_dynamo`.

### 6.3 Verificación

**DynamoDB → Tables → telemetria_rack → Explore table items**: filas con columnas
`device`, `ts_epoch`, `temp`, `hum`, `smoke`, `alarm`, `state`, `ts_utc`.

---

## 7. Parte E — API de lectura (Lambda + API Gateway)

### 7.1 Lambda

**Lambda → Create function** → Author from scratch:
- **Name:** `leer_telemetria`, **Runtime:** Python 3.13.

```python
import json
import boto3
from boto3.dynamodb.conditions import Key
from decimal import Decimal

dynamodb = boto3.resource('dynamodb', region_name='us-east-2')
tabla = dynamodb.Table('telemetria_rack')

def limpiar(obj):
    if isinstance(obj, list):
        return [limpiar(i) for i in obj]
    if isinstance(obj, dict):
        return {k: limpiar(v) for k, v in obj.items()}
    if isinstance(obj, Decimal):
        return int(obj) if obj % 1 == 0 else float(obj)
    return obj

def lambda_handler(event, context):
    device = 'nodo-rack-01'
    limite = 50
    resp = tabla.query(
        KeyConditionExpression=Key('device').eq(device),
        ScanIndexForward=False,
        Limit=limite
    )
    items = limpiar(resp.get('Items', []))
    items.reverse()
    return {
        'statusCode': 200,
        'headers': {
            'Content-Type': 'application/json',
            'Access-Control-Allow-Origin': '*'
        },
        'body': json.dumps(items)
    }
```

> Tras pegar el código, hacer clic en **Deploy** (si no, corre el código viejo).

### 7.2 Permiso de lectura

Lambda → **Configuration → Permissions** → clic en el Execution role → en IAM:
**Add permissions → Attach policies → AmazonDynamoDBReadOnlyAccess**.

### 7.3 API Gateway

**API Gateway → HTTP API → Build**:
- **Integration:** Lambda `leer_telemetria`
- **API name:** `api_telemetria`
- **Route:** `GET /lecturas`
- **CORS:** Allow-Origin `*`, Allow-Methods `GET`, Allow-Headers `*`

URL: `https://<api-id>.execute-api.us-east-2.amazonaws.com/lecturas`. Probar en el
navegador: debe devolver el JSON con las lecturas.

---

## 8. Parte F — Dashboard web (S3)

Archivo `index.html` autocontenido (HTML + Chart.js) que consume el API cada 15 s y
grafica temperatura, humedad y humo con semáforo de estado. En el código, `API_URL`
apunta al endpoint `.../lecturas`.

### 8.1 Bucket y hospedaje

**S3 → Create bucket** → `dataguard-dashboard-rack-01`, region us-east-2. Subir
`index.html`.

Hospedaje mediante **S3 Static Website Hosting** (público, HTTP):
- Properties → Static website hosting → Enable, index document `index.html`.
- Permissions → desactivar Block Public Access.
- Bucket policy:
  ```json
  {
    "Version": "2012-10-17",
    "Statement": [
      {
        "Effect": "Allow",
        "Principal": "*",
        "Action": "s3:GetObject",
        "Resource": "arn:aws:s3:::dataguard-dashboard-rack-01/*"
      }
    ]
  }
  ```

URL en Properties → Static website hosting:
`http://dataguard-dashboard-rack-01.s3-website.us-east-2.amazonaws.com`

---

## 9. Parte G — Alertas remotas (SNS)

### 9.1 Tema y suscripción

**SNS → Topics → Create topic**: Standard, nombre `alertas_rack`.
**Create subscription**: Email, endpoint = correo del equipo. **Confirmar** desde el
correo recibido (sin confirmar, no llegan alertas).

### 9.2 Regla IoT de alerta crítica

**IoT Core → Message routing → Rules → Create rule**:
- **Rule name:** `rule_alerta_critica`
- **SQL:**
  ```sql
  SELECT *, 'Cuarto de servidores: condicion critica detectada' AS mensaje
  FROM 'datacenter/rack/telemetria'
  WHERE temp >= 30 OR smoke >= 450
  ```
- **Action:** **SNS** → topic `alertas_rack`, formato **RAW**, IAM role nuevo
  `rol_iot_sns`.

### 9.3 Prueba

Subir la temperatura del DHT22 (calor real) hasta cruzar 30 °C, o acercar humo al MQ-2
hasta cruzar 450. Debe llegar el correo de alerta.

---

## 10. Parte H — Histórico y análisis (S3 + Athena)

### 10.1 Bucket de histórico

**S3 → Create bucket** → `dataguard-historico-rack-01`, region us-east-2, **Block
Public Access activado** (datos privados de auditoría).

### 10.2 Regla IoT que archiva en S3 (particionado por fecha)

**IoT Core → Message routing → Rules → Create rule**:
- **Rule name:** `rule_historico_a_s3`
- **SQL:** `SELECT * FROM 'datacenter/rack/telemetria'`
- **Action:** **S3**, bucket `dataguard-historico-rack-01`, IAM role `rol_iot_s3`.
- **Key:**
  ```
  telemetria/${parse_time("yyyy/MM/dd", timestamp())}/${newuuid()}.json
  ```
  Esto guarda cada lectura como `telemetria/2026/06/14/<uuid>.json`, particionada por
  fecha.

### 10.3 Athena — configuración y tabla

1. **Athena → Query editor**. Configurar ubicación de resultados:
   `s3://dataguard-historico-rack-01/athena-results/`.
2. Crear base de datos:
   ```sql
   CREATE DATABASE dataguard;
   ```
3. Crear la tabla externa sobre el histórico:
   ```sql
   CREATE EXTERNAL TABLE IF NOT EXISTS dataguard.telemetria (
     device   string,
     temp     double,
     hum      double,
     smoke    int,
     alarm    boolean,
     state    string,
     ts_utc   string,
     ts_epoch bigint
   )
   ROW FORMAT SERDE 'org.openx.data.jsonserde.JsonSerDe'
   LOCATION 's3://dataguard-historico-rack-01/telemetria/'
   TBLPROPERTIES ('has_encrypted_data'='false');
   ```

### 10.4 Consultas de análisis

```sql
-- Comprobar lectura
SELECT * FROM dataguard.telemetria LIMIT 10;

-- Temperatura promedio, mínima y máxima
SELECT ROUND(AVG(temp),2) AS temp_promedio,
       MIN(temp) AS temp_minima,
       MAX(temp) AS temp_maxima
FROM dataguard.telemetria;

-- Eventos críticos
SELECT COUNT(*) AS eventos_criticos
FROM dataguard.telemetria
WHERE temp >= 30 OR smoke >= 450;

-- Lecturas por estado
SELECT state, COUNT(*) AS total
FROM dataguard.telemetria
GROUP BY state;

-- Promedio por hora (¿cuándo se calienta el cuarto?)
SELECT substr(ts_utc,1,13) AS hora,
       ROUND(AVG(temp),1) AS temp_prom,
       MAX(smoke) AS humo_max
FROM dataguard.telemetria
GROUP BY substr(ts_utc,1,13)
ORDER BY hora;
```

> Cada consulta escanea solo KB de datos (costo de fracciones de centavo a esta escala).

---

## 11. Verificación de extremo a extremo

| # | Qué se valida | Cómo |
|---|---|---|
| 1 | Mosquitto recibe del ESP8266 | `docker exec -it dataguard-mosquitto mosquitto_sub -t "datacenter/rack/telemetria" -v` |
| 2 | Bridge reenvía a AWS | `docker logs -f dataguard-bridge` → líneas `[AWS -> ...]` |
| 3 | AWS recibe | IoT Core → MQTT test client → suscribir al tópico |
| 4 | Se almacena | DynamoDB → Explore table items |
| 5 | API devuelve datos | abrir la URL `.../lecturas` en el navegador |
| 6 | Dashboard funciona | abrir la URL del sitio S3 |
| 7 | Alerta llega | cruzar umbral (calor/humo) → correo SNS |
| 8 | Histórico y análisis | S3 con archivos por fecha; consulta en Athena |

---

## 12. Decisiones de diseño y alcance

**Arquitectura de dos caminos (hot / cold path).** DynamoDB sirve datos recientes al
dashboard (acceso rápido, NoSQL, encaja con JSON); S3 + Athena cubren el análisis
histórico con SQL. Cada uno hace lo que el otro hace mal.

**Sin Lambda en la ingesta.** La regla escribe directo a DynamoDB y a S3, porque el
dato ya viene validado y con timestamp desde el edge (la Raspberry Pi). Lambda se usa
solo en la lectura, donde es imprescindible (el navegador no puede consultar DynamoDB
directamente). Esto evita un servicio innecesario en la ruta de escritura.

**Gateway edge (Raspberry Pi) en vez de dispositivo directo.** El ESP8266 publica MQTT
plano a Mosquitto; la Pi maneja el TLS pesado hacia AWS y custodia los certificados de
forma centralizada. Permite escalar a N nodos sin tocar la nube y habilita
store-and-forward. La alarma local (LEDs + buzzer) opera independiente de la red.

**Alcance deliberado — servicios no incluidos:**
- **CloudFront:** la arquitectura objetivo era S3 privado + CloudFront (HTTPS/CDN). Se
  optó por S3 Static Website Hosting porque cubre la necesidad de servir el dashboard y
  los servicios desplegados ya superan el mínimo requerido. CloudFront queda
  documentado como evolución futura para HTTPS y distribución global.
- **CloudWatch (métricas/dashboards):** las alertas reactivas se resuelven con la regla
  IoT → SNS, que es más inmediata y directa que publicar métricas custom a CloudWatch.
  Se documenta como evolución futura para observabilidad y paneles operativos.

**Servicios AWS desplegados:** 8 (IoT Core, DynamoDB, Lambda, API Gateway, S3, SNS,
Athena, IAM). El mínimo requerido por el proyecto es 4.

---

## 13. Solución de problemas

| Síntoma | Causa probable | Solución |
|---|---|---|
| ESP8266: `MQTT fallo, rc=-2` | IP del broker incorrecta | `hostname -I` (mayúscula) en la Pi |
| `rc=-2` con IP correcta | Mosquitto no escucha en la red | `listener 1883`; `ss -tlnp \| grep 1883` |
| LCD en blanco | Contraste / dirección I2C | Potenciómetro del módulo; revisar dirección en Serial |
| DHT22 devuelve `NaN` | Pin D0 sin interrupciones | Mover a D3/D4, actualizar `PIN_DHT` |
| MQ-2 marca muy alto al inicio | Falta calentamiento | Dejar encendido varios minutos |
| Bridge: `No such file .../app/certs/...` | Certificados mal nombrados / volumen mal montado | Verificar nombres en `~/certs` y la ruta del `-v` |
| Bridge: `AWS_ERROR_MQTT_UNEXPECTED_HANGUP` | Certificado inactivo o **sin política** | Activar certificado y adjuntar `rack-gw-policy` |
| Bridge: `[Errno -5] No address associated with hostname` | `LOCAL_BROKER` ≠ nombre del contenedor | Usar `LOCAL_BROKER=dataguard-mosquitto` |
| Lambda devuelve "Hello from Lambda" | Código no desplegado | Botón **Deploy** en el editor |
| Lambda: `AccessDenied` en DynamoDB | Falta política en el rol | Adjuntar `AmazonDynamoDBReadOnlyAccess` |
| Dashboard sin datos | CORS o `API_URL` incorrecta | Verificar CORS en API Gateway y la URL en el HTML |
| CloudFront/CloudShell bloqueados | Verificación de cuenta nueva | Esperar verificación o abrir caso a Support |

---

## 14. Seguridad y optimización de costos

**Seguridad:**
- Certificados montados como volumen **solo lectura** (`:ro`); **nunca** dentro de la
  imagen Docker. La llave privada no se sube a control de versiones (`.gitignore`
  excluye `certs/`).
- El bridge es el único componente con acceso a internet/AWS; el ESP8266 solo habla
  MQTT plano en la red local.
- Las políticas IAM actuales (`Resource: *`, `AmazonDynamoDBReadOnlyAccess`) permiten
  validar el sistema. **Mejora recomendada:** restringir cada política a su recurso
  específico (Thing, tópico, tabla) aplicando mínimo privilegio.

**Optimización de costos (Cloud Rightsizing):**
- Diseño íntegramente **serverless** (IoT Core, Lambda, DynamoDB on-demand, S3, Athena):
  el costo escala con el uso, prácticamente $0 a escala de laboratorio.
- **DynamoDB:** activar **TTL** sobre `ts_epoch` para autodepurar datos calientes
  pasados X días y no acumular almacenamiento.
- **S3 histórico:** política de ciclo de vida (p. ej., objetos > 90 días → Glacier o
  eliminación).
- **Athena:** el costo depende de datos escaneados; el particionado por fecha ya
  limita el escaneo. Para mayor escala, agrupar lecturas (Kinesis Firehose) o convertir
  a Parquet comprimido.

**Limitación conocida (alertas):** la regla de alerta dispara con cada lectura que
cumple la condición; si la variable se mantiene sobre el umbral, llega un correo cada
15 s. En producción convendría una Lambda intermedia con estado (cooldown /
deduplicación) que además formatee un correo legible en lugar del JSON crudo (RAW).

---

## 15. Resumen de recursos AWS

| Servicio | Recurso | Nombre |
|---|---|---|
| IoT Core | Thing | `rack-gw-01` |
| IoT Core | Policy | `rack-gw-policy` |
| IoT Core | Rule (guardar) | `rule_telemetria_a_dynamo` |
| IoT Core | Rule (histórico) | `rule_historico_a_s3` |
| IoT Core | Rule (alerta) | `rule_alerta_critica` |
| DynamoDB | Tabla | `telemetria_rack` |
| Lambda | Función | `leer_telemetria` |
| API Gateway | HTTP API | `api_telemetria` (GET /lecturas) |
| S3 | Bucket (dashboard) | `dataguard-dashboard-rack-01` |
| S3 | Bucket (histórico) | `dataguard-historico-rack-01` |
| SNS | Topic | `alertas_rack` |
| Athena | Base de datos | `dataguard` |
| Athena | Tabla externa | `telemetria` |
| IAM | Roles | `rol_iot_dynamo`, `rol_iot_s3`, `rol_iot_sns`, rol de ejecución de Lambda |

---

## Estado del proyecto

- [x] Nodo sensor (ESP8266): lectura, alarma local y publicación MQTT
- [x] Centro de control (Mosquitto + bridge en Docker)
- [x] Ingesta a AWS IoT Core (TLS, certificados X.509)
- [x] Almacenamiento en DynamoDB
- [x] API de lectura (Lambda + API Gateway)
- [x] Dashboard web (S3 Static Website Hosting)
- [x] Alertas remotas (IoT Rule → SNS → correo)
- [x] Histórico crudo en S3 (particionado por fecha)
- [x] Análisis de datos con Athena (SQL)

Sistema funcional de extremo a extremo: del sensor físico al análisis en la nube.