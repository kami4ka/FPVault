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
  (Apache-2.0) — USB-стек пристрою (ядро + класи MSC і video + порт MUSB),
  вендорований у `vendor/cherryusb/` з правками конфігурації під bare metal
  і доданим READ CAPACITY(16); зміни позначені в заголовках файлів.
- **[lhdjply/f1c200s_library](https://github.com/lhdjply/f1c200s_library)**
  (MIT) — рецепт підйому USB PHY/клоків і референс MSC-дескрипторів
  (`src/usbphy.c`, частини `src/usbmsc.c`), звірено з мейнлайновими Linux
  `musb_sunxi` і `phy-sun4i-usb`.

## Застосунок для комп'ютера (`desktop/`)

Компаньйон FPVault Desktop містить вкладені сторонні виконувані файли. У
репозиторій вони не комітяться: `desktop/scripts/fetch-binaries.mjs`
завантажує їх під час збірки й звіряє з sha256, зафіксованими в
`desktop/resources/binaries.lock.json`.

- **[FFmpeg](https://ffmpeg.org/)** 6.1.1 (GPL-2.0-or-later, зібраний із
  `--enable-gpl` та libx264) — вкладений статичний бінарник для
  необов'язкового експорту в H.264/MP4. Відповідні вихідні коди:
  https://ffmpeg.org/releases/ffmpeg-6.1.1.tar.xz. Самі бінарники — з
  [eugeneware/ffmpeg-static](https://github.com/eugeneware/ffmpeg-static).
  Більше ніщо в застосунку його не використовує: імпорт, ремонт, склеювання
  й відтворення — чистий TypeScript.
- **[x264](https://www.videolan.org/developers/x264.html)** (GPL-2.0-or-later)
  — усередині тієї збірки FFmpeg; саме він робить можливим експорт у MP4.
- **[sunxi-tools](https://github.com/linux-sunxi/sunxi-tools)** (GPL-2.0-or-later)
  — `sunxi-fel` вкладено для відновлення через FEL; збирається із
  зафіксованого коміту скриптом `desktop/scripts/build-sunxi-fel.sh` і
  лінкується статично з libusb (LGPL-2.1-or-later), libfdt
  (BSD-2-Clause/GPL-2.0) та zlib. Готових збірок немає для жодної платформи,
  на яку йде застосунок, — тому він збирається, а не завантажується.
- **[Electron](https://electronjs.org/)** (MIT), **Chromium** (BSD-3-Clause),
  **[Node.js](https://nodejs.org/)** (MIT), **[React](https://react.dev/)**
  (MIT), **[Vite](https://vite.dev/)** (MIT),
  **[Tailwind CSS](https://tailwindcss.com/)** (MIT) — середовище виконання
  та інструменти збірки.
- **[react-markdown](https://github.com/remarkjs/react-markdown)** (MIT) та
  **[remark-gfm](https://github.com/remarkjs/remark-gfm)** (MIT) — показують
  нотатки релізів GitHub на екрані прошивки. Сирий HTML не відтворюється —
  це типова поведінка react-markdown і причина, чому взято саме його, а не
  парсер плюс санітайзер.

Читач AVI у `desktop/src/shared/avi/` — це порт на TypeScript власного
`tools/checkavi.py` із цього ж репозиторію, і набір тестів стежить, щоб обидві
реалізації давали однакові діагностики.

Сукупна робота ліцензована **GPL-3.0-or-later**; вендоровані дерева
зберігають свої файли ліцензій. Похідні файли коду несуть позначку
походження в заголовку.
