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
#include <array>
#include <tuple>

using std::string;
using std::cout;
using std::cerr;
using std::endl;

static string mirrordir;

static const string term_red = "[0;31m";
static const string term_yellow = "[0;33m";
static const string term_reset = "[0;0m";


static void copyFile(const string& from, const string& to)
{
  // fork splits this process into two exact copies. vfork doesn't make a fully
  // copy, but we don't care because the child process isn't going to modify
  // anything, it's just going to build arguments and exec cp
  const pid_t cpID = vfork();
  if (cpID == 0) {
    // This is the child process, exec cp to do the copy

    // -p means to preserve the mode, ownership, and timestamps
    // This array must end with nullptr because that's what execv wants
    const std::array<const char*, 5> execvargs{{
      "/bin/cp", "-a", from.c_str(), to.c_str(), nullptr
    }};

    // const_cast is scary but we're assured that execv won't actually modify
    // the array http://stackoverflow.com/a/190208
    // http://pubs.opengroup.org/onlinepubs/9699919799/functions/exec.html
    execv("/bin/cp", const_cast<char**>(execvargs.data()));
  } else {
    // This is the parent process, so wait for the copy to finish and then reap
    // it.
    wait(nullptr);
  }
}

// Given path, return the child name (part after last /) and parent name (the
// rest of it, not including the /. Return empty string for root directory.
std::tuple<string, string> break_off_last_path_entry(const string& path) {

  size_t last_delim_pos = path.find_last_of('/');
  if (last_delim_pos == string::npos) {
    cerr << "Was given a string without a slash..." << endl;
    exit(2);
  }

  string parent_path = path.substr(0, last_delim_pos);
  
  // Return (parent, child)
  return std::make_tuple(parent_path, path.substr(last_delim_pos+1));
}


static void backupFile(const string& path) {
  // Get filename, which is the current time in ISO8601 format
  time_t now;
  time(&now);
  std::array<char, sizeof "2016-04-24T18:21:45Z"> timebuf;
  strftime(timebuf.data(), timebuf.size(), "%FT%TZ", gmtime(&now));
  string timestring(timebuf.begin(), timebuf.end());

  string containing_dir, filename;
  std::tie(containing_dir, filename) = break_off_last_path_entry(path);

  // Make .snapshots directory
  string newLocationBuilder = containing_dir + "/.snapshots";
  cerr << "Making" << newLocationBuilder << endl;
  int err = mkdir( newLocationBuilder.c_str(), 0700);
  // If we got an error that's not a "file already exists" error
  if (err == -1 && errno != EEXIST) {
    cerr << "Couldn't make " << newLocationBuilder << " error was " << strerror(errno) << "(" << errno << ")" << endl;
  }

  // Make .snapshots/thefile directory
  newLocationBuilder += "/" + filename;
  cerr << "Making" << newLocationBuilder << endl;
  err = mkdir( newLocationBuilder.c_str(), 0700);
  // If we got an error that's not a "file already exists" error
  if (err == -1 && errno != EEXIST) {
    cerr << "Couldn't make " << newLocationBuilder << " error was " << strerror(errno) << "(" << errno << ")" << endl;
  }

  // Copy the file to .snapsots/thefile/thetime
  newLocationBuilder += "/" + timestring;
  cerr << "Copying to " << newLocationBuilder << endl;
  copyFile(path, newLocationBuilder);
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

  backupFile(mirrorpath);

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

  backupFile(mirrorpath);

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

  backupFile(mirrorpath);

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

  if (argc < 2) {
    cerr << "First argument should be the backend directory" << endl;
    return 2;
  }

  char* c_cwd = get_current_dir_name();
  mirrordir = string(c_cwd) + "/" + argv[1];
  free(c_cwd);
  ++argv;
  --argc;

  cout << "Opening " << mirrordir << " as backend directory" << endl;

  return fuse_main(argc, argv, &xmp_oper, NULL);
}
