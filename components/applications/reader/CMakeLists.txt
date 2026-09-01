idf_component_register(
    SRCS "src/espaperplay_reader.c"
         "src/espaperplay_reader_txt.c"
         "src/espaperplay_reader_epub.c"
         "src/espaperplay_reader_pagen.c"
         "src/espaperplay_reader_history.c"
    INCLUDE_DIRS "include"
    REQUIRES storage input ui lvgl
    PRIV_REQUIRES heap freetype zlib
)
