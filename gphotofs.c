/*
    FUSE Filesystem for libgphoto2.
    Copyright (C) 2005  Philip Langdale <philipl@mail.utexas.edu>
    Copyright (C) 2007  Marcus Meissner <marcus@jet.franken.de>

    This program can be distributed under the terms of the GNU GPL.
    See the file COPYING.
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <fuse.h>

#include <gphoto2/gphoto2.h>

#include <glib.h>
#include <glib/gi18n.h>
#include <glib/gprintf.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/time.h>

/*
 * Static variables set by command line arguments.
 */
static gchar *sPort = NULL;
static gchar *sModel = NULL;
static gchar *sUsbid = NULL;
static gint sSpeed = 0;
static gboolean sHelp;

static struct timeval glob_tv_zero;

/*
 * The OpenFile struct encapsulates a CameraFile and an open count.
 * This allows us to track multiple overlapping open/release calls
 * on a file.
 */

struct OpenFile {
   CameraFile *file;
   unsigned long count;

   void *buf;
   unsigned long size;
   int writing;
   gchar *destdir;
   gchar *destname;
};
typedef struct OpenFile OpenFile;

static void
freeOpenFile(OpenFile *openFile)
{
   gp_file_unref(openFile->file);
   g_free(openFile);
}


/*
 * gpresultToErrno:
 *
 * Maps libgphoto2 errors to errnos. For some errors, there isn't
 * really a good mapping, so a best effort has to be made.
 */

static int
gpresultToErrno(int result)
{
   switch (result) {
   case GP_ERROR:
      return -EPROTO;
   case GP_ERROR_BAD_PARAMETERS:
      return -EINVAL;
   case GP_ERROR_NO_MEMORY:
      return -ENOMEM;
   case GP_ERROR_LIBRARY:
      return -ENOSYS;
   case GP_ERROR_UNKNOWN_PORT:
      return -ENXIO;
   case GP_ERROR_NOT_SUPPORTED:
      return -EPROTONOSUPPORT;
   case GP_ERROR_TIMEOUT:
      return -ETIMEDOUT;
   case GP_ERROR_IO:
   case GP_ERROR_IO_SUPPORTED_SERIAL:
   case GP_ERROR_IO_SUPPORTED_USB:
   case GP_ERROR_IO_INIT:
   case GP_ERROR_IO_READ:
   case GP_ERROR_IO_WRITE:
   case GP_ERROR_IO_UPDATE:
   case GP_ERROR_IO_SERIAL_SPEED:
   case GP_ERROR_IO_USB_CLEAR_HALT:
   case GP_ERROR_IO_USB_FIND:
   case GP_ERROR_IO_USB_CLAIM:
   case GP_ERROR_IO_LOCK:
      return -EIO;

   case GP_ERROR_CAMERA_BUSY:
      return -EBUSY;
   case GP_ERROR_FILE_NOT_FOUND:
   case GP_ERROR_DIRECTORY_NOT_FOUND:
      return -ENOENT;
   case GP_ERROR_FILE_EXISTS:
   case GP_ERROR_DIRECTORY_EXISTS:
      return -EEXIST;
   case GP_ERROR_PATH_NOT_ABSOLUTE:
      return -ENOTDIR;
   case GP_ERROR_CORRUPTED_DATA:
      return -EIO;
   case GP_ERROR_CANCEL:
      return -ECANCELED;

   /* These are pretty dubious mappings. */
   case GP_ERROR_MODEL_NOT_FOUND:
      return -EPROTO;
   case GP_ERROR_CAMERA_ERROR:
      return -EPERM;
   case GP_ERROR_OS_FAILURE:
      return -EPIPE;
   }
   return -EINVAL;
}

struct GPCtx {
   Camera *camera;
   GPContext *context;
   CameraAbilitiesList *abilities;
   int debug_func_id;

   gchar *directory;
   GHashTable *files;
   GHashTable *dirs;
   GHashTable *reads;
   GHashTable *writes;
};
typedef struct GPCtx GPCtx;

static int
gphotofs_readdir(const char *path,
                 void *buf,
                 fuse_fill_dir_t filler,
                 off_t offset,
                 struct fuse_file_info *fi)
{
   GPCtx *p;
   CameraList *list = NULL;
   int i;
   int ret = 0;

