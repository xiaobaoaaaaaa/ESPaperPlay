idf_component_register(
    SRCS "src/espaperplay_gui.c"
    INCLUDE_DIRS "include"
    REQUIRES board epd touch
    # NOTE: lvgl will be added to REQUIRES when the GUI backend is ported.
)
