#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# simulator_oled.py — симуляция экрана ESP8266 AI Terminal (OLED SSD1306 128x64)
# ---------------------------------------------------------------------------
# Воспроизводит разметку и содержимое ключевых состояний из 8.6.ino как они
# выглядели бы на реальном дисплее. Рисует PNG-картинки в папку screenshots/.
#
# Принцип: сцена рисуется в большом разрешении (SCALE х пикселя на "пиксель"
# OLED), затем результат даунсэмплится по порогу -> классический пиксельный вид.
#
# Запуск:
#   pip install pillow
#   python simulator_oled.py
# ---------------------------------------------------------------------------
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

SCALE = 4            # сколько рендер-пикселей приходится на 1 пиксель OLED
GW, GH = 128, 64     # физическая матрица OLED
RW, RH = GW * SCALE, GH * SCALE   # размер растровой сцены

OUT = Path(__file__).parent / "screenshots"
OUT.mkdir(exist_ok=True)

# Пиксельный шрифт (Painter/базовый) 11px; рисуем в масштабе, потом ресэмплим.
TEXT_FONT = ImageFont.load_default(size=11 * SCALE)

# Шрифт для подписей под экранами (нужна кириллица).
import os
FDIR = "C:/Windows/Fonts"
_CAP_FONT_PATH = os.path.join(FDIR, "arial.ttf")
CAP_FONT = ImageFont.truetype(_CAP_FONT_PATH, 16) if os.path.exists(_CAP_FONT_PATH) else TEXT_FONT


