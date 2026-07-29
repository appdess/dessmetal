# NAM Parametric Training Container for Vertex AI
FROM pytorch/pytorch:2.6.0-cuda12.4-cudnn9-runtime@sha256:77f17f843507062875ce8be2a6f76aa6aa3df7f9ef1e31d9d7432f4b0f563dee

# Set working directory
WORKDIR /app

# Install system dependencies (non-interactive)
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y \
    libsndfile1 \
    ffmpeg \
    git \
    libportaudio2 \
    && rm -rf /var/lib/apt/lists/*

# Copy requirements first for better caching  
COPY cloud/requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

# Copy all trainer_source code
COPY . /app/trainer_source/

# Set Python path
ENV PYTHONPATH=/app:$PYTHONPATH

# Default entrypoint for Vertex AI custom training
ENTRYPOINT ["python", "-u", "/app/trainer_source/cloud/cloud_train_entrypoint.py"]
