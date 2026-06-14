FROM python:3.11-slim

# Directorio de trabajo dentro del contenedor
WORKDIR /app

# Instalar dependencias primero (mejor cacheo de capas)
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

# Copiar el script del bridge
COPY bridge_aws.py .

# Los certificados NO se copian a la imagen: se montan como volumen en runtime.
# El codigo los espera en /app/certs (ver CERT_DIR en bridge_aws.py).

# Ejecutar el bridge
CMD ["python", "-u", "bridge_aws.py"]