   p = (GPCtx *)fuse_get_context()->private_data;

   filler(buf, ".", NULL, 0);
   filler(buf, "..", NULL, 0);

   /* Read directories */
   gp_list_new(&list);

   ret = gp_camera_folder_list_folders(p->camera, path, list, p->context);
   if (ret != 0) {
      goto error;
   }

   for (i = 0; i < gp_list_count(list); i++) {
      struct stat *stbuf;
      const char *name;
      gchar *key;

      stbuf = g_new0(struct stat, 1);
      stbuf->st_mode = S_IFDIR | 0755;
      /* This is not a correct number in general. */
      stbuf->st_nlink = 2;
      stbuf->st_uid = getuid();
      stbuf->st_gid = getgid();

      gp_list_get_name(list, i, &name);
      filler(buf, name, stbuf, 0);

      key = g_build_filename(path, name, NULL);

      g_hash_table_replace(p->dirs, key, stbuf);
   }

   gp_list_free(list);
   list = NULL;

   /* Read files */
   gp_list_new(&list);

   ret = gp_camera_folder_list_files(p->camera, path, list, p->context);
   if (ret != 0) {
      goto error;
   }

   for (i = 0; i < gp_list_count(list); i++) {
      struct stat *stbuf;
      const char *name;
      gchar *key;
      CameraFileInfo info;

      gp_list_get_name(list, i, &name);
      ret = gp_camera_file_get_info(p->camera, path, name, &info, p->context);
      if (ret != 0) {
         goto error;
      }

      stbuf = g_new0(struct stat, 1);
      stbuf->st_mode = S_IFREG | 0644;
      stbuf->st_nlink = 1;
      stbuf->st_uid = getuid();
      stbuf->st_gid = getgid();
      stbuf->st_size = info.file.size;
      stbuf->st_mtime = info.file.mtime;
      stbuf->st_blocks = (info.file.size / 512) +
                         (info.file.size % 512 > 0 ? 1 : 0);

      filler(buf, name, stbuf, 0);

      key = g_build_filename(path, name, NULL);

      g_hash_table_replace(p->files, key, stbuf);
   }

exit:
   if (list) {
      gp_list_free(list);
   }
   return ret;

 error:
   ret = gpresultToErrno(ret);
   goto exit;
}

static int
dummyfiller(void *buf, const char *name,
            const struct stat *stbuf, off_t off
) {
   return 0;
}


static int
gphotofs_getattr(const char *path,
                 struct stat *stbuf)
{
   int ret = 0;
   GPCtx *p = (GPCtx *)fuse_get_context()->private_data;
   struct stat *mystbuf = NULL;
   gpointer value;
   guint i;

   memset(stbuf, 0, sizeof(struct stat));
   if(strcmp(path, "/") == 0) {
      stbuf->st_mode = S_IFDIR | 0755;
      stbuf->st_nlink = 2;
      stbuf->st_uid = getuid();
      stbuf->st_gid = getgid();
      return 0;
   }

   /*
    * Due to the libgphoto2 api, the best way of verifying
    * if a file exists is to iterate the contents of that
    * directory; so if we don't know about the file already,
    * turn around and call readdir on the directory and try
    * again.
    */
   for (i = 2; i > 0; i--) {
      gchar *dir;
      value = g_hash_table_lookup(p->files, path);
      if (!value) {
	 value = g_hash_table_lookup(p->dirs, path);
      }
      if (value) {
	 mystbuf = (struct stat *)value;
	 break;
      }

      dir = g_path_get_dirname(path);
      ret = gphotofs_readdir(dir, NULL, dummyfiller, 0, NULL);
      g_free(dir);
      if (ret != 0) {
	 return ret;
      }
   }

   if (mystbuf) {
      stbuf->st_mode = mystbuf->st_mode;
      stbuf->st_nlink = mystbuf->st_nlink;
      stbuf->st_uid = mystbuf->st_uid;
      stbuf->st_gid = mystbuf->st_gid;
      stbuf->st_size = mystbuf->st_size;
      stbuf->st_blocks = mystbuf->st_blocks;
      stbuf->st_mtime = mystbuf->st_mtime;
   } else {
      ret = -ENOENT;
   }
   return ret;
}

static int
gphotofs_open(const char *path,
              struct fuse_file_info *fi)
{
   GPCtx *p = (GPCtx *)fuse_get_context()->private_data;
   OpenFile *openFile;


