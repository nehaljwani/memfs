#define FUSE_USE_VERSION 26
#define _GNU_SOURCE

#include <sys/types.h>
#include <sys/mman.h>

#include <endian.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <fuse.h>
#include <iconv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>

#define MEMFS_DIRECTORY 0
#define MEMFS_FILE 1

char *
basename (const char *name)
{
    const char *base = name;

    while (*name) {
        if (*name++ == '/')
            base = name;
    }
    return (char *) base;
}

char *
ubasename (const char *name)
{
    const char *base = name;
    const char *beg = name;

    while (*name) {
        if (*name++ == '/')
            base = name;
    }
    char *res = strndup(beg, base - beg);
    if (strlen(res) == 1)
        res[strlen(res) - 1] = '/';
    else if (res[strlen(res) - 1] == '/')
        res[strlen(res) - 1] = '\0';
    return res;
}

struct memFS_directory {
    char *name;
    struct memFS_entry *entries;
    int nentries;
};

struct memFS_file {
    char *name;
    char *data;
    size_t size;
};

struct memFS_entry {
    int type;
    union {
        struct memFS_directory fs_dir;
        struct memFS_file fs_file;
    } element;
    struct stat st;
};

struct memFS {
	const char *dev;
	int fs;
        struct memFS_entry *root;
};

struct memFS_search_data {
	const char *name;
	int found;
	struct stat *st;
};

struct memFS memFS_info, *f = &memFS_info;

uid_t mount_uid;
gid_t mount_gid;
time_t mount_time;
size_t memFS_inode = 0;

size_t
memFS_copy_file_contents(char *file, char **contents) {
    size_t fileSize = 0;

    //Open the stream. Note "b" to avoid DOS/UNIX new line conversion.
    FILE *stream = fopen(file, "rb");

    //Steak to the end of the file to determine the file size
    fseek(stream, 0L, SEEK_END);
    fileSize = ftell(stream);
    fseek(stream, 0L, SEEK_SET);

    //Allocate enough memory (add 1 for the \0, since fread won't add it)
    *contents = malloc(fileSize+1);

    //Read the file
    fread(*contents,fileSize,1,stream);
    *(*contents + fileSize) = '\0'; // Add terminating zero.

    //Print it again for debugging
    printf("%s\n", *contents);

    // Close the file
    fclose(stream);
    return fileSize;
}

static void
memFS_fill(const char *name, int level, struct memFS_directory *parent)
{
    DIR *dir;
    struct dirent *entry;
    char *actualpath = strcat(realpath(f->dev, NULL), name);

    if (!(dir = opendir(actualpath)))
        return;
    if (!(entry = readdir(dir)))
        return;

    do {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
           continue;

        char path[1024];
        int len = snprintf(path, sizeof(path)-1, "%s/%s", name, entry->d_name);
        path[len] = 0;
        actualpath = strcat(realpath(f->dev, NULL), path);
        parent->nentries++;
        parent->entries = (struct memFS_entry *)realloc(parent->entries, sizeof(struct memFS_entry) * parent->nentries);

        if (entry->d_type == DT_DIR) {

            printf("%*s[%s]\n", level*2, "", path + 1);

            parent->entries[parent->nentries - 1].type = MEMFS_DIRECTORY;
            parent->entries[parent->nentries - 1].element.fs_dir.name = strdup(path + 1);
            parent->entries[parent->nentries - 1].element.fs_dir.nentries = 0;
            parent->entries[parent->nentries - 1].element.fs_dir.entries = NULL;
            lstat(actualpath, &parent->entries[parent->nentries - 1].st);

            memFS_fill(path, level + 1, &parent->entries[parent->nentries - 1].element.fs_dir);
        }
        else {
            printf("%*s- %s\n", level*2, "", path + 1);

            parent->entries[parent->nentries - 1].type = MEMFS_FILE;
            parent->entries[parent->nentries - 1].element.fs_file.name = strdup(path + 1);
            lstat(actualpath, &parent->entries[parent->nentries - 1].st);
            parent->entries[parent->nentries - 1].element.fs_file.size =
            memFS_copy_file_contents(actualpath, &parent->entries[parent->nentries - 1].element.fs_file.data);
        }
        parent->entries[parent->nentries - 1].st.st_ino = memFS_inode++;
    } while ((entry = readdir(dir)));
    closedir(dir);
}

