FROM python:3.12-slim

# Logs sem buffer, para aparecerem no "docker compose logs -f"
ENV PYTHONUNBUFFERED=1

WORKDIR /app

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY collector.py .

CMD ["python", "collector.py"]
