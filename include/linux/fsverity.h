/* SPDX-License-Identifier: GPL-2.0 */
/*
 * fs-verity: read-only file-based authenticity protection
 *
 * This header declares the interface between the fs/verity/ support layer and
 * filesystems that support fs-verity.
 *
 * Copyright 2019 Google LLC
 */

#ifndef _LINUX_FSVERITY_H
#define _LINUX_FSVERITY_H

#include <linux/fs.h>
#include <crypto/hash_info.h>
#include <crypto/sha2.h>
#include <uapi/linux/fsverity.h>

/*
 * Largest digest size among all hash algorithms supported by fs-verity.
 * Currently assumed to be <= size of fsverity_descriptor::root_hash.
 */
#define FS_VERITY_MAX_DIGEST_SIZE	SHA512_DIGEST_SIZE

/* Arbitrary limit to bound the kmalloc() size.  Can be changed. */
#define FS_VERITY_MAX_DESCRIPTOR_SIZE	16384

/* Verity operations for filesystems */
struct fsverity_operations {

	/**
	 * Begin enabling verity on the given file.
	 *
	 * @filp: a readonly file descriptor for the file
	 *
	 * The filesystem must do any needed filesystem-specific preparations
	 * for enabling verity, e.g. evicting inline data.  It also must return
	 * -EBUSY if verity is already being enabled on the given file.
	 *
	 * i_rwsem is held for write.
	 *
	 * Return: 0 on success, -errno on failure
	 */
	int (*begin_enable_verity)(struct file *filp);

	/**
	 * End enabling verity on the given file.
	 *
	 * @filp: a readonly file descriptor for the file
	 * @desc: the verity descriptor to write, or NULL on failure
	 * @desc_size: size of verity descriptor, or 0 on failure
	 * @merkle_tree_size: total bytes the Merkle tree took up
	 *
	 * If desc == NULL, then enabling verity failed and the filesystem only
	 * must do any necessary cleanups.  Else, it must also store the given
	 * verity descriptor to a fs-specific location associated with the inode
	 * and do any fs-specific actions needed to mark the inode as a verity
	 * inode, e.g. setting a bit in the on-disk inode.  The filesystem is
	 * also responsible for setting the S_VERITY flag in the VFS inode.
	 *
	 * i_rwsem is held for write, but it may have been dropped between
	 * ->begin_enable_verity() and ->end_enable_verity().
	 *
	 * Return: 0 on success, -errno on failure
	 */
	int (*end_enable_verity)(struct file *filp, const void *desc,
				 size_t desc_size, u64 merkle_tree_size);

	/**
	 * Get the verity descriptor of the given inode.
	 *
	 * @inode: an inode with the S_VERITY flag set
	 * @buf: buffer in which to place the verity descriptor
	 * @bufsize: size of @buf, or 0 to retrieve the size only
	 *
	 * If bufsize == 0, then the size of the verity descriptor is returned.
	 * Otherwise the verity descriptor is written to 'buf' and its actual
	 * size is returned; -ERANGE is returned if it's too large.  This may be
	 * called by multiple processes concurrently on the same inode.
	 *
	 * Return: the size on success, -errno on failure
	 */
	int (*get_verity_descriptor)(struct inode *inode, void *buf,
				     size_t bufsize);

	/**
	 * Read a Merkle tree page of the given inode.
	 *
	 * @inode: the inode
	 * @index: 0-based index of the page within the Merkle tree
	 * @num_ra_pages: The number of Merkle tree pages that should be
	 *		  prefetched starting at @index if the page at @index
	 *		  isn't already cached.  Implementations may ignore this
	 *		  argument; it's only a performance optimization.
	 *
	 * This can be called at any time on an open verity file.  It may be
	 * called by multiple processes concurrently, even with the same page.
	 *
	 * Note that this must retrieve a *page*, not necessarily a *block*.
	 *
	 * Return: the page on success, ERR_PTR() on failure
	 */
	struct page *(*read_merkle_tree_page)(struct inode *inode,
					      pgoff_t index,
					      unsigned long num_ra_pages);

	/**
	 * Write a Merkle tree block to the given inode.
	 *
	 * @inode: the inode for which the Merkle tree is being built
	 * @buf: the Merkle tree block to write
	 * @pos: the position of the block in the Merkle tree (in bytes)
	 * @size: the Merkle tree block size (in bytes)
	 *
	 * This is only called between ->begin_enable_verity() and
	 * ->end_enable_verity().
	 *
	 * Return: 0 on success, -errno on failure
	 */
	int (*write_merkle_tree_block)(struct inode *inode, const void *buf,
				       u64 pos, unsigned int size);
};

#ifdef CONFIG_FS_VERITY

static inline struct fsverity_info *fsverity_get_info(const struct inode *inode)
{
	/*
	 * Pairs with the cmpxchg_release() in fsverity_set_info().
	 * I.e., another task may publish ->i_verity_info concurrently,
	 * executing a RELEASE barrier.  We need to use smp_load_acquire() here
	 * to safely ACQUIRE the memory the other task published.
	 */
	return smp_load_acquire(&inode->i_verity_info);
}