static void
memFS_init(const char *dev)
{
        printf("memFS_init(%s)\n", dev);
	mount_uid = getuid();
	mount_gid = getgid();
	mount_time = time(NULL);

	f->fs = open(dev, O_RDONLY);
	if (f->fs < 0)
		err(1, "open(%s)", dev);

        /* Source point will be a directory */
        f->root = (struct memFS_entry *)malloc(sizeof(struct memFS_entry));
        f->root->type = MEMFS_DIRECTORY;
        f->root->element.fs_dir.name = strdup("/");
        f->root->element.fs_dir.nentries = 0;
        f->root->element.fs_dir.entries = NULL;
        lstat(realpath(dev, NULL), &f->root->st);
        f->root->st.st_ino = memFS_inode++;

        memFS_fill("/", 0, &f->root->element.fs_dir);
}

struct memFS_entry *
memFS_search_entry(const char *name, struct memFS_entry *entry)
{
        int i;
        if (entry->type == MEMFS_DIRECTORY) {
            if (strcmp(name, entry->element.fs_dir.name) == 0)
                return entry;
            for (i = 0; i < entry->element.fs_dir.nentries; i++) {
                struct memFS_entry * result =
                    memFS_search_entry(name, &entry->element.fs_dir.entries[i]);
                if(result)
                    return result;
            }
        }
        else {
            if (strcmp(name, entry->element.fs_file.name) == 0)
                return entry;
        }
        return NULL;
}

static int
memFS_fuse_getattr(const char *path, struct stat *st)
{
        printf("getattr(path=\"%s\")\n", path);
        struct memFS_entry *entry = memFS_search_entry(path, f->root);
        if (entry) {
            *st = entry->st;
            return 0;
        }
        return -1;
}

static int
memFS_fuse_readdir(const char *path, void *data,
		  fuse_fill_dir_t filler, off_t offs, struct fuse_file_info *fi)
{
        printf("readdir(path=\"%s\")\n", path);
        struct memFS_entry *entry = memFS_search_entry(path, f->root);
        int i;
        if (entry) {
            for (i = 0; i < entry->element.fs_dir.nentries; i++) {
                struct memFS_entry *entry_tmp = &entry->element.fs_dir.entries[i];
                if ( entry_tmp->type == MEMFS_DIRECTORY) {
                    if (filler(data, basename(entry_tmp->element.fs_dir.name), NULL, 0) != 0)
                        return -ENOMEM;
                }
                else
                    if (filler(data, basename(entry_tmp->element.fs_file.name), NULL, 0) != 0)
                        return -ENOMEM;
            }
            return 0;
        }
        return -1;
}

static int
memFS_fuse_read(const char *path, char *buf, size_t size, off_t offs,
	       struct fuse_file_info *fi)
{
        printf("read(path=\"%s\")\n", path);
        struct memFS_entry *entry = memFS_search_entry(path, f->root);
        if (entry) {
            char *data = entry->element.fs_file.data;
            strncpy(buf, data + offs, size);
            return size;
        }
        return -1;
}

int memFS_fuse_open(const char *path, struct fuse_file_info *fi)
{
        printf("open(path=\"%s\")\n", path);
        struct memFS_entry *entry = memFS_search_entry(path, f->root);
        if (entry) {
            return 0;
        }
        return -1;
}

int memFS_fuse_releasedir(const char *path, struct fuse_file_info *fi)
{
        printf("releasedir(path=\"%s\")\n", path);
        struct memFS_entry *entry = memFS_search_entry(path, f->root);
        if (entry) {
            return 0;
        }
        return -1;
}

int memFS_fuse_opendir(const char *path, struct fuse_file_info *fi)
{
        printf("opendir(path=\"%s\")\n", path);
        struct memFS_entry *entry = memFS_search_entry(path, f->root);
        if (entry) {
            return 0;
        }
        return -1;
}


