#include "./include/fs/fs.h"
#include <dirent.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <error.h>

#include <sys/inotify.h>
#include <sys/stat.h>
#include <libgen.h>

#include <pthread.h>
#include <magic.h>
#include <string.h>

#define bool _Bool 

#define WATCH_SIZE 16

#define mem_read(dst, src, len)\
    memcpy((dst), (src), (len));

int inotify_fd = -1;
FileHandler **watches = NULL; 
size_t watches_cap = WATCH_SIZE;

callback_t *callbacks = NULL;

pthread_t watch_thread_id;
magic_t magic_db;

char* mimetype_database = NULL;

// void verify_magic_loading() {
//     struct stat st;
//     if (stat(mimetype_database, &st) != 0) {
//         perror("Database file missing");
//         exit(EXIT_FAILURE);
//     }
//     printf("Database size: %ld bytes\n", st.st_size);
//
//     // 2. Verify magic file signature
//     FILE *f = fopen(mimetype_database, "rb");
//     if (f) {
//         char header[8];
//         fread(header, 1, 8, f);
//         fclose(f);
//         printf("Magic header: %02X %02X %02X %02X %02X %02X %02X %02X\n",
//                header[0], header[1], header[2], header[3],
//                header[4], header[5], header[6], header[7]);
//     }
//
//     // 3. Force simple match test
//     const char *teststr = "<html>";
//     const char *result = magic_buffer(magic_db, teststr, strlen(teststr));
//     printf("Test result: %s\n", result ? result : "NULL");
//     printf("Last error: %s\n", magic_error(magic_db));
// }

void fs_init(bool watch) {
    if (watch) {
        inotify_fd = inotify_init();
        if (inotify_fd < 0) {
            error(-1, errno, "ERROR: failed to initialize inotify");
        }
        watches = malloc(WATCH_SIZE * sizeof(FileHandler*));
        callbacks = malloc(WATCH_SIZE * sizeof(callback_t));
        if (watches == NULL) error(-3, errno, "ERROR: failed to initialize inotify");
    }
    magic_db = magic_open(MAGIC_MIME_TYPE|MAGIC_ERROR);
    if (!magic_db) {
        fprintf(stderr, "Failed to initialize magic: %s\n", magic_error(magic_db));
        return;
    }
    // if (mimetype_database != NULL) printf("Using database: %s\n", mimetype_database);
    if (magic_load(magic_db, mimetype_database) != 0) {
        fprintf(stderr, "Failed to load database: %s\n", magic_error(magic_db));
        magic_close(magic_db);
        magic_db = NULL;
        return;
    }
}

void fs_terminate_magic() {
    magic_close(magic_db);
}

void fs_set_custom_magic(char* path) {
    char *resolved = realpath(path, NULL); // Use NULL to allocate
    if (!resolved) {
        perror("realpath failed");
        return;
    }
    mimetype_database = resolved;
}

void fs_terminate(bool watch) {
    if (watch) {
        if (inotify_fd < 0) return; // fs_init was never ran
        int _res = close(inotify_fd);
        if (_res < 0) {
            error(-1, errno, "ERROR: failed to terminate inotify");
        }
        free(watches);
        fs_terminate_magic();
    }
}

FileHandler* fs_create_filehandler(char* file_path, char* mode) {
    FileHandler* fh = malloc(sizeof(FileHandler));
    int _res = -1;
    if (mode[0] == 'r') _res = access(file_path, R_OK);
    else if (mode[0] == 'w') _res = access(file_path, W_OK);
    else _res = access(file_path, F_OK);
    if (_res == -1) {
        error(0, errno, "ERROR: failed to create FileHandler");
        return NULL;
    }
    fh->size = 0;
    fh->buff = NULL;
    fh->i = 0;
    struct stat s;
    if (stat(file_path, &s) != 0) {
        error(0, errno, "ERROR: failed to create FileHandler");
        return NULL;
    }
    if (s.st_mode & S_IFDIR) {
        DIR* _dir_stream = opendir(file_path);
        if (_dir_stream == NULL) {
            error(0, errno, "ERROR: failed to create FileHandler");
            return NULL;
        }
        fh->directory_stream = _dir_stream;
        fh->fd = NULL;
    }
    else if (s.st_mode & S_IFREG) {
        FILE* fd = fopen(file_path, mode);
        if (fd == NULL) {
            error(0, errno, "ERROR: failed to create FileHandler");
            return NULL;
        }
        fh->fd = fd;
        fh->directory_stream = NULL;
    }
    fh->file_path = file_path;
    fh->file_name = basename(strdup(file_path));
    return fh;
}

