idf_component_register(
    SRCS "buzzer_test.cpp"         # list the source files of this component
    PRIV_INCLUDE_DIRS "."
    PRIV_REQUIRES unity buzzer esp_timer
    )