int memFS_fuse_access(const char *path, int mask)
{
        printf("access(path=\"%s\"), mask=0%o\n", path, mask);
        struct memFS_entry *entry = memFS_search_entry(path, f->root);
        if (entry) {
            /* One should check for proper permissions here using entry->st.st_mode
             * and mask*/
                return 0;
        }
        return -1;
}

int memFS_fuse_write(const char *path, const char *buf, size_t size, off_t offset,
             struct fuse_file_info *fi)
{
        printf("write(path=\"%s\")\n", path);
        /* Option 1: Create file in /tmp, perform syscalls and then revert it back to memory
         * Option 2: Modify in the memory itself, change the appropriate stat thingy
         * Option 3: Create a new file in memory, unlink the old file and replace it with the new one */
        return 0;
}

int memFS_fuse_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    printf("create(path=\"%s\")\n", path);
    char *parentpath = ubasename(path);
    struct memFS_entry *entry = memFS_search_entry(path, f->root);
    struct memFS_entry *entry_ = memFS_search_entry(parentpath, f->root);
    if (entry)
        return -1;
    else {
        struct memFS_directory *parent = &entry_->element.fs_dir;
        parent->nentries++;
        parent->entries = (struct memFS_entry *)realloc(parent->entries, sizeof(struct memFS_entry) * parent->nentries);
        parent->entries[parent->nentries - 1].type = MEMFS_FILE;
        parent->entries[parent->nentries - 1].element.fs_file.name = strdup(path);
        parent->entries[parent->nentries - 1].element.fs_file.size = 0;
        struct stat *st_ = &parent->entries[parent->nentries - 1].st;
        st_->st_dev = 2054;
        st_->st_ino =  memFS_inode++;
        st_->st_mode = mode;
        st_->st_nlink =  1;
        st_->st_uid =  getuid();
        st_->st_gid =  getgid();
        st_->st_rdev =  0;
        st_->st_size =  0;
        st_->st_blksize =  4096;
        st_->st_blocks =  0;
        st_->st_atime =  time(NULL);
        st_->st_mtime =  time(NULL);
        st_->st_ctime =  time(NULL);
        return 0;
    }
}

int memFS_fuse_mkdir(const char *path, mode_t mode)
{
    printf("mkdir(path=\"%s\")\n", path);
    char *parentpath = ubasename(path);
    struct memFS_entry *entry = memFS_search_entry(path, f->root);
    struct memFS_entry *entry_ = memFS_search_entry(parentpath, f->root);
    if (!entry && entry_) {
        struct memFS_directory *parent = &entry_->element.fs_dir;
        parent->nentries++;
        parent->entries = (struct memFS_entry *)realloc(parent->entries, sizeof(struct memFS_entry) * parent->nentries);
        parent->entries[parent->nentries - 1].type = MEMFS_DIRECTORY;
        parent->entries[parent->nentries - 1].element.fs_dir.name = strdup(path);
        parent->entries[parent->nentries - 1].element.fs_dir.nentries = 0;
        parent->entries[parent->nentries - 1].element.fs_dir.entries = NULL;
        struct stat *st_ = &parent->entries[parent->nentries - 1].st;
        st_->st_dev = 2054;
        st_->st_ino =  memFS_inode++;
        st_->st_mode = mode;
        st_->st_nlink =  3;
        st_->st_uid =  getuid();
        st_->st_gid =  getgid();
        st_->st_rdev =  0;
        st_->st_size =  4096;
        st_->st_blksize =  4096;
        st_->st_blocks =  8;
        st_->st_atime =  time(NULL);
        st_->st_mtime =  time(NULL);
        st_->st_ctime =  time(NULL);
       return 0;
    }
    return -1;
}

int memFS_fuse_truncate(const char *path, off_t newsize)
{
    printf("truncate(path=\"%s\")\n", path);
    struct memFS_entry *entry = memFS_search_entry(path, f->root);
    if (entry) {
        entry->st.st_size = newsize;
        char *olddata = entry->element.fs_file.data;
        entry->element.fs_file.data = strndup(olddata, newsize);
        free(olddata);
        entry->st.st_atime = time(NULL);
        entry->st.st_mtime = time(NULL);
        entry->st.st_ctime = time(NULL);
        return 0;
    }
    return -1;
}