   if ((fi->flags & 3) == O_RDONLY) {
      openFile = g_hash_table_lookup(p->reads, path);
      if (!openFile) {
	 gchar *dir = g_path_get_dirname(path);
	 gchar *file = g_path_get_basename(path);
	 CameraFile *cFile;
	 int ret;

	 gp_file_new(&cFile);
	 ret = gp_camera_file_get(p->camera, dir, file, GP_FILE_TYPE_NORMAL,
				  cFile, p->context);
	 g_free(file);
	 g_free(dir);

	 if (ret != 0) {
	    gp_file_unref (cFile);
	    return gpresultToErrno(ret);
         }

	 openFile = g_new0(OpenFile, 1);
	 openFile->file = cFile;
	 openFile->count = 1;
	 g_hash_table_replace(p->reads, g_strdup(path), openFile);
      } else {
	 openFile->count++;
      }
      return 0;
   }
   if ((fi->flags & 3) == O_WRONLY) {
      openFile = g_hash_table_lookup(p->writes, path);
      if (!openFile) {
	 gchar *dir = g_path_get_dirname(path);
	 gchar *file = g_path_get_basename(path);

	 openFile = g_new0(OpenFile, 1);
	 openFile->file = NULL;
	 openFile->count = 1;
	 openFile->size = 0;
	 openFile->writing = 1;
	 openFile->destdir = g_strdup(dir);
	 openFile->destname = g_strdup(file);

	 openFile->buf = malloc(1);
	 if (!openFile->buf) return -1;
	 g_hash_table_replace(p->writes, g_strdup(path), openFile);

         g_free(dir);
         g_free(file);
      } else {
	 openFile->count++;
      }
      return 0;
   }
   return -EINVAL;
}

static int
gphotofs_read(const char *path,
              char *buf,
              size_t size,
              off_t offset,
              struct fuse_file_info *fi)
{
   GPCtx *p = (GPCtx *)fuse_get_context()->private_data;
   OpenFile *openFile;
   const char *data;
   unsigned long int dataSize;
   int ret;

   openFile = g_hash_table_lookup(p->reads, path);
   ret = gp_file_get_data_and_size(openFile->file, &data, &dataSize);
   if (ret == 0) {
      if (offset < dataSize) {
         if (offset + size > dataSize) {
            size = dataSize - offset;
         }
         memcpy(buf, data + offset, size);
         ret = size;
      } else {
         ret = 0;
      }
   } else {
      ret = gpresultToErrno(ret);
   }

   return ret;
}


/* ================================================================================== */


static int
gphotofs_write(const char *path, const char *wbuf, size_t size,
                       off_t offset, struct fuse_file_info *fi)
{
   GPCtx *p = (GPCtx *)fuse_get_context()->private_data;
   OpenFile *openFile = g_hash_table_lookup (p->writes, path);

   if (!openFile)
      return -1;
   if (offset + size > openFile->size) {
      openFile->size = offset + size;
      openFile->buf = realloc (openFile->buf, openFile->size);
   }
   memcpy(openFile->buf+offset, wbuf, size);
   return size;
}

static int
gphotofs_mkdir(const char *path, mode_t mode)
{
    int ret = 0;
    GPCtx *p = (GPCtx *)fuse_get_context()->private_data;
    gchar *dir = g_path_get_dirname(path);
    gchar *file = g_path_get_basename(path);

    ret = gp_camera_folder_make_dir(p->camera, dir, file, p->context);
    if (ret != 0) {
       ret = gpresultToErrno(ret);
    } else {
       struct stat *stbuf;	

       stbuf = g_new0(struct stat, 1);
       stbuf->st_mode = S_IFDIR | 0555;
       /* This is not a correct number in general. */
       stbuf->st_nlink = 2;
       stbuf->st_uid = getuid();
       stbuf->st_gid = getgid();
       g_hash_table_replace(p->dirs, g_strdup (path), stbuf);

    }
    g_free(dir);
    g_free(file);

    return ret;
}

