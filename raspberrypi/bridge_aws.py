#!/usr/bin/env python3
"""
============================================================
 Bridge Raspberry Pi: Mosquitto local  ->  AWS IoT Core
 Proyecto DataGuard - Monitoreo de cuarto de servidores
 (Version contenerizada - config por variables de entorno)
============================================================

 Flujo:
   ESP8266 --(MQTT plano)--> Mosquitto --(se suscribe este bridge)--> AWS IoT Core (MQTT/TLS)

 IMPORTANTE (Docker):
   Dentro del contenedor, 'localhost' es el propio contenedor, NO la Pi.
   Por eso LOCAL_BROKER se configura por variable de entorno.
   Opciones tipicas:
     - host.docker.internal  (si Mosquitto corre en la Pi, fuera del contenedor)
     - la IP de la Pi en la LAN (ej. 10.85.1.30)
     - el nombre del servicio mosquitto (si ambos van en el mismo docker-compose)
"""

import os
import json
import time
import datetime
from paho.mqtt import client as paho_mqtt
from awscrt import mqtt
from awsiot import mqtt_connection_builder

# ===================== CONFIGURACION (por variables de entorno) =====================

# --- Mosquitto local ---
LOCAL_BROKER = os.getenv("LOCAL_BROKER", "host.docker.internal")
LOCAL_PORT   = int(os.getenv("LOCAL_PORT", "1883"))
LOCAL_TOPIC  = os.getenv("LOCAL_TOPIC", "datacenter/rack/telemetria")
LOCAL_USER   = os.getenv("LOCAL_USER") or None
LOCAL_PASS   = os.getenv("LOCAL_PASS") or None

# --- AWS IoT Core ---
AWS_ENDPOINT  = os.getenv("AWS_ENDPOINT", "xxxxxxxxxxxxxx-ats.iot.us-east-1.amazonaws.com")
AWS_PORT      = int(os.getenv("AWS_PORT", "8883"))
AWS_CLIENT_ID = os.getenv("AWS_CLIENT_ID", "rack-gw-01")
AWS_TOPIC     = os.getenv("AWS_TOPIC", "datacenter/rack/telemetria")

# --- Certificados (montados como volumen en /app/certs) ---
CERT_DIR  = os.getenv("CERT_DIR", "/app/certs")
PATH_CERT = f"{CERT_DIR}/certificate.pem.crt"
PATH_KEY  = f"{CERT_DIR}/private.pem.key"
PATH_CA   = f"{CERT_DIR}/AmazonRootCA1.pem"

# ===================== CONEXION A AWS =====================

aws_connection = None

def conectar_aws():
    global aws_connection
    print(f"[AWS] Conectando a {AWS_ENDPOINT} como '{AWS_CLIENT_ID}'...", flush=True)
    aws_connection = mqtt_connection_builder.mtls_from_path(
        endpoint=AWS_ENDPOINT,
        port=AWS_PORT,
        cert_filepath=PATH_CERT,
        pri_key_filepath=PATH_KEY,
        ca_filepath=PATH_CA,
        client_id=AWS_CLIENT_ID,
        clean_session=False,
        keep_alive_secs=30,
    )
    aws_connection.connect().result()
    print("[AWS] Conectado a IoT Core.", flush=True)

def publicar_aws(payload_dict):
    mensaje = json.dumps(payload_dict)
    aws_connection.publish(
        topic=AWS_TOPIC,
        payload=mensaje,
        qos=mqtt.QoS.AT_LEAST_ONCE,
    )
    print(f"[AWS -> {AWS_TOPIC}] {mensaje}", flush=True)

# ===================== CALLBACKS MOSQUITTO LOCAL =====================

def on_connect_local(client, userdata, flags, rc):
    if rc == 0:
        print(f"[LOCAL] Conectado a Mosquitto ({LOCAL_BROKER}:{LOCAL_PORT}). "
              f"Suscribiendo a '{LOCAL_TOPIC}'...", flush=True)
        client.subscribe(LOCAL_TOPIC)
    else:
        print(f"[LOCAL] Fallo de conexion a Mosquitto, rc={rc}", flush=True)

def on_message_local(client, userdata, msg):
    crudo = msg.payload.decode("utf-8", errors="replace")
    try:
        data = json.loads(crudo)
    except json.JSONDecodeError:
        print(f"[LOCAL] Mensaje no es JSON valido, se ignora: {crudo}", flush=True)
        return

    # Marca temporal REAL (UTC). El ESP8266 no tiene reloj confiable.
    ahora_utc = datetime.datetime.now(datetime.timezone.utc)
    data["ts_utc"] = ahora_utc.isoformat()
    data["ts_epoch"] = int(ahora_utc.timestamp())
    data.pop("ts", None)

    try:
        publicar_aws(data)
    except Exception as e:
        print(f"[AWS] Error al publicar: {e}", flush=True)

# ===================== MAIN =====================

def main():
    # 1) AWS primero
    while True:
        try:
            conectar_aws()
            break
        except Exception as e:
            print(f"[AWS] No se pudo conectar: {e}. Reintentando en 5s...", flush=True)
            time.sleep(5)

    # 2) Mosquitto local
    local = paho_mqtt.Client(client_id="bridge-pi", clean_session=True)
    if LOCAL_USER:
        local.username_pw_set(LOCAL_USER, LOCAL_PASS)
    local.on_connect = on_connect_local
    local.on_message = on_message_local

    while True:
        try:
            local.connect(LOCAL_BROKER, LOCAL_PORT, keepalive=60)
            break
        except Exception as e:
            print(f"[LOCAL] No se pudo conectar a Mosquitto: {e}. Reintentando en 5s...", flush=True)
            time.sleep(5)

    print("[BRIDGE] Puente activo. Esperando mensajes del ESP8266...", flush=True)
    local.loop_forever()

if __name__ == "__main__":
    main()
