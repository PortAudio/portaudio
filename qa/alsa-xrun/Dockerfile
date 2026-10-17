FROM python:3.14-slim-bookworm
RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake build-essential libasound2-dev && rm -rf /var/lib/apt/lists/*
RUN pip install --no-cache-dir sounddevice==0.5.3 cffi==2.1.1 pycparser==3.11
