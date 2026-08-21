# Подяки

[English](CREDITS.md) | Українська

Ця прошивка стоїть на плечах попередніх робіт:

- **[nminaylov/F1C100s_projects](https://github.com/nminaylov/F1C100s_projects)**
  (GPL-3.0) — bare-metal HAL, вендорований у `vendor/F1C100s_projects/`
  (драйвери clock/GPIO/INTC/timer/UART/TVD/SDC, стартовий код ARM926 і код
  роботи з кешами, каркас збірки, mksunxi).
- **[milosladni/jepoc](https://github.com/milosladni/jepoc)** (LGPL-2.1+,
  автор Manuel Braga) — регістровий proof-of-concept JPEG-кодування на
  Cedar VE, з якого портовані `src/ve.c` / `src/vejpeg.c`.
- **[FatFs від ChaN](http://elm-chan.org/fsw/ff/00index_e.html)** (ліцензія
  BSD-типу) — вендорована у `vendor/fatfs/`.
- **f1c200-video-board** (GPL-3.0, той самий автор) — проєкт-попередник з
  експериментами, на виміряних результатах якого побудований цей дизайн:
  дисципліна кільця захоплення TVD (3 буфери, крок площин 4 МБ,
  переармування по завершенню через рядки-вартові), підйом клоків/MMU і
  завантажувач U-Boot YMODEM.
- **[mirkerson/c600](https://github.com/mirkerson/c600)** Linux 3.10 BSP —
  референс послідовності підйому клоків/скидання Video Engine для suniv
  (родина F1C).
- **[uli/allwinner-bare-metal](https://github.com/uli/allwinner-bare-metal)**
  `h264avi.c` (MIT) та
  **[s60sc/ESP32-CAM_MJPEG2SD](https://github.com/s60sc/ESP32-CAM_MJPEG2SD)**
  `avi.cpp` — референсні реалізації, вивчені для AVI-мультиплексора.
- **[linux-sunxi.org](https://linux-sunxi.org)** — документація регістрів
  Video Engine і зусилля з реверс-інжинірингу CedarX.
- **[CherryUSB](https://github.com/cherry-embedded/CherryUSB)** v1.2.0
  (Apache-2.0) — USB-стек пристрою (ядро + клас MSC + порт MUSB),
  вендорований у `vendor/cherryusb/` з правками конфігурації під bare metal
  і доданим READ CAPACITY(16); зміни позначені в заголовках файлів.
- **[lhdjply/f1c200s_library](https://github.com/lhdjply/f1c200s_library)**
  (MIT) — рецепт підйому USB PHY/клоків і референс MSC-дескрипторів
  (`src/usbphy.c`, частини `src/usbmsc.c`), звірено з мейнлайновими Linux
  `musb_sunxi` і `phy-sun4i-usb`.

Сукупна робота ліцензована **GPL-3.0-or-later**; вендоровані дерева
зберігають свої файли ліцензій. Похідні файли коду несуть позначку
походження в заголовку.
