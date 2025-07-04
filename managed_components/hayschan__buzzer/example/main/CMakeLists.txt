set(srcs
    buzzer_example.c
    ../../buzzer.c
)

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "../../include"
)
