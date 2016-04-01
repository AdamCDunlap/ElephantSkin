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
#include <string>
#include <string>
#include <iostream>

using std::string;
using std::cout;
using std::cerr;

string mirrordir = "/home/adam/docs/hmc/files/ElephantSkin/backenddir";

static void copyFile(const string& from, const string& to)
{
  pid_t cpID = fork();
  if (cpID == 0) {
    // Child process, do the copy
    execl( "/bin/cp", "/bin/cp", "-p", from.c_str(), to.c_str(), (char*)NULL);
  } else {
    // Parent process, wait for it
    wait(NULL);
  }
}

static int xmp_getattr(const char *cpath, struct stat *stbuf)
{
  int res;

  string path(cpath);
  string mirrorpath = mirrordir + path;
  res = lstat(mirrorpath.c_str(), stbuf);
  if (res == -1)
    return -errno;

  return 0;
}

static int xmp_access(const char *cpath, int mask)
{
  int res;

  string path(cpath);
  string mirrorpath = mirrordir + path;
  res = access(mirrorpath.c_str(), mask);
  if (res == -1)
    return -errno;

  return 0;
}

static int xmp_readlink(const char *cpath, char *buf, size_t size)
{
  int res;

  string path(cpath);
  string mirrorpath = mirrordir + path;
  res = readlink(mirrorpath.c_str(), buf, size - 1);
  if (res == -1)
    return -errno;

  buf[res] = '\0';
  return 0;
}