int memFS_fuse_rename(const char *path, const char *newpath)
{
    printf("rename(path=\"%s\", newpath=\"%s\")\n", path, newpath);
    struct memFS_entry *entry = memFS_search_entry(path, f->root);
    char *parentpath = ubasename(path);
    struct memFS_entry *entry_ = memFS_search_entry(parentpath, f->root);
    if (entry && entry_) {
        char *oldname = entry->element.fs_dir.name;
        entry->element.fs_dir.name = strdup(newpath);
        free(oldname);
        return 0;
    }
    return -1;

}

int memFS_fuse_rmdir(const char *path)
{
    printf("rmdir(path=\"%s\")\n", path);
    struct memFS_entry *entry = memFS_search_entry(path, f->root);
    char *parentpath = ubasename(path);
    struct memFS_entry *entry_ = memFS_search_entry(parentpath, f->root);
    if (entry && entry->type == MEMFS_DIRECTORY) {
        struct memFS_directory *parent = &entry_->element.fs_dir;
        if (entry->element.fs_dir.nentries == 0) {
            int i;
            for (i = 0; i < parent->nentries; i++) {
                if (parent->entries[i].type == MEMFS_DIRECTORY
                    && (strcmp(parent->entries[i].element.fs_dir.name,
                           entry->element.fs_dir.name) == 0)) {
                    /* One should free this memory, by creating array of entries again (without the deleted one)
                     * and manipulating pointers*/
                    break;
                }
            }
            while (i < parent->nentries) {
                parent->entries[i] = parent->entries[i + 1];
                i++;
            }
            parent->nentries--;
            return 0;
        }
        errno = EEXIST;
    }
    return -1;
}

int memFS_fuse_unlink(const char *path)
{
    printf("unlink(path=\"%s\")\n", path);
    struct memFS_entry *entry = memFS_search_entry(path, f->root);
    char *parentpath = ubasename(path);
    struct memFS_entry *entry_ = memFS_search_entry(parentpath, f->root);
    if (entry && entry->type == MEMFS_FILE) {
        struct memFS_directory *parent = &entry_->element.fs_dir;
        int i;
        printf("%s\n", parent->name);
        for (i = 0; i < parent->nentries; i++) {
            if (parent->entries[i].type == MEMFS_FILE
                && (strcmp(parent->entries[i].element.fs_file.name,
                           entry->element.fs_file.name) == 0)) {
                /* One should free this memory, by creating array of entries again (without the deleted one)
                 * and manipulating pointers*/
                break;
            }
        }
        while (i < parent->nentries) {
            parent->entries[i] = parent->entries[i + 1];
            i++;
        }
        parent->nentries--;
        return 0;
    }
    return -1;
}

static int
memFS_opt_args(void *data, const char *arg, int key, struct fuse_args *oargs)
{
	if (key == FUSE_OPT_KEY_NONOPT && !f->dev) {
		f->dev = strdup(arg);
		return (0);
	}
	return (1);
}

static struct fuse_operations memFS_ops = {
	.readdir = memFS_fuse_readdir,          //Working
	.opendir = memFS_fuse_opendir,          //Working
	.releasedir = memFS_fuse_releasedir,    //Working
	.mkdir = memFS_fuse_mkdir,              // :'(
	.rmdir = memFS_fuse_rmdir,              //Working
	.read = memFS_fuse_read,                //Working
        .open = memFS_fuse_open,                //Working
        .create = memFS_fuse_create,            // :('
        .write = memFS_fuse_write,              //Not implemented
        .truncate = memFS_fuse_truncate,        // ???
        .access = memFS_fuse_access,            //Working
        .getattr = memFS_fuse_getattr,          //Working, but creating problems ?
        .rename = memFS_fuse_rename,            //Can't say ^^
        .unlink = memFS_fuse_unlink,            //Working
//	.statfs = memFS_fuse_statfs,
//      .symlink = memFS_fuse_symlink,
//      .link = memFS_fuse_link,
};

int
main(int argc, char **argv)
{
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

	fuse_opt_parse(&args, NULL, NULL, memFS_opt_args);

	if (!f->dev)
		errx(1, "missing file system parameter");

	memFS_init(f->dev);
	return (fuse_main(args.argc, args.argv, &memFS_ops, NULL));
}
