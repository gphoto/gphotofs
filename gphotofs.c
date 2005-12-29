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
gphoto_readdir(const char *path,
               void *buf,
               fuse_fill_dir_t filler,
               off_t offset,
               struct fuse_file_info *fi)
{
   GPCtx *p;
   CameraList *list;
   int i;

   p = (GPCtx *)fuse_get_context()->private_data;

   filler(buf, ".", NULL, 0);
   filler(buf, "..", NULL, 0);

   /* Read directories */
   gp_list_new(&list);

   gp_camera_folder_list_folders(p->camera, path, list, p->context);

   for (i = 0; i < gp_list_count(list); i++) {
      struct stat *stbuf;
      const char *name;
      gchar *key;

      stbuf = g_new0(struct stat, 1);
      stbuf->st_mode = S_IFDIR | 0755;
      stbuf->st_nlink = 2;

      gp_list_get_name(list, i, &name);
      filler(buf, name, stbuf, 0);

      key = g_build_filename(path, name, NULL);

      g_hash_table_replace(p->dirs, key, stbuf);
   }

   /* Read files */
   gp_list_free(list);
   gp_list_new(&list);

   gp_camera_folder_list_files(p->camera, path, list, p->context);

   for (i = 0; i < gp_list_count(list); i++) {
      struct stat *stbuf;
      const char *name;
      gchar *key;
      CameraFileInfo info;

      gp_list_get_name(list, i, &name);
      gp_camera_file_get_info(p->camera, path, name, &info, p->context);

      stbuf = g_new0(struct stat, 1);
      stbuf->st_mode = S_IFREG | 0444;
      stbuf->st_nlink = 1;
      stbuf->st_size = info.file.size;
      stbuf->st_mtime = info.file.mtime;

      filler(buf, name, stbuf, 0);

      key = g_build_filename(path, name, NULL);

      g_hash_table_replace(p->files, key, stbuf);
   }

   return 0;
}

static int dummyfiller(void *buf,
                       const char *name,
                       const struct stat *stbuf,
                       off_t off)
{
   return 0;
}


static int
gphoto_getattr(const char *path,
               struct stat *stbuf)
{
   int res = 0;

   GPCtx *p = (GPCtx *)fuse_get_context()->private_data;

   memset(stbuf, 0, sizeof(struct stat));

    if(strcmp(path, "/") == 0) {
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
    } else {
        gboolean retryLookup;
        struct stat *mystbuf = NULL;
        gpointer value;

        do {
           retryLookup = FALSE;

           value = g_hash_table_lookup(p->dirs, path);
           if (value) {
              mystbuf = (struct stat *)value;
           } else {
              value = g_hash_table_lookup(p->files, path);
              if (value) {
                 mystbuf = (struct stat *)value;
              } else {
                 gchar *dir = g_path_get_dirname(path);
                 gphoto_readdir(dir, NULL, dummyfiller, 0, NULL);
                 g_free(dir);

                 retryLookup = TRUE;
              }
           }
        } while (retryLookup);

        if (mystbuf) {
           stbuf->st_mode = mystbuf->st_mode;
           stbuf->st_nlink = mystbuf->st_nlink;
           stbuf->st_size = mystbuf->st_size;
           stbuf->st_mtime = mystbuf->st_mtime;
        } else {
           res = -ENOENT;
        }
    }
    return res;
}

static int
gphoto_open(const char *path,
            struct fuse_file_info *fi)
{
   GPCtx *p = (GPCtx *)fuse_get_context()->private_data;
   CameraFile *cFile;

   if((fi->flags & 3) != O_RDONLY) {
      return -EACCES;
   }

   cFile = g_hash_table_lookup(p->reads, path);
   if (!cFile) {
      gchar *dir = g_path_get_dirname(path);
      gchar *file = g_path_get_basename(path);

      gp_file_new(&cFile);
      gp_camera_file_get(p->camera, dir, file, GP_FILE_TYPE_NORMAL,
                         cFile, p->context);

      g_hash_table_replace(p->reads, g_strdup(path), cFile);

      g_free(file);
      g_free(dir);
   }

   return 0;
}

static int
gphoto_read(const char *path,
            char *buf, size_t size,
            off_t offset,
            struct fuse_file_info *fi)
{
   GPCtx *p = (GPCtx *)fuse_get_context()->private_data;
   CameraFile *cFile;
   const char *data;
   unsigned long int dataSize;

   cFile = g_hash_table_lookup(p->reads, path);
   gp_file_get_data_and_size(cFile, &data, &dataSize);

   if (offset < dataSize) {
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
gphoto_release(const char *path,
               struct fuse_file_info *fi)
{
   GPCtx *p = (GPCtx *)fuse_get_context()->private_data;

   g_hash_table_remove(p->reads, path);

   return 0;
}

static int
gphoto_unlink(const char *path)
{
   GPCtx *p = (GPCtx *)fuse_get_context()->private_data;
   gchar *dir = g_path_get_dirname(path);
   gchar *file = g_path_get_basename(path);
   int ret;

   

   ret = gp_camera_file_delete(p->camera, dir, file, p->context);
   if (ret == 0) {
      g_hash_table_remove(p->files, path);
   } else {
      switch (ret) {
      case GP_ERROR_FILE_NOT_FOUND:
         ret = -ENOENT;
         break;
      default:
         ret = -EPROTO;
         break;
      }
   }

   return ret;
}

static void *
gphoto_init(void)
{
   GPCtx *p = g_new0(GPCtx, 1);

   gp_camera_new(&p->camera);
   p->context = gp_context_new();

   p->dirs = g_hash_table_new_full(g_str_hash, g_str_equal,
                                   g_free, g_free);
   p->files = g_hash_table_new_full(g_str_hash, g_str_equal,
                                    g_free, g_free);
   p->reads = g_hash_table_new_full(g_str_hash, g_str_equal,
                                    g_free, (GDestroyNotify)gp_file_unref);

   return p;
}

static void
gphoto_destroy(void *context)
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

static struct fuse_operations gphoto_oper = {
    .init	= gphoto_init,
    .destroy	= gphoto_destroy,
    .readdir	= gphoto_readdir,
    .getattr	= gphoto_getattr,
    .open	= gphoto_open,
    .read	= gphoto_read,
    .release	= gphoto_release,
    .unlink	= gphoto_unlink,
};

int main(int argc, char *argv[])
{
    return fuse_main(argc, argv, &gphoto_oper);
}
