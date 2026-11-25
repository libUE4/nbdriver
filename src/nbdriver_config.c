/* SPDX-License-Identifier: GPL-2.0-only */
/* Configuration-only file proxy. Uses the module installer's immutable creds
 * for a bounded operation, never commits credentials to a host task. */
#include <linux/cred.h>
#include <linux/build_bug.h>
#include <linux/dcache.h>
#include <linux/err.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/mount.h>
#include <linux/mutex.h>
#include <linux/namei.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/user_namespace.h>
#include <linux/version.h>
#include "nbdriver_abi.h"
#include "nbdriver_cfi.h"
#include "nbdriver_config.h"
#include "nbdriver_ksym.h"
#include "nbdriver_log.h"
#include "nbdriver_sf_groups.h"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)
MODULE_IMPORT_NS("VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver");
#else
MODULE_IMPORT_NS(VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver);
#endif

static const struct cred *config_cred;
static DEFINE_MUTEX(config_lock);
static __typeof__(&vfs_rename) config_rename_fn;
static __typeof__(&vfs_unlink) config_unlink_fn;
static __typeof__(&file_open_root) config_open_fn;
static __typeof__(&__put_cred) config_release_cred_fn;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)
static __typeof__(&lookup_one) config_lookup_fn;
#endif

static noinline void NBDRIVER_NO_CFI config_free_cred(struct cred *cred)
{
    READ_ONCE(config_release_cred_fn)(cred);
}

static noinline void NBDRIVER_NO_CFI config_put_cred(const struct cred *value)
{
    struct cred *cred = (struct cred *)value;
    bool last;
    if (!cred)
        return;
#ifdef CONFIG_DEBUG_CREDENTIALS
    validate_creds(cred);
#endif
    BUILD_BUG_ON(!__builtin_types_compatible_p(__typeof__(cred->usage), atomic_t) &&
                 !__builtin_types_compatible_p(__typeof__(cred->usage), atomic_long_t));
    last = __builtin_choose_expr(
        __builtin_types_compatible_p(__typeof__(cred->usage), atomic_long_t),
        atomic_long_dec_and_test((atomic_long_t *)&cred->usage),
        atomic_dec_and_test((atomic_t *)&cred->usage));
    if (last)
        config_free_cred(cred);
}

static bool root_caller(void)
{
    const struct cred *c = current_cred();
    return c->user_ns && !c->user_ns->parent && c->user_ns->level == 0 &&
        uid_eq(c->uid, GLOBAL_ROOT_UID) && uid_eq(c->euid, GLOBAL_ROOT_UID) &&
        uid_eq(c->suid, GLOBAL_ROOT_UID) && uid_eq(c->fsuid, GLOBAL_ROOT_UID);
}

int nbdriver_config_init(void)
{
    /* finit_module runs in the authorized installer/root-manager domain.
     * Capturing it avoids guessing KSU/Magisk SELinux domain names. */
    if (!root_caller())
        return -EACCES;
    config_release_cred_fn = (__typeof__(config_release_cred_fn))
        nbdriver_ksym_lookup_optional("__put_cred");
    if (!config_release_cred_fn)
        return -EOPNOTSUPP;
    config_cred = get_current_cred();
    return 0;
}

void nbdriver_config_exit(void)
{
    if (config_cred) {
        config_put_cred(config_cred);
        config_cred = NULL;
    }
}

static int config_resolve(void)
{
    if (!config_open_fn)
        config_open_fn = (__typeof__(config_open_fn))nbdriver_ksym_lookup_optional("file_open_root");
    if (!config_rename_fn)
        config_rename_fn = (__typeof__(config_rename_fn))nbdriver_ksym_lookup_optional("vfs_rename");
    if (!config_unlink_fn)
        config_unlink_fn = (__typeof__(config_unlink_fn))nbdriver_ksym_lookup_optional("vfs_unlink");
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)
    if (!config_lookup_fn)
        config_lookup_fn = (__typeof__(config_lookup_fn))nbdriver_ksym_lookup_optional("lookup_one");
    if (!config_lookup_fn)
        return -EOPNOTSUPP;
#endif
    return config_open_fn && config_rename_fn && config_unlink_fn ? 0 : -EOPNOTSUPP;
}

/* Raw-symbol boundaries stay typed and isolated; registered ioctl retains CFI. */
static noinline struct file *NBDRIVER_NO_CFI
config_open(struct file *parent, const char *leaf, int flags, umode_t mode)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
    return config_open_fn(&parent->f_path, leaf, flags, mode);
#else
    return config_open_fn(parent->f_path.dentry, parent->f_path.mnt, leaf, flags, mode);
#endif
}

static noinline int NBDRIVER_NO_CFI
config_unlink(struct file *directory, struct dentry *entry)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
    return config_unlink_fn(mnt_idmap(directory->f_path.mnt), file_inode(directory), entry, NULL);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 12, 0)
    return config_unlink_fn(mnt_user_ns(directory->f_path.mnt), file_inode(directory), entry, NULL);
#else
    return config_unlink_fn(file_inode(directory), entry, NULL);
