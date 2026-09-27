#!/usr/bin/env python3
"""Exercise production cache/FS/page-table/cover readers under ASan + UBSan.

The host supplies RTOS no-ops (workers are verified on hardware) and injects
FAT root-mkdir, replacement-rename, and buffered-close failures.
"""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HARNESS = r'''
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "espaperplay_config.h"
#include "espaperplay_fs.h"
#include "espcache.h"
#include "espaperplay_reader_pagen.h"
#include "espaperplay_reader_cover.h"
#include "espaperplay_reader_epub.h"
#include "espaperplay_reader_history.c"
static int rename_failure, close_failure;
int __real_mkdir(const char *, mode_t);
int __wrap_mkdir(const char *p, mode_t m) {
    if (!strcmp(p, ESPAPERPLAY_SYSTEM_SD_DIR)) { errno = EINVAL; return -1; }
    return __real_mkdir(p, m);
}
int __real_rename(const char *, const char *);
int __wrap_rename(const char *a, const char *b) {
    if (rename_failure == 1) { errno = EIO; return -1; }
    if (rename_failure == 2 && access(b, F_OK) == 0) { errno = EEXIST; return -1; }
    return __real_rename(a,b);
}
int __real_fclose(FILE *);
int __wrap_fclose(FILE *f) {
    int r = __real_fclose(f);
    if (close_failure) { close_failure = 0; errno = ENOSPC; return EOF; }
    return r;
}
void *heap_caps_malloc(size_t n, unsigned c) { return malloc(n); }
void *heap_caps_calloc(size_t n, size_t z, unsigned c) { return calloc(n,z); }
void heap_caps_free(void *p) { free(p); }
bool espaperplay_storage_is_mounted(void) { return true; }
esp_err_t espaperplay_reader_epub_probe_cover(const char *p, int w, int h,
                                              lv_image_dsc_t *d, uint8_t **b) {
    abort(); /* Cache-only access must never decode an EPUB. */
}
static bool write_text(FILE *f, void *p) { return fputs(p, f) >= 0; }
static bool fail_write(FILE *f, void *p) { fputs("partial", f); return false; }
static void expect_text(const char *p, const char *text) {
    char b[32] = {0}; FILE *f = fopen(p, "rb"); assert(f);
    assert(fread(b,1,sizeof(b)-1,f) == strlen(text)); fclose(f); assert(!strcmp(b,text));
}
static void page_file(const char *p, uint32_t count, int payload) {
    uint32_t h[] = {0x43525046,3,0x1234,0x5678,0,count};
    FILE *f = fopen(p,"wb"); assert(f); assert(fwrite(h,4,6,f)==6);
    for(int i=0;i<payload;i++) fputc(0,f); fclose(f);
}
int main(void) {
    const char *root = ESPAPERPLAY_SYSTEM_SD_DIR;
    assert(__real_mkdir(root,0700)==0);
    char p[256]; snprintf(p,sizeof(p),"%s/a/b/cache",root);
    assert(espcache_write_atomic(p,write_text,"old")); expect_text(p,"old");
    rename_failure=2;
    assert(espcache_write_atomic(p,write_text,"new")); expect_text(p,"new");
    rename_failure=0;
    assert(!espcache_write_atomic(p,fail_write,NULL)); expect_text(p,"new");
    close_failure=1;
    assert(!espcache_write_atomic(p,write_text,"bad")); expect_text(p,"new");
    rename_failure=1;
    assert(!espcache_write_atomic(p,write_text,"bad")); expect_text(p,"new");
    rename_failure=0;
    char tmp[260]; snprintf(tmp,sizeof(tmp),"%s.tmp",p); assert(access(tmp,F_OK)!=0);
    assert(espaperplay_fs_mkdir_p(p)==ESP_FAIL); /* A file is not a directory. */
    puts("PASS: FAT root, replace, callback/close/rename failures, file collision");

    snprintf(p,sizeof(p),"%s/cache/reader",root); assert(espaperplay_fs_mkdir_p(p)==ESP_OK);
    snprintf(p,sizeof(p),"%s/cache/reader/00001234.ch000.f00005678.pag",root);
    uint32_t b[1]; uint16_t l[1];
    page_file(p,1,6);
    assert(espaperplay_pagen_load(0x1234,0,0x5678,NULL,NULL,0)==-1);
    assert(espaperplay_pagen_load(0x1234,0,0x5678,b,l,1)==1 && b[0]==0 && l[0]==0);
    page_file(p,3,18);
    assert(espaperplay_pagen_load(0x1234,0,0x5678,b,l,1)==-3 && access(p,F_OK)==0);
    page_file(p,3,6);
    assert(espaperplay_pagen_load(0x1234,0,0x5678,NULL,NULL,0)==0 && access(p,F_OK)!=0);
    page_file(p,UINT32_MAX,0);
    assert(espaperplay_pagen_load(0x1234,0,0x5678,NULL,NULL,0)==0);
    puts("PASS: single-page/count-only/capacity reads, truncated/overflow rejection");

    char book[256]; snprintf(book,sizeof(book),"%s/book.epub",root);
    FILE *f=fopen(book,"wb"); assert(f); fputs("fixture",f); fclose(f);
    struct stat st; assert(stat(book,&st)==0);
    snprintf(p,sizeof(p),"%s/cache/reader/covers",root); assert(espaperplay_fs_mkdir_p(p)==ESP_OK);
    snprintf(p,sizeof(p),"%s/cache/reader/covers/%08x.78x104.cov",root,espcache_file_token(book));
    uint32_t h[]={0x56435045,2,2,2,(uint32_t)st.st_size,(uint32_t)st.st_mtime};
    f=fopen(p,"wb"); assert(f); fwrite(h,4,6,f); fwrite("12345678",1,8,f); fclose(f);
    espaperplay_reader_cover_result_t cover;
    assert(espaperplay_reader_cover_cached(book,78,104,&cover));
    assert(cover.w==2 && cover.h==2 && !memcmp(cover.buf,"12345678",8)); free(cover.buf);
    assert(!espaperplay_reader_cover_cached(book,79,104,&cover));
    assert(truncate(p,25)==0);
    assert(!espaperplay_reader_cover_cached(book,78,104,&cover));
    h[2]=h[3]=0; f=fopen(p,"wb"); fwrite(h,4,6,f); fclose(f);
    assert(espaperplay_reader_cover_cached(book,78,104,&cover) && !cover.buf);
    f=fopen(book,"ab"); fputs("changed",f); fclose(f);
    assert(!espaperplay_reader_cover_cached(book,78,104,&cover));
    puts("PASS: first-frame cover, dimensions, truncation, negative cache, source changes");
    hist_apply_update(book,7,25);
    hist_apply_update(book,9,25);
    espaperplay_reader_history_entry_t entry;
    assert(espaperplay_reader_history_find(book,&entry)==ESP_OK && entry.page==9);
    assert(espaperplay_reader_history_find("missing",&entry)==ESP_ERR_NOT_FOUND);
    hist_apply_remove("missing");
    hist_apply_remove(book);
    assert(espaperplay_reader_history_find(book,&entry)==ESP_ERR_NOT_FOUND);
    puts("PASS: history update/lookup/remove ownership and persistence");
    return 0;
}
'''
RTOS = '''#pragma once
#include <stdint.h>
#include <stddef.h>
typedef void *SemaphoreHandle_t;
typedef void *TaskHandle_t;
typedef void *QueueHandle_t;
typedef uint32_t TickType_t;
#define pdMS_TO_TICKS(x) (x)
#define pdTRUE 1
#define ESP_ERR_TIMEOUT 0x107
static inline unsigned xTaskGetTickCount(void) { return 0; }
static inline void vTaskDelay(int t) {}
static inline void *xQueueCreate(int n,int z) { return (void *)1; }
static inline int xQueueReceive(void *q,void *p,int t) { return 0; }
static inline int xQueueSend(void *q,const void *p,int t) { return 0; }
#define portMAX_DELAY 0
#define pdPASS 1
static inline void *xSemaphoreCreateMutex(void) { return (void *)1; }
static inline void *xSemaphoreCreateCounting(int n,int v) { return (void *)1; }
static inline int xSemaphoreTake(void *p,int v) { return 1; }
static inline int xSemaphoreGive(void *p) { return 1; }
static inline int xTaskCreate(void (*f)(void *),const char *n,int z,void *a,int p,void **t) { return 0; }
static inline int xTaskCreateWithCaps(void (*f)(void *),const char *n,int z,void *a,int p,void **t,int c) { return 0; }
static inline unsigned uxTaskGetStackHighWaterMark(void *t) { return 0; }
'''
with tempfile.TemporaryDirectory(prefix="reader-cache-test-") as td:
    base = Path(td)
    (base / "freertos").mkdir()
    (base / "freertos/FreeRTOS.h").write_text(RTOS)
    for name in ("idf_additions.h", "semphr.h", "task.h", "queue.h"):
        (base / "freertos" / name).write_text('#include "FreeRTOS.h"\n')
    (base / "esp_log.h").write_text('#pragma once\n' + '\n'.join(
        f'#define ESP_LOG{x}(tag,...) ((void)(tag))' for x in "IWDE"))
    (base / "espaperplay_config.h").write_text(
        f'#define ESPAPERPLAY_SYSTEM_SD_DIR "{td}/sdcard"\n')
    (base / "test.c").write_text(HARNESS)
    storage = ROOT / "components/services/storage"
    reader = ROOT / "components/applications/reader"
    command = [os.environ.get("CC", "clang"), "-std=c11", "-D_GNU_SOURCE", "-g", "-O1",
               "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-Wall", "-Wextra",
               "-Werror", "-Wno-unused-parameter", f"-I{storage}/include", f"-I{base}",
               f"-I{ROOT}/tools/reader/host", f"-I{reader}/include", f"-I{reader}/src", str(base / "test.c"),
               str(storage / "src/espaperplay_fs.c"), str(storage / "src/espcache.c"),
               str(reader / "src/espaperplay_reader_pagen.c"),
               str(reader / "src/espaperplay_reader_cover.c"),
               "-Wl,--wrap=mkdir,--wrap=rename,--wrap=fclose", "-o", str(base / "test")]
    subprocess.run(command, check=True)
    subprocess.run([str(base / "test")], check=True)