static int
gphotofs_rmdir(const char *path)
{
    int ret = 0;

    GPCtx *p = (GPCtx *)fuse_get_context()->private_data;
    gchar *dir = g_path_get_dirname(path);
    gchar *file = g_path_get_basename(path);

    ret = gp_camera_folder_remove_dir(p->camera, dir, file, p->context);
    if (ret != 0) {
       ret = gpresultToErrno(ret);
    } else {
       g_hash_table_remove(p->dirs, path);
    }
    g_free(dir);
    g_free(file);
    return ret;
}

static int
gphotofs_mknod(const char *path, mode_t mode, dev_t rdev)
{
   GPCtx *p = (GPCtx *)fuse_get_context()->private_data;
   gchar *dir = g_path_get_dirname(path);
   gchar *file = g_path_get_basename(path);
   char *data;
   int res;
   CameraFile *cfile;

   gp_file_new (&cfile);
   data = malloc(1);
   data[0] = 'c';
   res = gp_file_set_data_and_size (cfile, data, 1);
   if (res < 0) {
      gp_file_unref (cfile);
      return -1;
   }
   res = gp_camera_folder_put_file (p->camera, dir, file, GP_FILE_TYPE_NORMAL, cfile,
				    p->context);
   gp_file_unref (cfile);
   g_free(dir);
   g_free(file);
   return 0;
}



static int gphotofs_flush(const char *path, struct fuse_file_info *fi)
{
   GPCtx *p = (GPCtx *)fuse_get_context()->private_data;
   OpenFile *openFile = g_hash_table_lookup(p->writes, path);

   if (!openFile)
      return 0;
   if (openFile->writing) {
      int res;
      CameraFile *file;
      char *data;
      gp_file_new (&file);
      data = malloc (openFile->size);
      if (!data)
	 return -ENOMEM;
      memcpy (data, openFile->buf, openFile->size);
      /* The call below takes over responsbility of freeing data. */
      res = gp_file_set_data_and_size (file, data, openFile->size);
      if (res < 0) {
	 gp_file_unref (file);
	 return -1;
      }
      res = gp_camera_file_delete(p->camera, openFile->destdir, openFile->destname, p->context);
      res = gp_camera_folder_put_file (p->camera, openFile->destdir, openFile->destname, GP_FILE_TYPE_NORMAL, file, p->context);
      if (res < 0)
	 return -ENOSPC;
      gp_file_unref (file);
   }
   return 0;
}

static int gphotofs_fsync(const char *path, int isdatasync,
                       struct fuse_file_info *fi)
{
    (void) isdatasync;
    return gphotofs_flush(path, fi);
}



static int gphotofs_chmod(const char *path, mode_t mode)
{
    return 0;
}

static int gphotofs_chown(const char *path, uid_t uid, gid_t gid)
{
    return 0;
}

static int gphotofs_statfs(const char *path, struct statvfs *stvfs)
{
   GPCtx *p = (GPCtx *)fuse_get_context()->private_data;
   CameraStorageInformation *sifs;
   int res, nrofsifs;

   res = gp_camera_get_storageinfo (p->camera, &sifs, &nrofsifs, p->context);
   if (res < GP_OK)
      return gpresultToErrno(res);
   if (nrofsifs == 0)
      return -ENOSYS;

   if (nrofsifs == 1) {
      stvfs->f_bsize = 1024;
      stvfs->f_blocks = sifs->capacitykbytes;
      stvfs->f_bfree = sifs->freekbytes;
      stvfs->f_bavail = sifs->freekbytes;
      stvfs->f_files = -1;
      stvfs->f_ffree = -1;
      stvfs->f_favail = -1;
   }
   return 0;
}

static int
gphotofs_release(const char *path,
                 struct fuse_file_info *fi)
{
   GPCtx *p = (GPCtx *)fuse_get_context()->private_data;
   OpenFile *openFile = g_hash_table_lookup(p->reads, path);

   if (!openFile) openFile = g_hash_table_lookup(p->writes, path);

   if (openFile) {
      openFile->count--;
      if (openFile->count == 0) {
         if (openFile->writing) {
	     free (openFile->buf);
             g_hash_table_remove(p->writes, path);
         } else  {
             g_hash_table_remove(p->reads, path);
         }
      }
   }

   return 0;
}