#endif
}

static noinline int NBDRIVER_NO_CFI
config_rename(struct file *directory, struct dentry *old, struct dentry *new)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 12, 0)
    struct renamedata rd = { .old_dentry = old, .new_dentry = new };
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)
    rd.mnt_idmap = mnt_idmap(directory->f_path.mnt);
    rd.old_parent = directory->f_path.dentry;
    rd.new_parent = directory->f_path.dentry;
#else
    rd.old_dir = file_inode(directory);
    rd.new_dir = file_inode(directory);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
    rd.old_mnt_idmap = mnt_idmap(directory->f_path.mnt);
    rd.new_mnt_idmap = mnt_idmap(directory->f_path.mnt);
#else
    rd.old_mnt_userns = mnt_user_ns(directory->f_path.mnt);
    rd.new_mnt_userns = mnt_user_ns(directory->f_path.mnt);
#endif
#endif
    return config_rename_fn(&rd);
#else
    return config_rename_fn(file_inode(directory), old, file_inode(directory), new, NULL, 0);
#endif
}

static noinline struct dentry *NBDRIVER_NO_CFI
config_lookup(struct file *dir, const char *name)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)
    struct qstr q = QSTR_INIT(name, strlen(name));
    return config_lookup_fn(mnt_idmap(dir->f_path.mnt), &q, dir->f_path.dentry);
#else
    return lookup_one_len(name, dir->f_path.dentry, strlen(name));
#endif
}

static bool private_inode(struct inode *inode, bool directory)
{
    return inode && (uid_eq(inode->i_uid, GLOBAL_ROOT_UID) ||
        uid_eq(inode->i_uid, KUIDT_INIT(1000))) &&
        !(inode->i_mode & 0022) &&
        (directory ? S_ISDIR(inode->i_mode) :
         S_ISREG(inode->i_mode) && inode->i_nlink == 1);
}

static struct file *config_directory(void)
{
    struct file *parent, *child;
    const char *const children[] = { "nbg", "state" };
    int i;
    parent = filp_open("/data/misc/wmtrace", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC, 0);
    for (i = 0; !IS_ERR(parent); ++i) {
        if (!private_inode(file_inode(parent), true)) {
            fput(parent);
            return ERR_PTR(-EACCES);
        }
        if (i == ARRAY_SIZE(children))
            return parent;
        child = config_open(parent, children[i], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC, 0);
        fput(parent);
        parent = child;
    }
    return parent;
}

/* All dentries are looked up under the pinned, verified directory inode.
 * The exact temporary inode must still be present when publishing it. */
static int config_change(struct file *dir, const char *name, struct file *temp)
{
    struct dentry *target, *source = NULL;
    struct inode *inode;
    int error;
    error = mnt_want_write(dir->f_path.mnt);
    if (error)
        return error;
    inode_lock_nested(file_inode(dir), I_MUTEX_PARENT);
    target = config_lookup(dir, name);
    if (IS_ERR(target)) {
        error = PTR_ERR(target);
        goto unlock;
    }
    inode = d_inode(target);
    if (inode && !private_inode(inode, false)) {
        error = -EACCES;
        goto target_out;
    }
    if (!temp) {
        error = inode ? config_unlink(dir, target) : 0;
    } else {
        source = temp->f_path.dentry;
        if (source->d_parent != dir->f_path.dentry || d_unhashed(source) ||
            !private_inode(file_inode(temp), false))
            error = -ESTALE;
        else
            error = config_rename(dir, source, target);
    }
target_out:
    dput(target);
unlock:
    inode_unlock(file_inode(dir));
    mnt_drop_write(dir->f_path.mnt);
    return error;
}

static int config_read(struct file *dir, const char *name, void *bytes,
                       struct nbdriver_config_request *r)
{
    struct file *file;
    loff_t pos = 0, size;
    int error = 0;
    r->stage = NBDRIVER_CONFIG_STAGE_OPEN;
    file = config_open(dir, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0);
    if (IS_ERR(file))
        return PTR_ERR(file);
    if (!private_inode(file_inode(file), false)) {
        error = -EACCES;
        goto out;
    }
    r->stage = NBDRIVER_CONFIG_STAGE_TRANSFER;
    size = i_size_read(file_inode(file));
    if (size < 0 || size > r->size) {
        error = -E2BIG;
        goto out;
    }
    while (pos < size) {
        ssize_t n = kernel_read(file, (char *)bytes + pos, size - pos, &pos);
        if (n <= 0) { error = n < 0 ? n : -EIO; goto out; }
    }
    if (i_size_read(file_inode(file)) != size) { error = -ESTALE; goto out; }
    r->transferred = pos;
out:
    fput(file);
    return error;
}

