idf_component_register(
    SRCS "src/espaperplay_reader.c"
         "src/espaperplay_reader_txt.c"
         "src/espaperplay_reader_epub.c"
         "src/espaperplay_reader_cover.c"
         "src/espaperplay_reader_pagen.c"
         "src/espaperplay_reader_history.c"
         "src/libs/tjpgd/tjpgd.c"
    INCLUDE_DIRS "include"
    # PRIV 根目录：供 "src/libs/tjpgd/tjpgd.h" 形式的 include 解析
    PRIV_INCLUDE_DIRS "."
    REQUIRES storage input ui lvgl
    PRIV_REQUIRES heap freetype zlib
)