static int
gphotofs_unlink(const char *path)
{
   GPCtx *p = (GPCtx *)fuse_get_context()->private_data;
   gchar *dir = g_path_get_dirname(path);
   gchar *file = g_path_get_basename(path);
   int ret = 0;

   /* Don't allow deletion of open files. */
   if (g_hash_table_lookup(p->reads, path)) {
      ret = -EBUSY;
      goto exit;
   }

   ret = gp_camera_file_delete(p->camera, dir, file, p->context);
   if (ret != 0) {
      ret = gpresultToErrno(ret);
      goto exit;
   }

   g_hash_table_remove(p->files, path);
 exit:
   g_free(dir);
   g_free(file);

   return ret;

}

static void
#ifdef __GNUC__
                __attribute__((__format__(printf,3,0)))
#endif
debug_func (GPLogLevel level, const char *domain, const char *format,
            va_list args, void *data)
{
   struct timeval tv;
   long sec, usec;
   FILE *logfile = data;

   gettimeofday (&tv,NULL);
   sec = tv.tv_sec  - glob_tv_zero.tv_sec;
   usec = tv.tv_usec - glob_tv_zero.tv_usec;
   if (usec < 0) {sec--; usec += 1000000L;}
   fprintf (logfile, "%li.%06li %s(%i): ", sec, usec, domain, level);
   vfprintf (logfile, format, args);
   fputc ('\n', logfile);
   fflush (logfile);
}

static void *
gphotofs_init()
{
   int ret = GP_OK;
   GPCtx *p = g_new0(GPCtx, 1);

#if 0 /* enable for debugging */
   int fd = -1;
   FILE *f = NULL;

   fd = open("/tmp/gpfs.log",O_WRONLY|O_CREAT,0600);
   if (fd != -1) {
      f = fdopen(fd,"a");
      if (f)
         p->debug_func_id = gp_log_add_func (GP_LOG_ALL, debug_func, (void *) f);
   }
   fprintf (f, "log opened on pid %d\n", getpid());
#endif

   p->context = gp_context_new();
   gettimeofday (&glob_tv_zero, NULL);

   setlocale (LC_CTYPE,"en_US.UTF-8"); /* for ptp2 driver to convert to utf-8 */

   gp_camera_new (&p->camera);

   gp_abilities_list_new(&p->abilities);
   gp_abilities_list_load(p->abilities, p->context);

   if (sSpeed) {
      GPPortInfo	info;
      GPPortType	type;

      /* Make sure we've got a serial port. */
      ret = gp_camera_get_port_info(p->camera, &info);
      gp_port_info_get_type (info, &type);
      if (ret != 0) {
         goto error;
      } else if (type != GP_PORT_SERIAL) {
         g_fprintf(stderr, "%s\n", _("You can only specify speeds for serial ports."));
         goto error;
      }

      /* Set the speed. */
      ret = gp_camera_set_port_speed(p->camera, sSpeed);
      if (ret != 0) {
         goto error;
      }
   }

   if (sModel) {
      CameraAbilities a;
      int m;

      m = gp_abilities_list_lookup_model(p->abilities, sModel);
      if (m < 0) {
         g_fprintf(stderr, _("Model %s was not recognised."), sModel);
         g_fprintf(stderr, "\n");
         goto error;
      }

      ret = gp_abilities_list_get_abilities(p->abilities, m, &a);
      if (ret != 0) {
         goto error;
      }

      ret = gp_camera_set_abilities(p->camera, a);
      if (ret != 0) {
         goto error;
      }

      /* Marcus: why save it? puzzling. */
      ret = gp_setting_set("gphoto2", "model", a.model);
      if (ret != 0) {
         goto error;
      }
   }

   if (sPort) {
      GPPortInfo info;
      GPPortInfoList *il = NULL;
      int i;

      ret = gp_port_info_list_new(&il);
      if (ret != 0) {
         goto error;
      }

      ret = gp_port_info_list_load(il);
      if (ret != 0) {
         goto error;
      }

      i = gp_port_info_list_lookup_path(il, sPort);
      if (i == GP_ERROR_UNKNOWN_PORT) {
         g_fprintf(stderr,
                   _("The port you specified ('%s') can not "
                     "be found. Please specify one of the ports "
                     "found by 'gphoto2 --list-ports' make sure "
                     "the spelling is correct (i.e. with prefix "
                     "'serial:' or 'usb:')."), sPort);
         g_fprintf(stderr, "\n");
         goto error;
      } else if (i < 0) {
         ret = i;
         goto error;
      } else {
	 char *xpath;
         ret = gp_port_info_list_get_info(il, i, &info);
         if (ret != 0) {
            goto error;
         }

         ret = gp_camera_set_port_info (p->camera, info);
         if (ret != 0)
            goto error;
         /* Marcus: why save it? puzzling. */
	 gp_port_info_get_path (info, &xpath);
         gp_setting_set("gphoto2", "port", xpath);

         gp_port_info_list_free(il);
      }
   }

   p->dirs = g_hash_table_new_full(g_str_hash, g_str_equal,
                                   g_free, g_free);
   p->files = g_hash_table_new_full(g_str_hash, g_str_equal,
                                    g_free, g_free);
   p->reads = g_hash_table_new_full(g_str_hash, g_str_equal,
                                    g_free, (GDestroyNotify)freeOpenFile);
   p->writes = g_hash_table_new_full(g_str_hash, g_str_equal,
                                    g_free, (GDestroyNotify)freeOpenFile);

   return p;

 error:
   if (ret != GP_OK) {
      g_fprintf(stderr, _("Error initialising gphotofs: %s"),
                gp_result_as_string(ret));
      g_fprintf(stderr, "\n");
   }
   exit(EXIT_FAILURE);
}

