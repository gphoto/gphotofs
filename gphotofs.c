/*
    FUSE Filesystem for libgphoto2.
    Copyright (C) 2005  Philip Langdale <philipl@mail.utexas.edu>

    This program can be distributed under the terms of the GNU GPL.
    See the file COPYING.
*/

#include <fuse.h>

#include <gphoto2.h>

#include <glib.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>

struct OpenFile {
   CameraFile *file;
   guint count;
};
typedef struct OpenFile OpenFile;

static void
freeOpenFile(OpenFile *openFile)
{
   gp_file_unref(openFile->file);
   g_free(openFile);
}

static int
gpresultToErrno(int result)
{
   switch (result) {
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
}

struct GPCtx {
   Camera *camera;
   GPContext *context;
   CameraAbilitiesList *abilities;

   gchar *directory;
   GHashTable *files;
   GHashTable *dirs;
   GHashTable *reads;
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
   CameraList *list;
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
      stbuf->st_mode = S_IFDIR | 0555;
      /* This is not a correct number in general. */
      stbuf->st_nlink = 2;

      gp_list_get_name(list, i, &name);
      filler(buf, name, stbuf, 0);

      key = g_build_filename(path, name, NULL);

      g_hash_table_replace(p->dirs, key, stbuf);
   }

   /* Read files */
   gp_list_free(list);
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
      stbuf->st_mode = S_IFREG | 0444;
      stbuf->st_nlink = 1;
      stbuf->st_size = info.file.size;
      stbuf->st_mtime = info.file.mtime;
      stbuf->st_blocks = (info.file.size / 512) +
                         (info.file.size % 512 > 0 ? 1 : 0);

      filler(buf, name, stbuf, 0);

      key = g_build_filename(path, name, NULL);

      g_hash_table_replace(p->files, key, stbuf);
   }

   return ret;

 error:
   return gpresultToErrno(ret);
}

static int dummyfiller(void *buf,
                       const char *name,
                       const struct stat *stbuf,
                       off_t off)
{
   return 0;
}


static int
gphotofs_getattr(const char *path,
                 struct stat *stbuf)
{
   int ret = 0;
   GPCtx *p = (GPCtx *)fuse_get_context()->private_data;

   memset(stbuf, 0, sizeof(struct stat));

   if(strcmp(path, "/") == 0) {
      stbuf->st_mode = S_IFDIR | 0555;
      stbuf->st_nlink = 2;
   } else {
      struct stat *mystbuf = NULL;
      gpointer value;
      guint i;

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
         stbuf->st_size = mystbuf->st_size;
         stbuf->st_mtime = mystbuf->st_mtime;
         stbuf->st_blocks = mystbuf->st_blocks;
      } else {
         ret = -ENOENT;
      }
   }
   return ret;
}

static int
gphotofs_open(const char *path,
              struct fuse_file_info *fi)
{
   GPCtx *p = (GPCtx *)fuse_get_context()->private_data;
   OpenFile *openFile;

   if((fi->flags & 3) != O_RDONLY) {
      return -EACCES;
   }

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

static int
gphotofs_read(const char *path,
              char *buf, size_t size,
              off_t offset,
              struct fuse_file_info *fi)
{
   GPCtx *p = (GPCtx *)fuse_get_context()->private_data;
   OpenFile *openFile;
   const char *data;
   unsigned long int dataSize;

   openFile = g_hash_table_lookup(p->reads, path);
   if (gp_file_get_data_and_size(openFile->file, &data, &dataSize) == 0 &&
       offset < dataSize) {
      if (offset + size > dataSize) {
         size = dataSize - offset;
      }
      memcpy(buf, data + offset, size);
   } else {
      size = 0;
   }

   return size;
}

static int
gphotofs_release(const char *path,
                 struct fuse_file_info *fi)
{
   GPCtx *p = (GPCtx *)fuse_get_context()->private_data;
   OpenFile *openFile = g_hash_table_lookup(p->reads, path);

   openFile->count--;
   if (openFile->count == 0) {
      g_hash_table_remove(p->reads, path);
   }

   return 0;
}

static int
gphotofs_unlink(const char *path)
{
   GPCtx *p = (GPCtx *)fuse_get_context()->private_data;
   gchar *dir = g_path_get_dirname(path);
   gchar *file = g_path_get_basename(path);
   int ret;

   /* Don't allow deletion of open files. */
   if (g_hash_table_lookup(p->reads, path)) {
      return -EBUSY;
   }

   ret = gp_camera_file_delete(p->camera, dir, file, p->context);
   if (ret != 0) {
      return gpresultToErrno(ret);
   }

   g_hash_table_remove(p->files, path);

   return 0;
}

static void *
gphotofs_init(void)
{
   GPCtx *p = g_new0(GPCtx, 1);

   gp_camera_new(&p->camera);
   p->context = gp_context_new();

   p->dirs = g_hash_table_new_full(g_str_hash, g_str_equal,
                                   g_free, g_free);
   p->files = g_hash_table_new_full(g_str_hash, g_str_equal,
                                    g_free, g_free);
   p->reads = g_hash_table_new_full(g_str_hash, g_str_equal,
                                    g_free, (GDestroyNotify)freeOpenFile);

   return p;
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
};

int main(int argc, char *argv[])
{
    return fuse_main(argc, argv, &gphotofs_oper);
}
