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
#include <cassert>
#include <string>
#include <iostream>
#include <array>
#include <tuple>
#include <thread>
#include <vector>
#include <algorithm>
#include <chrono>
#include <iomanip>

using std::string;
using std::cout;
using std::cerr;
using std::endl;

static string mirrordir;

static const string term_red = "[0;31m";
static const string term_yellow = "[0;33m";
static const string term_reset = "[0;0m";

static int GARBAGE_INTERVAL = 5; //how often to garbage collect in seconds
static string SNAPSHOT_DIRECTORY_NAME = ".elephant_snapshot";
static int LANDMARK_AGE = 604800;  //the amount of time (in seconds) to keep all
                                   //backups, default to 7 days
static int LANDMARK_AMOUNT = 50;   //how many version of a file to keep before
                                   //cleaning some up 

using clk = std::chrono::system_clock;
// year-month-day-hour:minutes:seconds
static string backup_timestamp_fmt = "%Y-%m-%d-%T";

string parentDir = "..";
string selfDir = ".";                           

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

//class iteratable_directory {
//  const string dirname_;
// public:
//  iteratable_directory(const string& dirname) 
//    : dirname_(dirname)
//  {
//  }
//
//  class iterator {
//    DIR* dir;
//    iterator(const string& dirname) 
//      : dir(opendir(dirname.c_str()))
//    {
//    }
//    iterator(bool)
//      : dir(nullptr)
//    {
//    }
//    friend class iteratable_directory;
//   public:
//    string operator*() {
//      dirent entry;
//      dirent* entry_ptr;
//      if(readdir_r(dir, &entry, &entry_ptr) != 0) {
//        cerr << "Readdir failed!" << endl;
//      }
//      if (entry_ptr == nullptr) {
//        // This means we're done
//        dir = nullptr;
//      }
//      return string(entry.d_name);
//    }
//    iteratable_directory::iterator& operator++() {
//      return *this;
//    }
//    
//  };
//
//  iterator begin() {
//    return iterator(dirname_);
//  }
//  iterator end() {
//    return iterator(false);
//  }
//};

// Reads the given directory and calls callback with the filename for every
// file that is not . or ..
static void directory_map(const string& dirname,
      std::function<void(const string&)> callback) {
  DIR* dir = opendir(dirname.c_str());
  while(true) {
    dirent entry;
    dirent* entry_ptr;
    if(readdir_r(dir, &entry, &entry_ptr) != 0) {
      cerr << "Readdir failed!" << endl;
      exit(3);
    }
    if (entry_ptr == nullptr) {
      // This means we're done
      break;
    }
    std::string filename(entry.d_name);
    if(filename == parentDir || filename == selfDir) {
      continue;
    }
    callback(filename);
  }
  closedir(dir);
}

//the function to determine whether to keep a file past its landmark
static bool keepFileEvaluation( const std::time_t& time_newest,
                                const std::time_t& time_curr,
                                const int iteration_newest,
                                const int iteration_prev,
                                const int iteration_curr){
                                    
  int iterations_since_last_keep = iteration_prev - iteration_curr;
  //some smart function to see how often to keep
  int keep_threshold = 3; //temporary value
  
  cerr << "newest iter " << iteration_newest << " current iter " << iteration_curr << " since kept " << iterations_since_last_keep << std::endl;
  //first check if the backup is new enough and not too many have been stored
  //if so, automatically keep it, otherwise compare against function
  if((iteration_newest - iteration_curr) > LANDMARK_AMOUNT or (time_curr - time_newest) > LANDMARK_AGE){
    if(iterations_since_last_keep >= keep_threshold){
      return true;
    } else {
      return false;
    }
  } else {
    return true;
  }
}

// Given a filename of a backup, return its creation time and its iteration
// number
std::tuple<time_t, size_t> get_time_and_iteration_from_filename
    (const string& name) {
  std::stringstream namestringstream(name);

  std::tm filetime_as_tm;
  char underscore;
  size_t currIteration;
  namestringstream
    >> std::get_time(&filetime_as_tm, backup_timestamp_fmt.c_str())
    >> underscore >> currIteration;
  assert(!namestringstream.fail());
  std::time_t filetime_as_time_t = std::mktime(&filetime_as_tm);

  cout << term_yellow << "From name " << name << " we got time " <<
   std::put_time(&filetime_as_tm, backup_timestamp_fmt.c_str()) <<
     " and iteration " << currIteration <<
    " and underscore " << underscore << term_reset << endl;
  assert(underscore == '_');

  return std::make_tuple(filetime_as_time_t, currIteration);
}

