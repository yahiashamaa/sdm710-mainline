// SPDX-License-Identifier: GPL-2.0
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/file.h>
#include <linux/slab.h>
#include <linux/nospec.h>
#include <linux/io_uring.h>

#include <uapi/linux/io_uring.h>

#include "io_uring.h"
#include "rsrc.h"
#include "filetable.h"
#include "msg_ring.h"


/* All valid masks for MSG_RING */
#define IORING_MSG_RING_MASK		(IORING_MSG_RING_CQE_SKIP | \
					IORING_MSG_RING_FLAGS_PASS)

struct io_msg {
	struct file			*file;
	struct file			*src_file;
	struct callback_head		tw;
	u64 user_data;
	u32 len;
	u32 cmd;
	u32 src_fd;
	union {
		u32 dst_fd;
		u32 cqe_flags;
	};
	u32 flags;
};

void io_msg_ring_cleanup(struct io_kiocb *req)
{
	struct io_msg *msg = io_kiocb_to_cmd(req, struct io_msg);

	if (WARN_ON_ONCE(!msg->src_file))
		return;

	fput(msg->src_file);
	msg->src_file = NULL;
}

static void io_msg_tw_complete(struct callback_head *head)
{
	struct io_msg *msg = container_of(head, struct io_msg, tw);
	struct io_kiocb *req = cmd_to_io_kiocb(msg);
	struct io_ring_ctx *target_ctx = req->file->private_data;
	int ret = 0;

	if (current->flags & PF_EXITING)
		ret = -EOWNERDEAD;
	else if (!io_post_aux_cqe(target_ctx, msg->user_data, msg->len, 0))
		ret = -EOVERFLOW;

	if (ret < 0)
		req_set_fail(req);
	io_req_queue_tw_complete(req, ret);
}

static int io_msg_ring_data(struct io_kiocb *req)
{
	struct io_ring_ctx *target_ctx = req->file->private_data;
	struct io_msg *msg = io_kiocb_to_cmd(req, struct io_msg);
	u32 flags = 0;

	if (msg->src_fd || msg->flags & ~IORING_MSG_RING_FLAGS_PASS)
		return -EINVAL;

	if (!(msg->flags & IORING_MSG_RING_FLAGS_PASS) && msg->dst_fd)
		return -EINVAL;

	if (msg->flags & IORING_MSG_RING_FLAGS_PASS)
		flags = msg->cqe_flags;

	if (target_ctx->task_complete && current != target_ctx->submitter_task) {
		init_task_work(&msg->tw, io_msg_tw_complete);
		if (task_work_add(target_ctx->submitter_task, &msg->tw,
				  TWA_SIGNAL_NO_IPI))
			return -EOWNERDEAD;

		atomic_or(IORING_SQ_TASKRUN, &target_ctx->rings->sq_flags);
		return IOU_ISSUE_SKIP_COMPLETE;
	}

	if (io_post_aux_cqe(target_ctx, msg->user_data, msg->len, flags))
		return 0;

	return -EOVERFLOW;
}

static void io_double_unlock_ctx(struct io_ring_ctx *octx,
				 unsigned int issue_flags)
{
	mutex_unlock(&octx->uring_lock);
}

static int io_double_lock_ctx(struct io_ring_ctx *octx,
			      unsigned int issue_flags)
{
	/*
	 * To ensure proper ordering between the two ctxs, we can only
	 * attempt a trylock on the target. If that fails and we already have
	 * the source ctx lock, punt to io-wq.
	 */
	if (!(issue_flags & IO_URING_F_UNLOCKED)) {
		if (!mutex_trylock(&octx->uring_lock))
			return -EAGAIN;
		return 0;
	}
	mutex_lock(&octx->uring_lock);
	return 0;
}

static struct file *io_msg_grab_file(struct io_kiocb *req, unsigned int issue_flags)
{
	struct io_msg *msg = io_kiocb_to_cmd(req, struct io_msg);
	struct io_ring_ctx *ctx = req->ctx;
	struct file *file = NULL;
	unsigned long file_ptr;
	int idx = msg->src_fd;

	io_ring_submit_lock(ctx, issue_flags);
	if (likely(idx < ctx->nr_user_files)) {
		idx = array_index_nospec(idx, ctx->nr_user_files);
		file_ptr = io_fixed_file_slot(&ctx->file_table, idx)->file_ptr;
		file = (struct file *) (file_ptr & FFS_MASK);
		if (file)
			get_file(file);
	}
	io_ring_submit_unlock(ctx, issue_flags);
	return file;
}

