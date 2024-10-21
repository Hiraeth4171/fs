#include "./include/fs/fs.h"
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <error.h>

#include <sys/inotify.h>
#include <sys/stat.h>

#define bool _Bool 
#define false 1
#define true 0

#define WATCH_SIZE 16

int inotify_fd = -1;
FileHandler **watches = NULL; 
size_t watches_cap = WATCH_SIZE;

callback_t *callbacks = NULL;

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
}

void fs_terminate(bool watch) {
    if (watch) {
        if (inotify_fd < 0) return; // fs_init was never ran
        int _res = close(inotify_fd);
        if (_res < 0) {
            error(-1, errno, "ERROR: failed to terminate inotify");
        }
        free(watches);
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
        fh->fd = NULL;
    }
    else if (s.st_mode & S_IFREG) {
        FILE* fd = fopen(file_path, mode);
        if (fd == NULL) {
            error(0, errno, "ERROR: failed to create FileHandler");
            return NULL;
        }
        fh->fd = fd;
    }
    fh->file_path = file_path;
    return fh;
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
    if (fh->fd != NULL) fclose(fh->fd);
    if (_res == EOF) error(0, errno, "ERROR: failed to destroy FileHandler");
    if (fh->buff != NULL) free(fh->buff);
    if (inotify_fd != -1) close(inotify_fd);
}

int fs_start_watching() {
    char buff[4096] __attribute__ ((aligned(__alignof__(struct inotify_event))));
    struct inotify_event *event;
    ssize_t len;
    printf("started watching!\n");
    for (;;) {
        len = read(inotify_fd, buff, sizeof(buff));
        if (len == -1 && errno != EAGAIN) {
            error(EXIT_FAILURE, errno, "failed to read from inotify_fd");
        }
        if (len <= 0) break;
        for (char *ptr = buff; ptr < buff + len; ptr += sizeof(struct inotify_event) + event->len) {
            event = (struct inotify_event*) ptr;
            callbacks[event->wd-1](event, watches[event->wd-1]);
        }
    }
    return 0;
}
