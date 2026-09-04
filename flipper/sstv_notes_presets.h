#pragma once
/* Ready-made ASCII notes to send, or to load into the keyboard and edit. All <= 200 chars. */
static const char* const SSTV_NOTE_PRESETS[] = {
    "Hello from Flipper!",
    "Meet me at the usual spot, 7pm.",
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ 0123456789 - text test pattern",
    "The quick brown fox jumps over the lazy dog. Pack my box with five dozen liquor jugs. "
    "Now is the time for all good folks to check word wrap.",
    "( o_o )  hi from a dolphin",
    "Battery low. Send help and snacks.",
    "SFTV works: Slow Flipper TV, over the air, one buzzer at a time.",
    "Привет! Это заметка на русском - проверка UTF-8.",
    "Флиппер передает текст звуком, Playdate его рисует.",
};
#define SSTV_NOTE_PRESET_COUNT ((int)(sizeof(SSTV_NOTE_PRESETS) / sizeof(SSTV_NOTE_PRESETS[0])))