static int config_write(struct file *dir, const char *name, void *bytes,
                        struct nbdriver_config_request *r)
{
    struct file *temp;
    loff_t pos = 0;
    char tmp[64];
    int error, cleanup;
    snprintf(tmp, sizeof(tmp), ".%s.nbdriver-tmp", name);
    r->stage = NBDRIVER_CONFIG_STAGE_OPEN;
    /* Recover only our fixed staging leaf from an interrupted earlier save. */
    error = config_change(dir, tmp, NULL);
    if (error)
        return error;
    temp = config_open(dir, tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
    if (IS_ERR(temp))
        return PTR_ERR(temp);
    if (!private_inode(file_inode(temp), false)) { error = -EACCES; goto out; }
    r->stage = NBDRIVER_CONFIG_STAGE_TRANSFER;
    while (pos < r->size) {
        ssize_t n = kernel_write(temp, (char *)bytes + pos, r->size - pos, &pos);
        if (n <= 0) { error = n < 0 ? n : -EIO; goto out; }
    }
    r->stage = NBDRIVER_CONFIG_STAGE_FILE_SYNC;
    error = vfs_fsync(temp, 0);
    if (error)
        goto out;
    r->stage = NBDRIVER_CONFIG_STAGE_PUBLISH;
    error = config_change(dir, name, temp);
    if (error)
        goto out;
    r->transferred = r->size;
    r->stage = NBDRIVER_CONFIG_STAGE_DIRECTORY_SYNC;
    error = vfs_fsync(dir, 0);
out:
    fput(temp);
    cleanup = config_change(dir, tmp, NULL);
    if (cleanup)
        nbd_warn("stage=config_temp_cleanup error=%d\n", cleanup);
    return error;
}

long nbdriver_config_ioctl(unsigned long argument)
{
    struct nbdriver_config_request r;
    const struct cred *previous;
    struct file *dir;
    void *bytes = NULL;
    const char *name;
    int error;
    if (copy_from_user(&r, (void __user *)argument, sizeof(r)))
        return -EFAULT;
    r.transferred = 0;
    r.stage = NBDRIVER_CONFIG_STAGE_VALIDATE;
    error = -EINVAL;
    if (r.version != NBDRIVER_CONFIG_ABI_VERSION || r.reserved ||
        r.operation > NBDRIVER_CONFIG_DELETE)
        goto result;
    name = r.config == NBDRIVER_CONFIG_DFM ? "preferences.conf" :
           r.config == NBDRIVER_CONFIG_RUST ? "rust-preferences.conf" : NULL;
    if (r.operation == NBDRIVER_CONFIG_QUERY) {
        if (r.config || r.buffer || r.size)
            goto result;
    } else if (!name || r.size > NBDRIVER_CONFIG_MAX ||
        (r.operation == NBDRIVER_CONFIG_DELETE ? r.size || r.buffer : !r.size || !r.buffer)) {
        goto result;
    }
    r.stage = NBDRIVER_CONFIG_STAGE_IDENTITY;
    error = -EACCES;
    if (!root_caller() && !nbdriver_sf_control_caller())
        goto result;
    if (r.operation == NBDRIVER_CONFIG_READ || r.operation == NBDRIVER_CONFIG_WRITE) {
        bytes = kmalloc(r.size, GFP_KERNEL);
        if (!bytes) { error = -ENOMEM; goto result; }
        /* Copy before touching storage or overriding any credentials. */
        if (r.operation == NBDRIVER_CONFIG_WRITE &&
            copy_from_user(bytes, u64_to_user_ptr(r.buffer), r.size)) {
            error = -EFAULT;
            goto result;
        }
    }
    mutex_lock(&config_lock);
    error = config_cred ? config_resolve() : -ENODEV;
    if (error || r.operation == NBDRIVER_CONFIG_QUERY)
        goto unlock;
    previous = override_creds(config_cred);
    r.stage = NBDRIVER_CONFIG_STAGE_DIRECTORY;
    dir = config_directory();
    if (IS_ERR(dir)) {
        error = PTR_ERR(dir);
        if (error == -ENOENT && r.operation == NBDRIVER_CONFIG_DELETE)
            error = 0;
    } else {
        if (r.operation == NBDRIVER_CONFIG_READ)
            error = config_read(dir, name, bytes, &r);
        else if (r.operation == NBDRIVER_CONFIG_WRITE)
            error = config_write(dir, name, bytes, &r);
        else {
            r.stage = NBDRIVER_CONFIG_STAGE_PUBLISH;
            error = config_change(dir, name, NULL);
            if (!error) {
                r.stage = NBDRIVER_CONFIG_STAGE_DIRECTORY_SYNC;
                error = vfs_fsync(dir, 0);
            }
        }
        fput(dir);
    }
    revert_creds(previous);
    if (!error && r.operation == NBDRIVER_CONFIG_READ &&
        copy_to_user(u64_to_user_ptr(r.buffer), bytes, r.transferred)) {
        r.stage = NBDRIVER_CONFIG_STAGE_COPY;
        error = -EFAULT;
    }
unlock:
    mutex_unlock(&config_lock);
result:
    kfree(bytes);
    if (!error)
        r.stage = 0;
    else
        nbd_warn("stage=config operation=%u config=%u step=%u error=%d\n",
                 r.operation, r.config, r.stage, error);
    if (copy_to_user((void __user *)argument, &r, sizeof(r)))
        return -EFAULT;
    return error;
}