static int xmp_readdir(const char *cpath, void *buf, fuse_fill_dir_t filler,
                       off_t offset, struct fuse_file_info *fi)
{
  DIR *dp;
  struct dirent *de;

  (void) offset;
  (void) fi;

  string path(cpath);
  string mirrorpath = mirrordir + path;
  dp = opendir(mirrorpath.c_str());
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

static int xmp_mknod(const char *cpath, mode_t mode, dev_t rdev)
{
  int res;
  string path(cpath);
  string mirrorpath = mirrordir + path;

  /* On Linux this could just be 'mknod(path, mode, rdev)' but this
     is more portable */
  if (S_ISREG(mode)) {
    res = open(mirrorpath.c_str(), O_CREAT | O_EXCL | O_WRONLY, mode);
    if (res >= 0)
      res = close(res);
  } else if (S_ISFIFO(mode))
    res = mkfifo(mirrorpath.c_str(), mode);
  else
    res = mknod(mirrorpath.c_str(), mode, rdev);
  if (res == -1)
    return -errno;

  return 0;
}

static int xmp_mkdir(const char *cpath, mode_t mode)
{
  int res;
  string path(cpath);
  string mirrorpath = mirrordir + path;

  res = mkdir(mirrorpath.c_str(), mode);
  if (res == -1)
    return -errno;

  return 0;
}

static int xmp_unlink(const char *cpath)
{
  int res;

  string path(cpath);
  string mirrorpath = mirrordir + path;
  res = unlink(mirrorpath.c_str());
  if (res == -1)
    return -errno;

  return 0;
}

static int xmp_rmdir(const char *cpath)
{
  int res;

  string path(cpath);
  string mirrorpath = mirrordir + path;
  res = rmdir(mirrorpath.c_str());
  if (res == -1)
    return -errno;

  return 0;
}

static int xmp_symlink(const char *cto, const char *cfrom)
{
  int res;
  string to(cto), from(cfrom);
  string mirrorto = mirrordir + to;
  string mirrorfrom = mirrordir + from;

  res = symlink(mirrorto.c_str(), mirrorfrom.c_str());
  if (res == -1)
    return -errno;

  return 0;
}

static int xmp_rename(const char *cfrom, const char *cto)
{
  int res;
  string to(cto), from(cfrom);
  string mirrorto = mirrordir + to;
  string mirrorfrom = mirrordir + from;

  res = rename(mirrorfrom.c_str(), mirrorto.c_str());
  if (res == -1)
    return -errno;

  return 0;
}

static int xmp_link(const char *cfrom, const char *cto)
{
  int res;

  string to(cto), from(cfrom);
  string mirrorto = mirrordir + to;
  string mirrorfrom = mirrordir + from;

  res = link(mirrorfrom.c_str(), mirrorto.c_str());
  if (res == -1)
    return -errno;

  return 0;
}

static int xmp_chmod(const char *cpath, mode_t mode)
{
  int res;

  string path(cpath);
  string mirrorpath = mirrordir + path;
  res = chmod(mirrorpath.c_str(), mode);
  if (res == -1)
    return -errno;

  return 0;
}

static int xmp_chown(const char *cpath, uid_t uid, gid_t gid)
{
  int res;

  string path(cpath);
  string mirrorpath = mirrordir + path;
  res = lchown(mirrorpath.c_str(), uid, gid);
  if (res == -1)
    return -errno;

  return 0;
}

static int xmp_truncate(const char *cpath, off_t size)
{
  int res;

  string path(cpath);
  string mirrorpath = mirrordir + path;

  // Make a copy of the file first!
  copyFile(mirrorpath, "/home/adam/lastTruncatedFile");

  res = truncate(mirrorpath.c_str(), size);
  if (res == -1)
    return -errno;

  return 0;
}

static int xmp_utimens(const char *cpath, const struct timespec ts[2])
{
  int res;
  struct timeval tv[2];

  tv[0].tv_sec = ts[0].tv_sec;
  tv[0].tv_usec = ts[0].tv_nsec / 1000;
  tv[1].tv_sec = ts[1].tv_sec;
  tv[1].tv_usec = ts[1].tv_nsec / 1000;

  string path(cpath);
  string mirrorpath = mirrordir + path;
  res = utimes(mirrorpath.c_str(), tv);
  if (res == -1)
    return -errno;

  return 0;
}

static int xmp_open(const char *cpath, struct fuse_file_info *fi)
{
  int res;

  string path(cpath);
  string mirrorpath = mirrordir + path;
  res = open(mirrorpath.c_str(), fi->flags);
  if (res == -1)
    return -errno;

  close(res);
  return 0;
}

static int xmp_read(const char *cpath, char *buf, size_t size, off_t offset,
                    struct fuse_file_info *fi)
{
  int fd;
  int res;

  string path(cpath);
  string mirrorpath = mirrordir + path;
  (void) fi;
  fd = open(mirrorpath.c_str(), O_RDONLY);
  if (fd == -1)
    return -errno;

  res = pread(fd, buf, size, offset);
  if (res == -1)
    res = -errno;

  close(fd);
  return res;
}

static int xmp_write(const char *cpath, const char *buf, size_t size,
                     off_t offset, struct fuse_file_info *fi)
{
  int fd;
  int res;

  (void) fi;
  string path(cpath);
  string mirrorpath = mirrordir + path;
  fd = open(mirrorpath.c_str(), O_WRONLY);
  if (fd == -1)
    return -errno;

  res = pwrite(fd, buf, size, offset);
  if (res == -1)
    res = -errno;

  close(fd);
  return res;
}

static int xmp_statfs(const char *cpath, struct statvfs *stbuf)
{
  int res;

  string path(cpath);
  string mirrorpath = mirrordir + path;
  res = statvfs(mirrorpath.c_str(), stbuf);
  if (res == -1)
    return -errno;

  return 0;
}

static int xmp_release(const char *cpath, struct fuse_file_info *fi)
{
  /* Just a stub.   This method is optional and can safely be left
     unimplemented */

  (void) cpath;
  (void) fi;
  return 0;
}

static int xmp_fsync(const char *cpath, int isdatasync,
                     struct fuse_file_info *fi)
{
  /* Just a stub.   This method is optional and can safely be left
     unimplemented */

  (void) cpath;
  (void) isdatasync;
  (void) fi;
  return 0;
}

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
};

int main(int argc, char *argv[])
{
  umask(0);
  return fuse_main(argc, argv, &xmp_oper, NULL);
}
