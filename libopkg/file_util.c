/* file_util.c - convenience routines for common stat operations

   Carl D. Worth

   Copyright (C) 2001 University of Southern California

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2, or (at
   your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.
*/

#include "includes.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>

#include "sprintf_alloc.h"
#include "file_util.h"
#include "md5.h"
#include "libbb/libbb.h"
#undef strlen

#if defined HAVE_SHA256
#include "sha256.h"
#endif

int file_exists(const char *file_name)
{
    int err;
    struct stat stat_buf;

    err = stat(file_name, &stat_buf);
    if (err == 0) {
	return 1;
    } else {
	return 0;
    }
}

int file_is_dir(const char *file_name)
{
    int err;
    struct stat stat_buf;

    err = stat(file_name, &stat_buf);
    if (err) {
	return 0;
    }

    return S_ISDIR(stat_buf.st_mode);
}

/* read a single line from a file, stopping at a newline or EOF.
   If a newline is read, it will appear in the resulting string.
   Return value is a malloc'ed char * which should be freed at
   some point by the caller.

   Return value is NULL if the file is at EOF when called.
*/
#define FILE_READ_LINE_BUF_SIZE 1024
char *file_read_line_alloc(FILE *file)
{
    char buf[FILE_READ_LINE_BUF_SIZE];
    int buf_len;
    char *line = NULL;
    int line_size = 0;

    memset(buf, 0, FILE_READ_LINE_BUF_SIZE);
    while (fgets(buf, FILE_READ_LINE_BUF_SIZE, file)) {
	buf_len = strlen(buf);
	if (line) {
	    line_size += buf_len;
	    line = xrealloc(line, line_size);
	    strcat(line, buf);
	} else {
	    line_size = buf_len + 1;
	    line = xstrdup(buf);
	}
	if (buf[buf_len - 1] == '\n') {
	    break;
	}
    }

    return line;
}

int file_move(const char *src, const char *dest)
{
    int err;

    err = rename(src, dest);

    if (err && errno == EXDEV) {
	err = file_copy(src, dest);
	unlink(src);
    } else if (err) {
	fprintf(stderr, "%s: ERROR: failed to rename %s to %s: %s\n",
		__FUNCTION__, src, dest, strerror(errno));
    }

    return err;
}

/* I put these here to keep libbb dependencies from creeping all over
   the opkg code */
int file_copy(const char *src, const char *dest)
{
    int err;

    err = copy_file(src, dest, FILEUTILS_FORCE | FILEUTILS_PRESERVE_STATUS);
    if (err) {
	fprintf(stderr, "%s: ERROR: failed to copy %s to %s\n",
		__FUNCTION__, src, dest);
    }

    return err;
}

int file_mkdir_hier(const char *path, long mode)
{
    return make_directory(path, mode, FILEUTILS_RECUR);
}

char *file_md5sum_alloc(const char *file_name)
{
    static const int md5sum_bin_len = 16;
    static const int md5sum_hex_len = 32;

    static const unsigned char bin2hex[16] = {
	'0', '1', '2', '3',
	'4', '5', '6', '7',
	'8', '9', 'a', 'b',
	'c', 'd', 'e', 'f'
    };

    int i, err;
    FILE *file;
    char *md5sum_hex;
    unsigned char md5sum_bin[md5sum_bin_len];

    md5sum_hex = xcalloc(1, md5sum_hex_len + 1);

    file = fopen(file_name, "r");
    if (file == NULL) {
	fprintf(stderr, "%s: Failed to open file %s: %s\n",
		__FUNCTION__, file_name, strerror(errno));
	free(md5sum_hex);
	return NULL;
    }

    err = md5_stream(file, md5sum_bin);
    if (err) {
	fprintf(stderr, "%s: ERROR computing md5sum for %s: %s\n",
		__FUNCTION__, file_name, strerror(err));
	fclose(file);
	free(md5sum_hex);
	return NULL;
    }

    fclose(file);

    for (i=0; i < md5sum_bin_len; i++) {
	md5sum_hex[i*2] = bin2hex[md5sum_bin[i] >> 4];
	md5sum_hex[i*2+1] = bin2hex[md5sum_bin[i] & 0xf];
    }
    
    md5sum_hex[md5sum_hex_len] = '\0';
    
    return md5sum_hex;
}

