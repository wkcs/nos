/* Copyright (C) 2026 NOS contributors.
 * IDF owns the image and its shared data. There is no private user-data image.
 * DRAM/IRAM/DROM/PSRAM access is checked by the Xtensa shared-memory hook. */
PROVIDE(__user_text_start = _text_start);
PROVIDE(__user_text_end = _text_end);
PROVIDE(__user_text_load = _text_start);
PROVIDE(__user_data_start = 0);
PROVIDE(__user_data_end = 0);
PROVIDE(__user_data_load = 0);
PROVIDE(__user_bss_start = 0);
PROVIDE(__user_bss_end = 0);
