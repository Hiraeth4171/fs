#ifndef FS_H
#define FS_H
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/inotify.h>
#include <dirent.h>

#define WD_LIMIT_PER_FILE 5

/*
 *  FileHandler [ fd, buffer, size ]
 *  - need a way to bind events to file handlers [x]
 *      - inotify [x]
 *      - windows [ ]
 *  - read files, write to files, access files, file permissions, etc etc [~]
 *  - mimetypes and other file stats [~]
 * */

typedef struct FileHandler FileHandler;
struct FileHandler {
    FILE* fd;
    const char* file_path;
    short i;
    int wd[WD_LIMIT_PER_FILE];
    size_t size;
    char* buff;
    DIR* directory_stream;
};

typedef struct ByteReader ByteReader;
struct ByteReader {
    FileHandler* fh;
    char* _start;
    char* _end;
    size_t _offset;
};

typedef void (*callback_t)(struct inotify_event*, FileHandler*);

void fs_set_custom_magic(char* path);
void fs_init(_Bool watch);
FileHandler* fs_create_filehandler(char* file_path, char* mode);
void fs_destroy_filehandler(FileHandler* fh);
int fs_watch_filehandler(FileHandler* fh, uint32_t mask, callback_t callback);
void fs_terminate(_Bool watch);
void fs_terminate_magic(void);
int fs_start_watching(void); // spawns a thread for the event loop
int fs_stop_watching(void); // spawns a thread for the event loop

ByteReader* fs_create_byte_reader(const char* file_path);
void* fs_br_read(ByteReader* byte_reader, size_t n);
void fs_br_seek(ByteReader* byte_reader, size_t offset);
void fs_destroy_byte_reader(ByteReader* byte_reader);

const char* fs_get_mimetype_raw(const char* path);
const char* fs_get_mimetype(FileHandler* fh);

void fs_read_filehandler(FileHandler* fh);
void fs_delete_file(FileHandler* fh);
char* fs_stream_from_dir(FileHandler* fh);
size_t fs_dir_len(FileHandler* fh);
void fs_memory_map_filehandler(FileHandler* fh);

void clean_test(void);

#endif 