#ifdef HAVE_SHA256
char *file_sha256sum_alloc(const char *file_name)
{
    static const int sha256sum_bin_len = 32;
    static const int sha256sum_hex_len = 64;

    static const unsigned char bin2hex[16] = {
	'0', '1', '2', '3',
	'4', '5', '6', '7',
	'8', '9', 'a', 'b',
	'c', 'd', 'e', 'f'
    };

    int i, err;
    FILE *file;
    char *sha256sum_hex;
    unsigned char sha256sum_bin[sha256sum_bin_len];

    sha256sum_hex = xcalloc(1, sha256sum_hex_len + 1);

    file = fopen(file_name, "r");
    if (file == NULL) {
	fprintf(stderr, "%s: Failed to open file %s: %s\n",
		__FUNCTION__, file_name, strerror(errno));
	free(sha256sum_hex);
	return NULL;
    }

    err = sha256_stream(file, sha256sum_bin);
    if (err) {
	fprintf(stderr, "%s: ERROR computing sha256sum for %s: %s\n",
		__FUNCTION__, file_name, strerror(err));
	fclose(file);
	free(sha256sum_hex);
	return NULL;
    }

    fclose(file);

    for (i=0; i < sha256sum_bin_len; i++) {
	sha256sum_hex[i*2] = bin2hex[sha256sum_bin[i] >> 4];
	sha256sum_hex[i*2+1] = bin2hex[sha256sum_bin[i] & 0xf];
    }
    
    sha256sum_hex[sha256sum_hex_len] = '\0';
    
    return sha256sum_hex;
}

#endif


int
rm_r(const char *path)
{
	int ret = 0;
	DIR *dir;
	struct dirent *dent;

	dir = opendir(path);
	if (dir == NULL) {
		perror_msg("%s: opendir(%s)", __FUNCTION__, path);
		return -1;
	}

	if (fchdir(dirfd(dir)) == -1) {
		perror_msg("%s: fchdir(%s)", __FUNCTION__, path);
		closedir(dir);
		return -1;
	}

	while (1) {
		errno = 0;
		if ((dent = readdir(dir)) == NULL) {
			if (errno) {
				perror_msg("%s: readdir(%s)",
						__FUNCTION__, path);
				ret = -1;
			}
			break;
		}

		if (!strcmp(dent->d_name, ".") || !strcmp(dent->d_name, ".."))
			continue;

#ifdef _BSD_SOURCE
		if (dent->d_type == DT_DIR) {
			if ((ret = rm_r(dent->d_name)) == -1)
				break;
			continue;
		} else if (dent->d_type == DT_UNKNOWN)
#endif
		{
			struct stat st;
			if ((ret = lstat(dent->d_name, &st)) == -1) {
				perror_msg("%s: lstat(%s)",
						__FUNCTION__, dent->d_name);
				break;
			}
			if (S_ISDIR(st.st_mode)) {
				if ((ret = rm_r(dent->d_name)) == -1)
					break;
				continue;
			}
		}

		if ((ret = unlink(dent->d_name)) == -1) {
			perror_msg("%s: unlink(%s)",
					__FUNCTION__, dent->d_name);
			break;
		}
	}

	if (chdir("..") == -1) {
		ret = -1;
		perror_msg("%s: chdir(%s/..)", __FUNCTION__, path);
	}

	if (rmdir(path) == -1 ) {
		ret = -1;
		perror_msg("%s: rmdir(%s)", __FUNCTION__, path);
	}

	if (closedir(dir) == -1) {
		ret = -1;
		perror_msg("%s: closedir(%s)", __FUNCTION__, path);
	}

	return ret;
}