static void backupFile(const string& path) {
  string containing_dir, filename;
  std::tie(containing_dir, filename) = break_off_last_path_entry(path);

  // Make .elephant_snapshots directory
  std::stringstream newLocationBuilder;
  newLocationBuilder << containing_dir << "/" << SNAPSHOT_DIRECTORY_NAME;
  cerr << "Making" << newLocationBuilder.str() << endl;
  int err = mkdir( newLocationBuilder.str().c_str(), 0700);
  // If we got an error that's not a "file already exists" error
  if (err == -1 && errno != EEXIST) {
    cerr << "Couldn't make " << newLocationBuilder.str() << " error was " << strerror(errno) << "(" << errno << ")" << endl;
  }

  // Make .snapshots/thefile directory
  newLocationBuilder << "/" << filename;
  cerr << "Making" << newLocationBuilder.str() << endl;
  err = mkdir( newLocationBuilder.str().c_str(), 0700);
  // If we got an error that's not a "file already exists" error
  if (err == -1 && errno != EEXIST) {
    cerr << "Couldn't make " << newLocationBuilder.str() << " error was " << strerror(errno) << "(" << errno << ")" << endl;
  }

  // Get the current time in the right format
  std::time_t timept_as_time_t = clk::to_time_t(clk::now());
  std::tm* timept_as_tm = std::localtime(&timept_as_time_t);
  std::stringstream time_stringstream;
  time_stringstream << std::put_time(timept_as_tm, backup_timestamp_fmt.c_str());
  string timestring = time_stringstream.str();

  // Get the largest revision number in the directory
  size_t largest_previous_revision_number = 0;
  directory_map(newLocationBuilder.str(),
    [&largest_previous_revision_number](const string& backup_name) {
      size_t revision_number;
      std::tie(std::ignore, revision_number) =
        get_time_and_iteration_from_filename(backup_name);

      largest_previous_revision_number
        = std::max(largest_previous_revision_number, revision_number);

  });

  newLocationBuilder << "/" << timestring << "_" << largest_previous_revision_number+1;

  // Copy the file to .snapsots/thefile/thetime
  cerr << "Copying to " << newLocationBuilder.str() << endl;
  copyFile(path, newLocationBuilder.str());
}