static void
gphotofs_destroy(void *context)
{
   if (!context) {
      return;
   }

   GPCtx *p = (GPCtx *)context;

   if (p->reads) {
      g_hash_table_destroy(p->reads);
   }
   if (p->files) {
      g_hash_table_destroy(p->files);
   }
   if (p->dirs) {
      g_hash_table_destroy(p->dirs);
   }
   g_free(p->directory);

   if (p->abilities) {
      gp_abilities_list_free(p->abilities);
   }
   if (p->camera) {
      gp_camera_unref(p->camera);
   }
   if (p->context) {
      gp_context_unref(p->context);
   }
   g_free(p);
}

static struct fuse_operations gphotofs_oper = {
    .init	= gphotofs_init,
    .destroy	= gphotofs_destroy,
    .readdir	= gphotofs_readdir,
    .getattr	= gphotofs_getattr,
    .open	= gphotofs_open,
    .read	= gphotofs_read,
    .release	= gphotofs_release,
    .unlink	= gphotofs_unlink,

    .write	= gphotofs_write,
    .mkdir	= gphotofs_mkdir,
    .rmdir	= gphotofs_rmdir,
    .mknod	= gphotofs_mknod,
    .flush	= gphotofs_flush,
    .fsync	= gphotofs_fsync,

    .chmod	= gphotofs_chmod,
    .chown	= gphotofs_chown,

    .statfs	= gphotofs_statfs
};

static GOptionEntry options[] =
{
   { "port", 0, 0, G_OPTION_ARG_STRING, &sPort, N_("Specify port device"), "path" },
   { "speed", 0, 0, G_OPTION_ARG_INT, &sSpeed, N_("Specify serial transfer speed"), "speed" },
   { "camera", 0, 0, G_OPTION_ARG_STRING, &sModel, N_("Specify camera model"), "model" },
   { "usbid", 0, 0, G_OPTION_ARG_STRING, &sUsbid, N_("(expert only) Override USB IDs"), "usbid" },
   { "help-fuse", 'h', 0, G_OPTION_ARG_NONE, &sHelp, N_("Show FUSE help options"), NULL },
};

int
main(int argc,
     char *argv[])
{
   GError *error = NULL;

   GOptionContext *context = g_option_context_new(_("- gphoto filesystem"));
   g_option_context_add_main_entries(context, options, GETTEXT_PACKAGE);
   g_option_context_set_ignore_unknown_options(context, TRUE);
   g_option_context_parse(context, &argc, &argv, &error);

   if (sHelp) {
      const char *fusehelp[] = { argv[0], "-ho", NULL};

      return fuse_main(2, (char **)fusehelp, &gphotofs_oper);
   } else if (sUsbid) {
      g_fprintf(stderr, "--usbid is not yet implemented\n");
      return 1;
   } else {
     char **newargv = malloc ( (argc+2)*sizeof(char*));
     memcpy (newargv+2,argv+1,sizeof(char*)*(argc-1));
     newargv[0] = argv[0];
     newargv[1] = "-s"; /* disable multithreading */

     return fuse_main(argc+1, newargv, &gphotofs_oper);
   }
}
