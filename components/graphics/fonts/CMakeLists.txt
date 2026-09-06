# ESPaperPlay fonts component
#
# 只读字体资产：
#   - 构建期把 assets/fonts/*.ttf 打包成 SPIFFS 镜像（fonts 分区，8MB，spiffs 子类型）
#   - 生成 mmap_generate_fonts.h（文件索引 / 校验和），输出到构建目录
#   - 运行期 mmap_assets_new 映射分区 + esp_lv_fs_desc_init 注册 LVGL 盘符 'A:'

set(INCLUDE_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/include")
file(MAKE_DIRECTORY "${INCLUDE_GEN_DIR}")

idf_component_register(
    SRCS "src/espaperplay_fonts.c" "src/espaperplay_sd_fonts.c"
    INCLUDE_DIRS "include" "${INCLUDE_GEN_DIR}"
    REQUIRES esp_mmap_assets esp_lv_fs lvgl board storage
)

# 打包 assets/fonts 目录为 fonts 分区 SPIFFS 镜像，并生成资源索引头文件。
# FLASH_IN_PROJECT: idf.py flash 时随固件一起烧写 fonts 分区。
set(FONT_ASSETS_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../assets/fonts")
if(EXISTS "${FONT_ASSETS_DIR}")
    spiffs_create_partition_assets(
        fonts
        "${FONT_ASSETS_DIR}"
        FLASH_IN_PROJECT
        MMAP_FILE_SUPPORT_FORMAT ".ttf"
        IMPORT_INC_PATH "${INCLUDE_GEN_DIR}"
    )
    add_dependencies(${COMPONENT_LIB} assets_fonts_bin)
else()
    message(WARNING "fonts component: assets/fonts directory missing, skipping partition image build")
endif()

# ---- FreeType 链接钩子 ----
# 开启 CONFIG_LV_USE_FREETYPE 后，lvgl 组件自身会编译 src/libs/freetype/*.c
# （LVGL 只提供 FreeType 封装层），这些源文件需要 freetype 头文件（ft2build.h
# 及 internal 头）与错误字符串宏；espressif/freetype 组件不会自动链接进 lvgl，
# 这里显式接线（与官方 esp_lvgl_adapter 的做法一致）。
idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
if(TARGET ${lvgl_lib})
    target_link_libraries(${lvgl_lib} PUBLIC idf::espressif__freetype)
endif()
idf_build_set_property(COMPILE_DEFINITIONS "FT_ERR_PREFIX=FT_Err_" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "FT_CONFIG_OPTION_ERROR_STRINGS" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "FT2_BUILD_LIBRARY" APPEND)

# LV_FREETYPE_USE_LVGL_PORT 生效：FreeType 打开字体文件走 LVGL 文件系统（盘符 A:）。
# 机制：LVGL 的 lv_ftsystem.c 提供与 freetype 自带 ftsystem.c 同名的强符号
# FT_Stream_Open / FT_New_Memory / FT_Done_Memory（内部用 lv_fs_open / lv_malloc），
# 链接时覆盖 ANSI fopen 实现。若不清除 freetype 目标里的 ftsystem.c，链接器先拉入
# ANSI 版，LVGL 版成为死代码，运行时 fopen("A:xxx.ttf") 必然失败——这是
# LV_FREETYPE_USE_LVGL_PORT 生效的关键一步（与官方 esp_lvgl_adapter 处理一致）。
if(CONFIG_LV_FREETYPE_USE_LVGL_PORT)
    idf_component_get_property(lvgl_dir lvgl__lvgl COMPONENT_DIR)
    get_target_property(_ft_sources freetype SOURCES)
    list(FILTER _ft_sources EXCLUDE REGEX ".*/ftsystem\\.c$")
    set_target_properties(freetype PROPERTIES SOURCES "${_ft_sources}")
    target_sources(freetype PRIVATE "${lvgl_dir}/src/libs/freetype/lv_ftsystem.c")
    target_include_directories(freetype PRIVATE "${lvgl_dir}")
endif()
