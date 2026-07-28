# NAM Parametric Training Container for Vertex AI
FROM pytorch/pytorch:2.1.0-cuda12.1-cudnn8-runtime

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
