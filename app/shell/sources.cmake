# Application component source selection.
if(CONFIG_SHELL)
    list(APPEND APP_SRCS shell/nshell.c shell/session.c shell/parse.c)
endif()

