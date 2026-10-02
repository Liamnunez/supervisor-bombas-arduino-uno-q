# ============================================================
# Dockerfile - Supervisor de Bombas Telemetry (FastAPI)
# Multi-stage: build deps -> runtime
# ============================================================

# --- Build stage ---
FROM python:3.12-slim AS builder

WORKDIR /app

# System deps for building wheels (if any)
RUN apt-get update && apt-get install -y --no-install-recommends \
    gcc \
    libpq-dev \
    && rm -rf /var/lib/apt/lists/*

# Install Python deps to /install
COPY linux/requirements.txt .
RUN pip install --no-cache-dir --prefix=/install -r requirements.txt


# --- Runtime stage ---
FROM python:3.12-slim AS runtime

WORKDIR /app

# Runtime system deps
RUN apt-get update && apt-get install -y --no-install-recommends \
    libpq5 \
    curl \
    && rm -rf /var/lib/apt/lists/*

# Copy installed packages from builder
COPY --from=builder /install /usr/local

# Copy application code
COPY linux/app ./app
COPY linux/static ./static

# Create non-root user
RUN useradd -m -u 1000 supervisor && \
    chown -R supervisor:supervisor /app
USER supervisor

# Environment defaults (override via docker-compose / env file)
ENV PYTHONUNBUFFERED=1 \
    PYTHONDONTWRITEBYTECODE=1 \
    SUPERVISOR_ALERTS_DB=/data/alerts.db \
    SUPERVISOR_AUDIT_LOG=/data/audit.log \
    SUPERVISOR_MAINTENANCE_DB=/data/maintenance.db \
    SUPERVISOR_METRICS_DB=/data/metrics.db

# Expose FastAPI port
EXPOSE 8080

# Health check
HEALTHCHECK --interval=30s --timeout=5s --start-period=10s --retries=3 \
    CMD curl -f http://localhost:8080/api/status || exit 1

# Run as module (relative imports work)
CMD ["python", "-m", "app.main"]