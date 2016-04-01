/*
  FUSE: Filesystem in Userspace
  Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>

  This program can be distributed under the terms of the GNU GPL.
  See the file COPYING.

  gcc -Wall `pkg-config fuse --cflags --libs` fusexmp.c -o fusexmp
*/

#define FUSE_USE_VERSION 26

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifdef linux
/* For pread()/pwrite() */
#define _XOPEN_SOURCE 500
#endif

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <sys/time.h>
#ifdef HAVE_SETXATTR
#include <sys/xattr.h>
#endif
#include <sys/wait.h>

const static char* mirrordir = "/home/adam/docs/hmc/files/ElephantSkin/backenddir";

static void copyFile(const char* from, const char* to)
{
  pid_t cpID = fork();
  if (cpID == 0) {
    // Child process, do the copy
    execl( "/bin/cp", "/bin/cp", "-p", from, to, (char*)NULL);
  } else {
    // Parent process, wait for it
    wait(NULL);
  }
}

static int xmp_getattr(const char *path, struct stat *stbuf)
{
  int res;

  char appendedPath[256];
  strcpy(appendedPath, mirrordir);
  strcat(appendedPath, path);
  res = lstat(appendedPath, stbuf);
  if (res == -1)
    return -errno;

  return 0;
}

static int xmp_access(const char *path, int mask)
{
  int res;

  char appendedPath[256];
  strcpy(appendedPath, mirrordir);
  strcat(appendedPath, path);
  res = access(appendedPath, mask);
  if (res == -1)
    return -errno;

  return 0;
}

static int xmp_readlink(const char *path, char *buf, size_t size)
{
  int res;

  char appendedPath[256];
  strcpy(appendedPath, mirrordir);
  strcat(appendedPath, path);
  res = readlink(appendedPath, buf, size - 1);
  if (res == -1)
    return -errno;

  buf[res] = '\0';
  return 0;
}


static int xmp_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                       off_t offset, struct fuse_file_info *fi)
{
  DIR *dp;
  struct dirent *de;

  (void) offset;
  (void) fi;

  char appendedPath[256];
  strcpy(appendedPath, mirrordir);
  strcat(appendedPath, path);
  dp = opendir(appendedPath);
  if (dp == NULL)
    return -errno;

  while ((de = readdir(dp)) != NULL) {
    struct stat st;
    memset(&st, 0, sizeof(st));
    st.st_ino = de->d_ino;
    st.st_mode = de->d_type << 12;
    if (filler(buf, de->d_name, &st, 0))
      break;
  }

  closedir(dp);
  return 0;
}

static int xmp_mknod(const char *path, mode_t mode, dev_t rdev)
{
  int res;
  char appendedPath[256];
  strcpy(appendedPath, mirrordir);
  strcat(appendedPath, path);

  /* On Linux this could just be 'mknod(path, mode, rdev)' but this
     is more portable */
  if (S_ISREG(mode)) {
    res = open(appendedPath, O_CREAT | O_EXCL | O_WRONLY, mode);
    if (res >= 0)
      res = close(res);
  } else if (S_ISFIFO(mode))
    res = mkfifo(appendedPath, mode);
  else
    res = mknod(appendedPath, mode, rdev);
  if (res == -1)
    return -errno;

  return 0;
}

static int xmp_mkdir(const char *path, mode_t mode)
{
  int res;
  char appendedPath[256];
  strcpy(appendedPath, mirrordir);
  strcat(appendedPath, path);

  res = mkdir(appendedPath, mode);
  if (res == -1)
    return -errno;

  return 0;
}

static int xmp_unlink(const char *path)
{
  int res;

  char appendedPath[256];
  strcpy(appendedPath, mirrordir);
  strcat(appendedPath, path);
  res = unlink(appendedPath);
  if (res == -1)
    return -errno;

  return 0;
}

static int xmp_rmdir(const char *path)
{
  int res;

  char appendedPath[256];
  strcpy(appendedPath, mirrordir);
  strcat(appendedPath, path);
  res = rmdir(appendedPath);
  if (res == -1)
    return -errno;

  return 0;
}

static int xmp_symlink(const char *to, const char *from)
{
  int res;

  char appendedTo[256];
  strcpy(appendedTo, mirrordir);
  strcat(appendedTo, to);
  char appendedFrom[256];
  strcpy(appendedFrom, mirrordir);
  strcat(appendedFrom, from);
  res = symlink(appendedTo, appendedFrom);
  if (res == -1)
    return -errno;

  return 0;
}

static int xmp_rename(const char *from, const char *to)
{
  int res;

  char appendedTo[256];
  strcpy(appendedTo, mirrordir);
  strcat(appendedTo, to);
  char appendedFrom[256];
  strcpy(appendedFrom, mirrordir);
  strcat(appendedFrom, from);
  res = rename(appendedFrom, appendedTo);
  if (res == -1)
    return -errno;

  return 0;
}

static int xmp_link(const char *from, const char *to)
{
  int res;

  char appendedTo[256];
  strcpy(appendedTo, mirrordir);
  strcat(appendedTo, to);
  char appendedFrom[256];
  strcpy(appendedFrom, mirrordir);
  strcat(appendedFrom, from);
  res = link(appendedFrom, appendedTo);
  if (res == -1)
    return -errno;

  return 0;
}

static int xmp_chmod(const char *path, mode_t mode)
{
  int res;

  char appendedPath[256];
  strcpy(appendedPath, mirrordir);
  strcat(appendedPath, path);
  res = chmod(appendedPath, mode);
  if (res == -1)
    return -errno;

  return 0;
}