/* enable.c */

int fsverity_ioctl_enable(struct file *filp, const void __user *arg);

/* measure.c */

int fsverity_ioctl_measure(struct file *filp, void __user *arg);
int fsverity_get_digest(struct inode *inode,
			u8 digest[FS_VERITY_MAX_DIGEST_SIZE],
			enum hash_algo *alg);

/* open.c */

int __fsverity_file_open(struct inode *inode, struct file *filp);
int __fsverity_prepare_setattr(struct dentry *dentry, struct iattr *attr);
void __fsverity_cleanup_inode(struct inode *inode);

/**
 * fsverity_cleanup_inode() - free the inode's verity info, if present
 * @inode: an inode being evicted
 *
 * Filesystems must call this on inode eviction to free ->i_verity_info.
 */
static inline void fsverity_cleanup_inode(struct inode *inode)
{
	if (inode->i_verity_info)
		__fsverity_cleanup_inode(inode);
}

/* read_metadata.c */

int fsverity_ioctl_read_metadata(struct file *filp, const void __user *uarg);

/* verify.c */

bool fsverity_verify_blocks(struct page *page, unsigned int len,
			    unsigned int offset);
void fsverity_verify_bio(struct bio *bio);
void fsverity_enqueue_verify_work(struct work_struct *work);

#else /* !CONFIG_FS_VERITY */

static inline struct fsverity_info *fsverity_get_info(const struct inode *inode)
{
	return NULL;
}

/* enable.c */

static inline int fsverity_ioctl_enable(struct file *filp,
					const void __user *arg)
{
	return -EOPNOTSUPP;
}

/* measure.c */

static inline int fsverity_ioctl_measure(struct file *filp, void __user *arg)
{
	return -EOPNOTSUPP;
}

static inline int fsverity_get_digest(struct inode *inode,
				      u8 digest[FS_VERITY_MAX_DIGEST_SIZE],
				      enum hash_algo *alg)
{
	return -EOPNOTSUPP;
}

/* open.c */

static inline int __fsverity_file_open(struct inode *inode, struct file *filp)
{
	return -EOPNOTSUPP;
}

static inline int __fsverity_prepare_setattr(struct dentry *dentry,
					     struct iattr *attr)
{
	return -EOPNOTSUPP;
}

static inline void fsverity_cleanup_inode(struct inode *inode)
{
}

/* read_metadata.c */

static inline int fsverity_ioctl_read_metadata(struct file *filp,
					       const void __user *uarg)
{
	return -EOPNOTSUPP;
}

/* verify.c */

static inline bool fsverity_verify_blocks(struct page *page, unsigned int len,
					  unsigned int offset)
{
	WARN_ON(1);
	return false;
}

static inline void fsverity_verify_bio(struct bio *bio)
{
	WARN_ON(1);
}

static inline void fsverity_enqueue_verify_work(struct work_struct *work)
{
	WARN_ON(1);
}

#endif	/* !CONFIG_FS_VERITY */

static inline bool fsverity_verify_page(struct page *page)
{
	return fsverity_verify_blocks(page, PAGE_SIZE, 0);
}

/**
 * fsverity_active() - do reads from the inode need to go through fs-verity?
 * @inode: inode to check
 *
 * This checks whether ->i_verity_info has been set.
 *
 * Filesystems call this from ->readahead() to check whether the pages need to
 * be verified or not.  Don't use IS_VERITY() for this purpose; it's subject to
 * a race condition where the file is being read concurrently with
 * FS_IOC_ENABLE_VERITY completing.  (S_VERITY is set before ->i_verity_info.)
 *
 * Return: true if reads need to go through fs-verity, otherwise false
 */
static inline bool fsverity_active(const struct inode *inode)
{
	return fsverity_get_info(inode) != NULL;
}

/**
 * fsverity_file_open() - prepare to open a verity file
 * @inode: the inode being opened
 * @filp: the struct file being set up
 *
 * When opening a verity file, deny the open if it is for writing.  Otherwise,
 * set up the inode's ->i_verity_info if not already done.
 *
 * When combined with fscrypt, this must be called after fscrypt_file_open().
 * Otherwise, we won't have the key set up to decrypt the verity metadata.
 *
 * Return: 0 on success, -errno on failure
 */
static inline int fsverity_file_open(struct inode *inode, struct file *filp)
{
	if (IS_VERITY(inode))
		return __fsverity_file_open(inode, filp);
	return 0;
}

/**
 * fsverity_prepare_setattr() - prepare to change a verity inode's attributes
 * @dentry: dentry through which the inode is being changed
 * @attr: attributes to change
 *
 * Verity files are immutable, so deny truncates.  This isn't covered by the
 * open-time check because sys_truncate() takes a path, not a file descriptor.
 *
 * Return: 0 on success, -errno on failure
 */
static inline int fsverity_prepare_setattr(struct dentry *dentry,
					   struct iattr *attr)
{
	if (IS_VERITY(d_inode(dentry)))
		return __fsverity_prepare_setattr(dentry, attr);
	return 0;
}

#endif	/* _LINUX_FSVERITY_H */