static int io_msg_install_complete(struct io_kiocb *req, unsigned int issue_flags)
{
	struct io_ring_ctx *target_ctx = req->file->private_data;
	struct io_msg *msg = io_kiocb_to_cmd(req, struct io_msg);
	struct file *src_file = msg->src_file;
	int ret;

	if (unlikely(io_double_lock_ctx(target_ctx, issue_flags)))
		return -EAGAIN;

	ret = __io_fixed_fd_install(target_ctx, src_file, msg->dst_fd);
	if (ret < 0)
		goto out_unlock;

	msg->src_file = NULL;
	req->flags &= ~REQ_F_NEED_CLEANUP;

	if (msg->flags & IORING_MSG_RING_CQE_SKIP)
		goto out_unlock;
	/*
	 * If this fails, the target still received the file descriptor but
	 * wasn't notified of the fact. This means that if this request
	 * completes with -EOVERFLOW, then the sender must ensure that a
	 * later IORING_OP_MSG_RING delivers the message.
	 */
	if (!io_post_aux_cqe(target_ctx, msg->user_data, msg->len, 0))
		ret = -EOVERFLOW;
out_unlock:
	io_double_unlock_ctx(target_ctx, issue_flags);
	return ret;
}

static void io_msg_tw_fd_complete(struct callback_head *head)
{
	struct io_msg *msg = container_of(head, struct io_msg, tw);
	struct io_kiocb *req = cmd_to_io_kiocb(msg);
	int ret = -EOWNERDEAD;

	if (!(current->flags & PF_EXITING))
		ret = io_msg_install_complete(req, IO_URING_F_UNLOCKED);
	if (ret < 0)
		req_set_fail(req);
	io_req_queue_tw_complete(req, ret);
}

static int io_msg_send_fd(struct io_kiocb *req, unsigned int issue_flags)
{
	struct io_ring_ctx *target_ctx = req->file->private_data;
	struct io_msg *msg = io_kiocb_to_cmd(req, struct io_msg);
	struct io_ring_ctx *ctx = req->ctx;
	struct file *src_file = msg->src_file;

	if (target_ctx == ctx)
		return -EINVAL;
	if (!src_file) {
		src_file = io_msg_grab_file(req, issue_flags);
		if (!src_file)
			return -EBADF;
		msg->src_file = src_file;
		req->flags |= REQ_F_NEED_CLEANUP;
	}

	if (target_ctx->task_complete && current != target_ctx->submitter_task) {
		init_task_work(&msg->tw, io_msg_tw_fd_complete);
		if (task_work_add(target_ctx->submitter_task, &msg->tw,
				  TWA_SIGNAL))
			return -EOWNERDEAD;

		return IOU_ISSUE_SKIP_COMPLETE;
	}
	return io_msg_install_complete(req, issue_flags);
}

int io_msg_ring_prep(struct io_kiocb *req, const struct io_uring_sqe *sqe)
{
	struct io_msg *msg = io_kiocb_to_cmd(req, struct io_msg);

	if (unlikely(sqe->buf_index || sqe->personality))
		return -EINVAL;

	msg->src_file = NULL;
	msg->user_data = READ_ONCE(sqe->off);
	msg->len = READ_ONCE(sqe->len);
	msg->cmd = READ_ONCE(sqe->addr);
	msg->src_fd = READ_ONCE(sqe->addr3);
	msg->dst_fd = READ_ONCE(sqe->file_index);
	msg->flags = READ_ONCE(sqe->msg_ring_flags);
	if (msg->flags & ~IORING_MSG_RING_MASK)
		return -EINVAL;

	return 0;
}

int io_msg_ring(struct io_kiocb *req, unsigned int issue_flags)
{
	struct io_msg *msg = io_kiocb_to_cmd(req, struct io_msg);
	int ret;

	ret = -EBADFD;
	if (!io_is_uring_fops(req->file))
		goto done;

	switch (msg->cmd) {
	case IORING_MSG_DATA:
		ret = io_msg_ring_data(req);
		break;
	case IORING_MSG_SEND_FD:
		ret = io_msg_send_fd(req, issue_flags);
		break;
	default:
		ret = -EINVAL;
		break;
	}

done:
	if (ret < 0) {
		if (ret == -EAGAIN || ret == IOU_ISSUE_SKIP_COMPLETE)
			return ret;
		req_set_fail(req);
	}
	io_req_set_res(req, ret, 0);
	return IOU_OK;
}