static int xmp_chown(const char *path, uid_t uid, gid_t gid)
{
  int res;

  char appendedPath[256];
  strcpy(appendedPath, mirrordir);
  strcat(appendedPath, path);
  res = lchown(appendedPath, uid, gid);
  if (res == -1)
    return -errno;

  return 0;
}

static int xmp_truncate(const char *path, off_t size)
{
  int res;

  char appendedPath[256];
  strcpy(appendedPath, mirrordir);
  strcat(appendedPath, path);

  // Make a copy of the file first!
  copyFile(appendedPath, "/home/adam/lastTruncatedFile");

  res = truncate(appendedPath, size);
  if (res == -1)
    return -errno;

  return 0;
}

static int xmp_utimens(const char *path, const struct timespec ts[2])
{
  int res;
  struct timeval tv[2];

  tv[0].tv_sec = ts[0].tv_sec;
  tv[0].tv_usec = ts[0].tv_nsec / 1000;
  tv[1].tv_sec = ts[1].tv_sec;
  tv[1].tv_usec = ts[1].tv_nsec / 1000;

  char appendedPath[256];
  strcpy(appendedPath, mirrordir);
  strcat(appendedPath, path);
  res = utimes(appendedPath, tv);
  if (res == -1)
    return -errno;

  return 0;
}

static int xmp_open(const char *path, struct fuse_file_info *fi)
{
  int res;

  char appendedPath[256];
  strcpy(appendedPath, mirrordir);
  strcat(appendedPath, path);
  res = open(appendedPath, fi->flags);
  if (res == -1)
    return -errno;

  close(res);
  return 0;
}

static int xmp_read(const char *path, char *buf, size_t size, off_t offset,
                    struct fuse_file_info *fi)
{
  int fd;
  int res;

  char appendedPath[256];
  strcpy(appendedPath, mirrordir);
  strcat(appendedPath, path);
  (void) fi;
  fd = open(appendedPath, O_RDONLY);
  if (fd == -1)
    return -errno;

  res = pread(fd, buf, size, offset);
  if (res == -1)
    res = -errno;

  close(fd);
  return res;
}

static int xmp_write(const char *path, const char *buf, size_t size,
                     off_t offset, struct fuse_file_info *fi)
{
  int fd;
  int res;

  (void) fi;
  char appendedPath[256];
  strcpy(appendedPath, mirrordir);
  strcat(appendedPath, path);
  fd = open(appendedPath, O_WRONLY);
  if (fd == -1)
    return -errno;

  res = pwrite(fd, buf, size, offset);
  if (res == -1)
    res = -errno;

  close(fd);
  return res;
}

static int xmp_statfs(const char *path, struct statvfs *stbuf)
{
  int res;

  char appendedPath[256];
  strcpy(appendedPath, mirrordir);
  strcat(appendedPath, path);
  res = statvfs(appendedPath, stbuf);
  if (res == -1)
    return -errno;

  return 0;
}

static int xmp_release(const char *path, struct fuse_file_info *fi)
{
  /* Just a stub.   This method is optional and can safely be left
     unimplemented */

  (void) path;
  (void) fi;
  return 0;
}

static int xmp_fsync(const char *path, int isdatasync,
                     struct fuse_file_info *fi)
{
  /* Just a stub.   This method is optional and can safely be left
     unimplemented */

  (void) path;
  (void) isdatasync;
  (void) fi;
  return 0;
}

#ifdef HAVE_SETXATTR
/* xattr operations are optional and can safely be left unimplemented */
static int xmp_setxattr(const char *path, const char *name, const char *value,
      size_t size, int flags)
{
  int res = lsetxattr(path, name, value, size, flags);
  if (res == -1)
    return -errno;
  return 0;
}

static int xmp_getxattr(const char *path, const char *name, char *value,
      size_t size)
{
  int res = lgetxattr(path, name, value, size);
  if (res == -1)
    return -errno;
  return res;
}

static int xmp_listxattr(const char *path, char *list, size_t size)
{
  int res = llistxattr(path, list, size);
  if (res == -1)
    return -errno;
  return res;
}

static int xmp_removexattr(const char *path, const char *name)
{
  int res = lremovexattr(path, name);
  if (res == -1)
    return -errno;
  return 0;
}
#endif /* HAVE_SETXATTR */

static struct fuse_operations xmp_oper = {
  .getattr  = xmp_getattr,
  .access    = xmp_access,
  .readlink  = xmp_readlink,
  .readdir  = xmp_readdir,
  .mknod    = xmp_mknod,
  .mkdir    = xmp_mkdir,
  .symlink  = xmp_symlink,
  .unlink    = xmp_unlink,
  .rmdir    = xmp_rmdir,
  .rename    = xmp_rename,
  .link    = xmp_link,
  .chmod    = xmp_chmod,
  .chown    = xmp_chown,
  .truncate  = xmp_truncate,
  .utimens  = xmp_utimens,
  .open    = xmp_open,
  .read    = xmp_read,
  .write    = xmp_write,
  .statfs    = xmp_statfs,
  .release  = xmp_release,
  .fsync    = xmp_fsync,
#ifdef HAVE_SETXATTR
  .setxattr  = xmp_setxattr,
  .getxattr  = xmp_getxattr,
  .listxattr  = xmp_listxattr,
  .removexattr  = xmp_removexattr,
#endif
};

int main(int argc, char *argv[])
{
  umask(0);
  return fuse_main(argc, argv, &xmp_oper, NULL);
}
