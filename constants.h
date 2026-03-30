#pragma once

#define CHANNEL_DIR "channels"

#define COLOURED_USER_NAME "\033[%dm[%s]\033[0m %s\n"
#define NUMBER_OF_TEXT_COLOURS 7

typedef struct text_color
{
    char name[16];
    int code;
} TextColor;

extern TextColor TEXT_COLOURS[];