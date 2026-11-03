#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# simulator_web_ui.py — скриншоты встроенной веб-панели ESP8266 (browser view)
# ---------------------------------------------------------------------------
# Воспроизводит вид встроенного веб-интерфейса (index_html из 8.6.ino):
# тёмная тема, голубые кнопки, шесть секций — Status, Prompts, Notes,
# Messages, Notice, Logs. Рисует в браузерном окне и сохраняет в screenshots/web/.
#
# Запуск:
#   pip install pillow
#   python simulator_web_ui.py
# ---------------------------------------------------------------------------
import os
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

OUT = Path(__file__).parent / "screenshots" / "web"
OUT.mkdir(parents=True, exist_ok=True)

FDIR = "C:/Windows/Fonts"


def font(size, bold=False):
    p = os.path.join(FDIR, "arialbd.ttf" if bold else "arial.ttf")
    if os.path.exists(p):
        return ImageFont.truetype(p, size)
    return ImageFont.load_default()


# Палитра из index_html прошивки
BODY = "#1a1a1a"; CARD = "#2a2a2a"; LINE = "#444444"
INPUT = "#1a1a1a"; BTN = "#4fc3f7"; BTN_D = "#ef5350"
TXT = "#e0e0e0"; SUB = "#81d4fa"; OK = "#4caf50"; ERR = "#f44336"

PAD = 15            # отступ контента внутри карточки
R = 8               # радиус скругления карточек


class Page:
    """Рисует окно браузера с адресной строкой и карточками веб-панели."""

    def __init__(self, width=840):
        self.W = width
        # высота подстраивается автоматически
        self.H = 46
        self.img = Image.new("RGB", (self.W, 5000), "#121212")
        self.d = ImageDraw.Draw(self.img)

    def _addr(self):
        self.d.rectangle([0, 0, self.W, 34], fill="#2c2c2c")
        self.d.rounded_rectangle([16, 9, self.W - 16, 27], 9, fill="#1a1a1a",
                                 outline="#3a3a3a")
        self.d.text((30, 12), "http://192.168.4.1/", font=font(13), fill=TXT)
        for dx in (self.W - 22, self.W - 38):
            self.d.ellipse([dx, 12, dx + 11, 23], fill="#444")

    def card(self, title, lines, actions=True):
        """Одна секция веб-панели. lines — список кортежей (kind, text)."""
        x = PAD
        y = self.H
        # сначала замерим высоту: заголовок + по 30px на каждую строку
        inner = 34  # заголовок + линия под ним
        inner += len(lines) * 30
        if actions:
            inner += 44     # строка ввода + кнопка
        h = inner + 16      # нижний отступ карточки
        self.d.rounded_rectangle([x, y, x + self.W - PAD * 2, y + h - 1], R,
                                 fill=CARD)
        # заголовок
        self.d.text((x + PAD, y + 12), title, font=font(16, True), fill=SUB)
        tw = self.d.textlength(title, font=font(16, True))
        self.d.line([x + PAD, y + 34, x + PAD + tw, y + 34], fill=LINE, width=1)
        cy = y + 42
        for kind, text in lines:
            if kind == "item":                      # строка данных (№ + текст)
                self.d.text((x + PAD, cy), text, font=font(13), fill=TXT)
            elif kind == "status":                  # ключ: значение
                k, v = text
                self.d.text((x + PAD, cy), k, font=font(13), fill=TXT)
                self.d.text((x + PAD + 60, cy), v, font=font(13, True),
                            fill=(OK if "OK" in v or v in ("OK",) else TXT))
            cy += 30
        # строка действий (поле ввода + кнопка)
        if actions:
            ay = cy + 6
            self.d.rounded_rectangle([x + PAD, ay, x + self.W - PAD * 3, ay + 24],
                                     5, fill=INPUT, outline=LINE)
            self.d.rounded_rectangle([x + self.W - PAD * 2 - 46, ay, x + self.W - PAD * 2, ay + 24],
                                     5, fill=BTN)
            self.d.text((x + self.W - PAD * 2 - 40, ay + 5), "Add", font=font(12, True),
                        fill="#000")
        self.H = y + h + 12

    def save(self, name, caption=None):
        self._addr()
        img = self.img.crop((0, 0, self.W, self.H))
        if caption:
            cap = Image.new("RGB", (self.W, self.H + 30), "#121212")
            cap.paste(img, (0, 0))
            d = ImageDraw.Draw(cap)
            d.text((12, self.H + 8), caption, font=font(14), fill=SUB)
            img = cap
        img.save(str(OUT / name))
        print("  →", name)


def main():
    print("Собираю скриншоты веб-панели ESP...")

    # Страница 1 — обзор (dashboard): статус + свёрнутые секции
    p = Page()
    p.card("Status", [("status", ("WiFi:", "OK")),
                      ("status", ("RSSI:", "-58 dBm")),
                      ("status", ("IP:", "192.168.4.1")),
                      ("status", ("Phone:", "192.168.43.1")),
                      ("status", ("Sync:", "OK"))], actions=False)
    p.card("Prompts", [("item", "1. Tell me a joke"),
                       ("item", "2. Explain quantum physics"),
                       ("item", "3. Write a poem")])
    p.save("01_dashboard.png", "Веб-панель: статус устройства и список промптов")

    # Страница 2 — заметки и сообщения
    p = Page()
    p.card("Notes", [("item", "1. Shopping list"),
                     ("item", "2. Project idea"),
                     ("item", "3. Contacts")])
    p.card("Messages", [("item", "[You] Hello!"),
                        ("item", "[Web] How are you?"),
                        ("item", "[You] OK, soon")],
           actions=True)  # кнопка Send
    p.save("02_notes_messages.png", "Заметки и сообщения с полем ввода и кнопкой Send")

    # Страница 3 — заметка и логи
    p = Page()
    p.card("Notice", [("item", "Meeting at 15:00")])
    p.card("Logs", [("item", "[WiFi] OK 192.168.4.1"),
                    ("item", "[Sync] OK 5s"),
                    ("item", "[AI] thinking...")], actions=False)
    p.save("03_notice_logs.png", "Поле заметки и список логов устройства")


if __name__ == "__main__":
    main()