static void cleanup_backups(const string& current_directory){
  //clean one file at a time by drilling into its directory
  cerr<< "entering backups folder " << current_directory<< std::endl;

  // For each backed up file in this directory...
  directory_map(current_directory, [&current_directory](const string& backup_dir_name) {
    // Vector of filenames for the backups for this file
    std::vector<string> backups;
    string next_path = current_directory + "/" + backup_dir_name;
    cerr << "opening path: " << next_path << std::endl;

    directory_map(next_path, [&backups](const string& backup_file_name) {
      backups.push_back(backup_file_name);
    });

    std::sort(backups.begin(), backups.end());
    //get most recent value against which to compare rest
    string mostRecentName;
    int mostRecentDate;
    int mostRecentIteration;
    int prevIteration;
    //cerr << "after abort test-1" << std::endl;
    if(!backups.empty()){

      mostRecentName = backups.back();
      backups.pop_back();
      prevIteration = mostRecentIteration;
      std::tie(mostRecentDate, mostRecentIteration) =
        get_time_and_iteration_from_filename(mostRecentName);
    }

    while(!backups.empty()){
      string currName = backups.back();
      backups.pop_back();

      std::time_t now_as_time_t = clk::to_time_t(clk::now());

      std::time_t thisFileTime;
      size_t currIteration;
      std::tie(thisFileTime, currIteration) =
        get_time_and_iteration_from_filename(currName);

      if(keepFileEvaluation(now_as_time_t, thisFileTime, mostRecentIteration, prevIteration, currIteration)){
        //iterationsSinceKept = 0;
        prevIteration = currIteration;
      } else {
        //++iterationsSinceKept;
        //string full_dir = current_directory + "/" + entry->d_name + "/" + currName);
        //cerr << "unlinking: " << full_dir << std::endl;
        //unlink(full_dir.c_str());
      }
    }

  });

//  DIR *dir = opendir(current_directory.c_str());
//  struct dirent *entry = readdir(dir);
//  while (entry != nullptr) {
//    if( parentDir.compare(entry->d_name)  != 0 &&
//        selfDir.compare(entry->d_name)    != 0){
//      cerr<< "In cleanup if: " << entry->d_name << std::endl;  
//      std::vector<string> backups;
//      string next_path = ((string)current_directory + "/" + (string)entry->d_name);
//      cerr << "opening path: " << next_path << std::endl;
//      DIR *dir_backups = opendir(next_path.c_str());
//      struct dirent *backup = readdir(dir_backups);
//      cerr<< "Current Backup:" << backup->d_name << std::endl;
//      while(backup != nullptr){
//        if( parentDir.compare(backup->d_name) != 0 && //dont want to check .. and .
//            selfDir.compare(backup->d_name) != 0){
//          cerr << "Adding to backup list " << backup->d_name << std::endl;
//          backups.push_back(backup->d_name);
//        }
//        backup = readdir(dir_backups);
//      }
//      closedir(dir_backups);
//      //cerr << "after abort test-5" << std::endl;
//      //alphabetical should make it oldest->newest
//      std::sort(backups.begin(), backups.end());
//      //get most recent value against which to compare rest
//      string mostRecentName;
//      int mostRecentDate;
//      int mostRecentIteration;
//      int iterationsSinceKept = 0;
//      //cerr << "after abort test-1" << std::endl;
//      if(!backups.empty()){
//        mostRecentName = backups.back();
//        backups.pop_back();
//        std::string::size_type n = mostRecentName.find( '_' );
//        mostRecentDate = stoi(mostRecentName.substr(0, n));
//        //cerr << "date " << mostRecentDate << std::endl;
//        mostRecentIteration = stoi(mostRecentName.substr(n+1));
//        //cerr << "iteration " << mostRecentIteration << std::endl;
//      }
//      cerr << "after abort test1" << std::endl;
//      while(!backups.empty()){
//        string currName = backups.back();
//        backups.pop_back();
//        std::string::size_type separator_pos = currName.find( '_' );
//
//
//        std::stringstream time_stringstream;
//        time_stringstream << currName.substr(0,separator_pos);
//        std::tm filetime_as_tm;
//        time_stringstream >> std::get_time(&filetime_as_tm, backup_timestamp_fmt.c_str());
//        std::time_t filetime_as_time_t = std::mktime(&filetime_as_tm);
//
//        size_t currIteration = stoi(currName.substr(separator_pos+1));
//
//        std::time_t now_as_time_t = clk::to_time_t(clk::now());
//
//        if(keepFileEvaluation(now_as_time_t, filetime_as_time_t, mostRecentIteration, currIteration, iterationsSinceKept)){
//          iterationsSinceKept = 0;
//        } else {
//          ++iterationsSinceKept;
//          string full_dir = ((string)current_directory + "/" + (string)entry->d_name + "/" + currName);
//          cerr << "unlinking: " << full_dir << std::endl;
//          unlink(full_dir.c_str());
//        }
//      }
//      cerr << "after abort test15" << std::endl;
//    }
//    entry = readdir(dir);
//  }
//  closedir(dir);
//  return;
}

//work way down the directory tree
//everytime it sees a .elephant_snapshot folder, call cleanup
static void traverse_directory_tree(const string current_directory){
  cerr << "traversting to this dir " << current_directory << std::endl;
  DIR *dir = opendir(current_directory.c_str());
  struct dirent *entry = readdir(dir);
  //read out files one at a time
  while (entry != nullptr)
  {
    //cerr << "before if: " << current_directory+"/"+entry->d_name << std::endl;
    if(parentDir.compare(entry->d_name) != 0 && 
      selfDir.compare(entry->d_name) != 0){ //dont want to check .. and .
      //stat to check if its a directory
      struct stat st;
      lstat((current_directory+"/"+entry->d_name).c_str(), &st);
      cerr << "just stated: " << current_directory+"/"+entry->d_name << std::endl;
      if(S_ISDIR(st.st_mode)){
        //clean in the snapshot_directory, keep traversing otherwise
        if(SNAPSHOT_DIRECTORY_NAME.compare(entry->d_name) == 0){
          cleanup_backups(current_directory+"/"+entry->d_name);
        } else {
          traverse_directory_tree(current_directory+"/"+entry->d_name);
        }
      }
    }
    entry = readdir(dir);
  }
  closedir(dir);
  return;
}

static void collectGarbage(){
  while(true){
    sleep(GARBAGE_INTERVAL);
    traverse_directory_tree(mirrordir);
  }
}

static int xmp_getattr(const char *cpath, struct stat *stbuf)
{
  int res;

  string path(cpath);
  cerr << path << std::endl;
  string mirrorpath = mirrordir + path;
  cerr << mirrorpath << std::endl;
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

  std::thread garbage_collection(collectGarbage);
  
  return fuse_main(argc, argv, &xmp_oper, NULL);
}