ByteReader* fs_create_byte_reader(const char* file_path) {
    FileHandler* fh = fs_create_filehandler((char*)file_path, "rb");
    if (fh == NULL) error(-3, 0, "ERROR: byte reader you were trying to read is NULL");
    if (fh->fd == NULL) error(-3, 0, "ERROR: byte reader given for read handles a directory");
    fs_read_filehandler(fh);
    ByteReader* _res = malloc(sizeof(ByteReader));
    _res->fh = fh;
    _res->_start = fh->buff;
    _res->_end = fh->buff+fh->size;
    _res->_offset = 0;
    return _res;
}

void* fs_br_read(ByteReader* byte_reader, size_t n) {
    if (byte_reader->_offset+n > byte_reader->fh->size) {
        n = byte_reader->fh->size - byte_reader->_offset;
    } // trim end if overflow'd
    char* _res = malloc(n);
    mem_read(_res, byte_reader->_start+byte_reader->_offset, n);
    byte_reader->_offset+=n;
    return (void*)_res;
}
void fs_br_seek(ByteReader* byte_reader, size_t offset) {
    if (offset > byte_reader->fh->size) {
        byte_reader->_offset = byte_reader->fh->size;
    } else {
        byte_reader->_offset = offset;
    }
}

int fs_watch_filehandler(FileHandler* fh, uint32_t mask, callback_t callback) {
    int wd = inotify_add_watch(inotify_fd, fh->file_path, mask);
    if (wd < 0) {
        error(-1, errno, "ERROR: failed to add watch");
    }
    if (wd > watches_cap) {
        FileHandler** tmp1 = realloc(watches, (watches_cap += 8) * sizeof(FileHandler*));
        if (tmp1 == NULL) error(-3, errno, "ERROR: failed to add watch");
        callback_t* tmp2 = realloc(callbacks, watches_cap * sizeof(callback_t));
        if (tmp2 == NULL) error(-3, errno, "ERROR: failed to add watch");
    }
    watches[wd-1] = fh;
    callbacks[wd-1] = callback;
    fh->wd[fh->i++] = wd;
    return wd;
}

void fs_destroy_filehandler(FileHandler *fh) {
    int _res = 1;
    if (fh == NULL) return;
    if (fh->fd != NULL) fclose(fh->fd);
    if (_res == EOF) error(0, errno, "ERROR: failed to destroy FileHandler");
    if (fh->buff != NULL) free(fh->buff);
    free(fh);
}

void fs_destroy_byte_reader(ByteReader* byte_reader) {
    fs_destroy_filehandler(byte_reader->fh);
    free(byte_reader);
}

void *fs_callback_event(void *_event) {
    pthread_detach(pthread_self());
    struct inotify_event *event = (struct inotify_event *) _event;
    callbacks[event->wd-1](event, watches[event->wd-1]);
    pthread_exit(NULL);
}

void *fs_watch_thread_func(void *arg) {

    char buff[4096] __attribute__ ((aligned(__alignof__(struct inotify_event))));
    struct inotify_event *event;
    ssize_t len;
    for (;;) {
        len = read(inotify_fd, buff, sizeof(buff));
        if (len == -1 && errno != EAGAIN) {
            error(EXIT_FAILURE, errno, "failed to read from inotify_fd");
        }
        if (len <= 0) break;
        for (char *ptr = buff; ptr < buff + len; ptr += sizeof(struct inotify_event) + event->len) {
            event = (struct inotify_event*) ptr;
            pthread_t pstid;
            pthread_create(&pstid, NULL, fs_callback_event, (void*)event);
        }
    }
    pthread_exit(NULL);
}

int fs_start_watching() {
    pthread_create(&watch_thread_id, NULL, fs_watch_thread_func, NULL);
    return 0;
}

int fs_stop_watching(void) {
    pthread_cancel(watch_thread_id);
    pthread_join(watch_thread_id, NULL);
    return 0;
}

char* fs_get_file_extension(const char* file_path) {
    char* _res = malloc(10);
    if (_res == NULL) error(-2, errno, "failed to get file extension");
    const char* ptr = file_path;
    for(; *ptr != '.' && *ptr != '\0'; ptr++) {

    }
    if (*ptr == '.') {
        size_t temp = (file_path + strlen(file_path)) - ptr;
            strncpy(_res, ptr+1, temp);
            return _res;
    }
    else return NULL;
}

