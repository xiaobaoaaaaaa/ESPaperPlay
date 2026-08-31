idf_component_register(
    SRCS "src/espaperplay_reader.c"
         "src/espaperplay_reader_txt.c"
         "src/espaperplay_reader_history.c"
    INCLUDE_DIRS "include"
    REQUIRES storage input ui
    PRIV_REQUIRES heap freertos
)