class Screen:
    """Сцена-буфер в высоком разрешении. Методы повторяют draw-функции 8.6.ino."""

    def __init__(self):
        self.reset()

    def reset(self):
        self.img = Image.new("L", (RW, RH), 0)
        self.d = ImageDraw.Draw(self.img)
        self.draw_color = 255

    # --- низкоуровневые примитивы (работают в "пикселях OLED") ---

    def _xy(self, xy):
        return (xy[0] * SCALE, xy[1] * SCALE)

    def _rect(self, x0, y0, x1, y1):
        # rect принимает коорд с зазорами; охватываем целый "пиксель" OLED
        return (x0 * SCALE, y0 * SCALE, (x1 + 1) * SCALE - 1, (y1 + 1) * SCALE - 1)

    def px(self, xy, on=True):
        x, y = self._xy(xy)
        self.d.rectangle([x, y, x + SCALE - 1, y + SCALE - 1],
                         fill=self.draw_color if on else 0)

    def box(self, xy, on=True):
        x0, y0, x1, y1 = self._rect(*xy)
        self.d.rectangle([x0, y0, x1, y1], fill=self.draw_color if on else 0)

    def hline(self, y, x0=0, x1=GW - 1):
        self.d.rectangle([x0 * SCALE, y * SCALE, (x1 + 1) * SCALE - 1, (y + 1) * SCALE - 1],
                         fill=self.draw_color)

    def frame(self, xy):
        x0, y0, x1, y1 = self._rect(*xy)
        self.d.rectangle([x0, y0, x1, y1], outline=self.draw_color,
                         width=max(1, SCALE // 3))

    # --- текст (имитация u8g2.print) ---

    _CH = 10  # высота строки шрифта в "пикселях OLED"

    def font_w(self, s):
        """Оценочная ширина строки (моноширинный шрифт) в пикселях OLED."""
        return int(round(self.d.textlength(s, font=TEXT_FONT) / SCALE))

    def text(self, cur, s):
        """Печать строки. cur = (x, y) базовая линия в координатах OLED."""
        x, y = cur
        self.d.text((x * SCALE, (y - self._CH) * SCALE), s, font=TEXT_FONT,
                    fill=self.draw_color)

    def invert(self, rect, on=True):
        """Рисование белым по чёрному (инверсия региона) либо наоборот."""
        x0, y0, x1, y1 = self._rect(*rect)
        region = self.img.crop((x0, y0, x1 + 1, y1 + 1))
        region = region.point(lambda p: (255 - p) if on else p)
        self.img.paste(region, (x0, y0))

    # --- вывод ---

    def save(self, name, caption):
        """Даунсэмпл до 128x64 по порогу и сохранение PNG с подписью."""
        small = self.img.resize((GW, GH), Image.BOX)
        disp = small.point(lambda p: 255 if p > 110 else 0)   # пиксельный порог
        disp = disp.resize((GW * 6, GH * 6), Image.NEAREST)   # увеличение для просмотра
        # холст с рамкой и подписью
        margin = 40
        canvas = Image.new("RGB", (disp.width, disp.height + margin), (22, 24, 28))
        canvas.paste(disp.convert("RGB"), (0, 0))
        dr = ImageDraw.Draw(canvas)
        dr.rectangle([0, 0, disp.width - 1, disp.height - 1], outline=(70, 82, 100))
        dr.text((14, disp.height + 8), caption, fill=(150, 200, 255), font=CAP_FONT)
        canvas.save(OUT / name)
        print("  →", name)
        return self


# --------------------------- функционирующие состояние-функции ---------------------------

def s_splash():
    s = Screen()
    s.text(((128 - s.font_w("ESP8266 AI")) // 2, 20), "ESP8266 AI")
    s.text(((128 - s.font_w("Terminal v8.6")) // 2, 34), "Terminal v8.6")
    s.frame((14, 54, 114, 62))                    # рамка прогресса
    s.box((16, 56, 16 + int(96 * 0.55) - 1, 59))  # заполнение ~55%
    s.save("01_splash.png", "Загрузка: заставка + прогресс-бар")
    return s


def s_menu():
    s = Screen()
    s.text((0, 10), "Menu [1/8]")
    s.hline(12)
    for i, it in enumerate(["AI Chat", "Time", "Notice", "Maps", "Calc"]):
        s.text((0, 22 + i * 8), ("> " if i == 0 else "  ") + it)
    s.save("02_menu.png", "Главное меню (курсор на 1-м пункте)")
    return s


def s_stats():
    s = Screen()
    s.text((0, 8), "Stats"); s.hline(10)
    s.text((0, 20), "Wi:OK")
    s.text((0, 30), "R:-58dBm")
    s.text((0, 40), "IP:192.168.43.12")
    s.text((0, 50), "Ph:192.168.43.1")
    s.text((0, 60), "Sy:OK  5s")
    s.save("03_stats.png", "Статус: WiFi / RSSI / IP / синхронизация")
    return s


def s_ai_list():
    s = Screen()
    s.text((0, 10), "AI [1/8]"); s.hline(12)
    for i, it in enumerate(["Tell me a joke", "Explain quantum", "Write a poem",
                            "What is AI?", "Motivational quote"]):
        s.text((0, 22 + i * 10), ("> " if i == 0 else "  ") + it)
    s.save("04_ai_list.png", "AI-чат: список промптов")
    return s


def s_ai_thinking():
    s = Screen()
    s.text((0, 10), "Thinking..."); s.hline(12)
    s.text((0, 30), "Tell me a joke")
    s.text((0, 50), "waiting...")
    s.save("05_thinking.png", "AI думает (Ollama отвечает)")
    return s


def s_response():
    s = Screen()
    s.text((0, 10), "AI response"); s.hline(12)
    lines = ["Why did the scarecrow win",
             "an award? Because he was",
             "outstanding in his field!",
             "And here goes a second",
             "line of the same joke"]
    for i, ln in enumerate(lines):
        s.text((0, 22 + i * 10), ln)
    s.text((0, GH - 2), "[1/2]")
    s.save("06_response.png", "Ответ AI, страница 1 из 2")
    return s


def s_time():
    s = Screen()
    s.text((0, 10), "Time"); s.hline(12)
    s.text((0, 28), "18:34:07*")
    s.save("07_time.png", "Часы (синхронизированы по NTP)")
    return s


def s_notice():
    s = Screen()
    s.text((0, 10), "Note"); s.hline(12)
    for i, ln in enumerate(["Meeting at 15:00",
                            "Buy bread and tea",
                            "Call the collegue"]):
        s.text((0, 21 + i * 10), ln)
    s.save("08_notice.png", "Заметка на экране")
    return s


def s_map():
    s = Screen()
    s.text((0, 10), "Maps [1/2]"); s.hline(12)
    s.text((0, 22), "> home")
    s.text((0, 30), "  work")
    s.save("09_maps.png", "Загруженные карты")
    return s


def s_settings():
    s = Screen()
    s.text((0, 10), "Set [1/7]"); s.hline(12)
    for i, it in enumerate(["WiFi", "Order", "Help", "Bright 100%",
                            "Notif ON", "Sleep ON", "Sync ON"]):
        s.text((0, 22 + i * 8), ("> " if i == 0 else "  ") + it)
    s.save("10_settings.png", "Настройки (7 пунктов)")
    return s


def s_wifi():
    s = Screen()
    s.text((0, 10), "WiFi [5]"); s.hline(12)
    for i, n in enumerate(["Home_5G", "Mobile-Hotspot", "*OpenNet",
                           "Fios-3542", "TP-Link"]):
        s.text((0, 22 + i * 8), ("> " if i == 1 else "  ") + n + (" *" if n.startswith("*") else ""))
    s.save("11_wifi_list.png", "Сканирование сетей (* = открытая)")
    return s


def s_calc():
    s = Screen()
    s.text((0, 10), "Calc: 12+7_"); s.hline(20)
    s.text((0, 32), "[OP +]")
    s.text((0, 44), "Nxt:cyc  Snd:+")
    s.text((0, GH - 2), "HldNxt:=  HldSnd:ext")
    s.save("12_calc.png", "Калькулятор ввод знака")
    return s


def s_messages():
    s = Screen()
    s.text((0, 10), "Chat [3]"); s.hline(12)
    for i, m in enumerate(["[Y] Hello!",
                           "[W] How are you?",
                           "[Y] OK, soon"]):
        s.text((0, 22 + i * 10), m)
    s.save("13_chat.png", "Сообщения (Y=ты, W=веб-панель)")
    return s


def s_notes():
    s = Screen()
    s.text((0, 10), "Notes [3]"); s.hline(12)
    for i, n in enumerate(["Shopping list", "Project idea", "Contacts"]):
        s.text((0, 22 + i * 8), ("> " if i == 0 else "  ") + n)
    s.save("14_notes.png", "Заметки (история)")
    return s


def s_games():
    s = Screen()
    s.text((0, 10), "Games"); s.hline(12)
    for i, g in enumerate(["Pong", "Dodg", "Dino", "Scor"]):
        s.text((0, 22 + i * 10), ("> " if i == 2 else "  ") + g)
    s.save("15_games.png", "Подменю игр")
    return s


def s_pong():
    s = Screen()
    for y in range(2, 64, 4):
        s.px((63, y)); s.px((64, y))
    s.box((0, 24, 1, 37))      # левая ракетка
    s.box((126, 30, 127, 43))  # правая ракетка (AI)
    s.box((60, 32, 62, 34))    # мяч
    s.text((2, 2), "S:3")
    s.save("16_pong.png", "Игра Pong (1 игрок против AI)")
    return s


def s_dino():
    s = Screen()
    s.hline(52)                            # земля
    s.px((110, 12)); s.px((111, 11)); s.px((111, 12)); s.px((110, 13))  # солнце
    for x in range(20, 31): s.px((x, 45))  # тело динозавра
    for y in range(40, 52): s.px((30, y))  # туловище/нога
    for y in range(36, 52): s.px((80, y))  # кактус
    s.px((79, 40)); s.px((81, 40))
    s.text((2, 2), "HI 120")
    s.text((70, 2), "45")
    s.save("17_dino.png", "Игра Dino: динозавр + кактус")
    return s


def s_dodger():
    s = Screen()
    s.box((60, 56, 67, 61))   # игрок
    s.box((24, 20, 29, 25))   # препятствия
    s.box((80, 4, 85, 9))
    s.text((0, 8), "S:2")
    s.save("18_dodger.png", "Игра Dodger")
    return s


def s_sleep_prompt():
    s = Screen()
    s.text((0, 10), "Sleep?"); s.hline(12)
    s.text((0, 30), "Nxt:Y  Snd:N")
    s.save("19_sleep.png", "Подтверждение режима сна")
    return s


def s_web_ui():
    s = Screen()
    s.text((0, 10), "Web UI"); s.hline(12)
    s.text((0, 26), "ESP8266 AI Terminal")
    s.text((0, 40), "Status / Prompts / Notes")
    s.text((0, 52), "Messages / Notice / Logs")
    s.save("20_web_ui.png", "Веб-панель в браузере (схематично)")
    return s


ALL = [
    s_splash, s_menu, s_stats, s_ai_list, s_ai_thinking,
    s_response, s_time, s_notice, s_map, s_settings,
    s_wifi, s_calc, s_messages, s_notes, s_games,
    s_pong, s_dino, s_dodger, s_sleep_prompt, s_web_ui,
]


def main():
    print("Рисую симуляцию OLED 128x64...")
    for fn in ALL:
        fn()
    print(f"\nГотово: {len(ALL)} скриншотов в {OUT.resolve()}")


if __name__ == "__main__":
    main()