const char* fs_get_mimetype_raw(const char* path) {
    const char* out = magic_file(magic_db, path);
    return out ? strdup(out) : NULL;
}

const char* fs_get_mimetype(FileHandler* fh) {
    if (fh == NULL) return "No FileHandler";
    if (fh->file_path == NULL) return "No file path";
    return fs_get_mimetype_raw(fh->file_path);
}


void fs_read_filehandler(FileHandler* fh) {
    if (fh == NULL) error(-3, 0, "ERROR: filehandler you were trying to read is NULL");
    if (fh->fd == NULL) error(-3, 0, "ERROR: filehandler given for read handles a directory");
    fseek(fh->fd, 0L, SEEK_END);
    long pos = ftell(fh->fd);
    fh->size = (size_t) pos;
    rewind(fh->fd);
    if (fh->buff != NULL) free(fh->buff);
    fh->buff = malloc(pos+1);
    fread(fh->buff, 1, pos, fh->fd);
    fh->buff[pos] = '\0';
}

void fs_delete_file(FileHandler* fh) {
    if (fh == NULL) error(-3, 0, "ERROR: filehandler you were trying to delete is NULL");
    if (fh->fd == NULL) error(-3, 0, "ERROR: filehandler given for delete handles a directory");
    const char* tmp = fh->file_path;
    fs_destroy_filehandler(fh);
    remove(tmp);
}

/**
 * this doesn't support subdirectories
 */
char* fs_stream_from_dir(FileHandler* fh) {
    if (fh == NULL) error(-3, 0, "ERROR: filehandler you were trying to read is NULL");
    if (fh->directory_stream == NULL) error(-3, 0, "ERROR: filehandler directory you were trying to stream is NULL");
    errno = 0;
    struct dirent* _dir = readdir(fh->directory_stream);
    if (_dir == NULL) {
        if (errno != 0) error(0, errno, "ERROR: directory read failed: %d", errno);
        return NULL;
    }
    if (_dir->d_type == DT_DIR) {
        
    }
    return _dir->d_name;
}

void fs_dir_apply_callback(FileHandler* fh, void (*func)(struct dirent*, void*, void*, char* prefix),
        void* ctx1, void* ctx2, char* prefix) {
    if (fh == NULL) error(-3, 0, "ERROR: filehandler you were trying to read is NULL");
    if (fh->directory_stream == NULL) error(-3, 0, "ERROR: filehandler directory you were trying to stream is NULL");
    errno = 0;
    struct dirent* _dir = readdir(fh->directory_stream);
    while (_dir != NULL) {
        if (_dir->d_name[0] == '.') goto progress; // ignore dot-first files
        if (_dir->d_type == DT_DIR) {
            char final[256];
            realpath(fh->file_path, final);
            strcat(final, "/");
            strcat(final,_dir->d_name);
            final[strlen(final)] = '\0';
            FileHandler* subdir = fs_create_filehandler(final, "r");
            int prefix_len = 0;
            char* temp;
            if (prefix != NULL) {
                prefix_len = strlen(prefix);
                temp = strdup(prefix);
            }
            prefix = realloc(prefix, prefix_len + strlen(subdir->file_name)+2);
            if (prefix[prefix_len] == '/') strcat(prefix, "/");
            strcat(prefix, subdir->file_name);
            fs_dir_apply_callback(subdir, func, ctx1, ctx2, prefix);
            prefix = temp;
        }
        else {
            func(_dir, ctx1, ctx2, prefix);
        }
progress:
        _dir = readdir(fh->directory_stream);
    }
    if (_dir == NULL) {
        if (errno != 0) error(0, errno, "ERROR: directory read failed: %d", errno);
    }
}

size_t fs_dir_len (FileHandler* fh) {
    if (fh == NULL) error(-3, 0, "ERROR: filehandler you were trying to read is NULL");
    if (fh->directory_stream == NULL) error(-3, 0, "ERROR: filehandler directory you were trying to stream is NULL");
    errno = 0;
    size_t len = 0;
    struct dirent *entry;
    DIR* dir = fh->directory_stream;
    rewinddir(dir); // Start from the beginning
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        len++;
    }
    rewinddir(dir); // Reset to the beginning when done
    return len;
} // a faster approach for linux only is possible, implement it later

void fs_memory_map_filehandler(FileHandler* fh) {
    // add code NOW
    return;
}
