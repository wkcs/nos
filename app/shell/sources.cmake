# Application component source selection.
if(CONFIG_SHELL)
    list(APPEND APP_SRCS shell/nshell.c shell/session.c shell/parse.c shell/glob.c shell/test_expr.c shell/printf_cmd.c shell/pipe.c)
elseif(CONFIG_SHELL_PIPE_TEST)
    list(APPEND APP_SRCS shell/pipe.c)
endif()

if(CONFIG_SHELL_PIPE_TEST)
    list(APPEND APP_SRCS shell/pipe_test.c)
endif()

