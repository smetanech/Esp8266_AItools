#!/usr/bin/env bash
# Запуск Phone Relay в Termux (Android).
# Скрипт ставит зависимости при первом запуске и стартует сервер.
set -e

cd "$(dirname "$0")"

echo "==> Установка зависимостей (если нужно)..."
pip install -r requirements.txt

echo "==> Запуск relay_server.py на :8000 ..."
python relay_server.py