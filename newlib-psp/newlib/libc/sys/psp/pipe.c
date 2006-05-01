/*
 * PSP Software Development Kit - http://www.pspdev.org
 * -----------------------------------------------------------------------
 * Licensed under the BSD license, see LICENSE in PSPSDK root for details.
 *
 * pipe.c - Socket wrappers to provide similar functions to normal unix
 *
 * Copyright (c) 2006 Rafael Cabezas <rafpsp@gmail.com>
 * 
 */
#include <stdio.h>
#include <errno.h>
#include <sys/syslimits.h>
#include <sys/fd_set.h>

#include <psptypes.h>
#include <pspthreadman.h>
#include <pspmodulemgr.h>
#include <pspkerror.h>
#include "fdman.h"

/* Pipe functions */
/* Returns how many bytes are in the pipe -- waiting to be read */
size_t __psp_pipe_peekmsgsize(int fd)
{
	SceKernelMppInfo info;
	info.size = sizeof(info);
	if (sceKernelReferMsgPipeStatus(__psp_descriptormap[fd]->sce_descriptor, &info) == 0) {
		return (info.bufSize - info.freeSize);
	}
	else {
		return -1;
	}
}

int pipe(int fildes[2])
{
	static int iIndex = 0;
	char name[32];
	iIndex++;

	sprintf(name, "__psp_pipe_%02d", iIndex);

	SceUID uid = sceKernelCreateMsgPipe(name, PSP_MEMORY_PARTITION_USER, 0, (void *)PIPE_BUF, NULL);

	if (uid >= 0) {
		fildes[0] = __psp_fdman_get_new_descriptor();
		if (fildes[0] != -1) {
			__psp_descriptormap[fildes[0]]->sce_descriptor = uid;
			__psp_descriptormap[fildes[0]]->type = __PSP_DESCRIPTOR_TYPE_PIPE;
		} 
		else {
			sceKernelDeleteMsgPipe(uid);
			errno = EFAULT;
			return -1;
		}
		
		fildes[1] = __psp_fdman_get_dup_descriptor(fildes[0]);
		if (fildes[1] != -1) {
			__psp_descriptormap[fildes[1]]->sce_descriptor = uid;
			return 0;
		} 
		else {
			sceKernelDeleteMsgPipe(uid);
			errno = EFAULT;
			return -1;
		}
		
	}
	else {
		errno = EFAULT;
		return -1;
	}
}

int __psp_pipe_close(int fd)
{
	int ret = 0;

	if( fd < 0 || fd > (__PSP_FILENO_MAX - 1) || (__psp_descriptormap[fd]->type != __PSP_DESCRIPTOR_TYPE_PIPE) ) {
		errno = EBADF;
		return -1;
	}
	
		
	if ( __psp_descriptormap[fd]->ref_count == 1 ) {
			/**
			* Delete a message pipe
			*
			* @param uid - The UID of the pipe
			*
			* @return 0 on success, < 0 on error
			*/
			ret = sceKernelDeleteMsgPipe(__psp_descriptormap[fd]->sce_descriptor);
	}
	
	__psp_fdman_release_descriptor(fd);
			
	if(ret < 0) {
		return __psp_set_errno(ret);
	}
	
	return 0;
}

int __psp_pipe_nonblocking_read(int fd, void *buf, size_t len)
{
	int ret;
	int sceuid;
	int size;
	
	if( fd < 0 || fd > (__PSP_FILENO_MAX - 1) || __psp_descriptormap[fd]->type != __PSP_DESCRIPTOR_TYPE_PIPE ) {
		errno = EBADF;
		return -1;
	}
	
	sceuid = __psp_descriptormap[fd]->sce_descriptor;

	size = __psp_pipe_peekmsgsize(fd);
	if (size > 0) {
		if (size < len) {
			len = size;
		}
	}
	else {
		errno = EBADF;
		return -1;
	}
 
	/**
	* Receive a message from a pipe
	*
	* @param uid - The UID of the pipe
	* @param message - Pointer to the message
	* @param size - Size of the message
	* @param unk1 - Unknown
	* @param unk2 - Unknown
	* @param timeout - Timeout for receive
	*
	* @return 0 on success, < 0 on error
	*/
    ret = sceKernelTryReceiveMsgPipe(sceuid, buf, len, 0, 0);
	
	if (ret == 0) {/* Success - Data */
		return len;
	}
	else if (ret == SCE_KERNEL_ERROR_MPP_EMPTY) {/* No data */
		errno = EAGAIN;
		return -1;
	}
	else {/* Error */
		return __psp_set_errno(ret);
	}
}

int __psp_pipe_read(int fd, void *buf, size_t len)
{
	int ret;
	int sceuid;
	int size;
	
	if( fd < 0 || fd > (__PSP_FILENO_MAX - 1) || __psp_descriptormap[fd]->type != __PSP_DESCRIPTOR_TYPE_PIPE ) {
		errno = EBADF;
		return -1;
	}
	
	sceuid = __psp_descriptormap[fd]->sce_descriptor;

	size = __psp_pipe_peekmsgsize(fd);
	if (size > 0) {
		if (size < len) {
			len = size;
		}
	}
	else {
		errno = EBADF;
		return -1;
	}

	/**
	* Receive a message from a pipe
	*
	* @param uid - The UID of the pipe
	* @param message - Pointer to the message
	* @param size - Size of the message
	* @param unk1 - Unknown
	* @param unk2 - Unknown
	* @param timeout - Timeout for receive
	*
	* @return 0 on success, < 0 on error
	*/
	ret = sceKernelReceiveMsgPipe(sceuid, buf, len, 0, NULL, NULL);
	
	if (ret == 0) {/* Success - Data */
		return len;
	}
	else {/* Error */
		return __psp_set_errno(ret);
	}
}

int __psp_pipe_write(int fd, const void *buf, size_t len)
{
	int ret;
	int sceuid;
	char *cbuf;
	
	if( fd < 0 || fd > (__PSP_FILENO_MAX - 1) || __psp_descriptormap[fd]->type != __PSP_DESCRIPTOR_TYPE_PIPE ) {
		errno = EBADF;
		return -1;
	}
	
	sceuid = __psp_descriptormap[fd]->sce_descriptor;

	/**
	* Send a message to a pipe
	*
	* @param uid - The UID of the pipe
	* @param message - Pointer to the message
	* @param size - Size of the message
	* @param unk1 - Unknown
	* @param unk2 - Unknown
	* @param timeout - Timeout for send
	*
	* @return 0 on success, < 0 on error
	*/
	cbuf = (char *)buf;
	ret = sceKernelSendMsgPipe(sceuid, cbuf, len, 0, NULL, NULL);
	
	if (ret == 0) {/* Success - Data */
		return len;
	}
	else {/* Error */
		return __psp_set_errno(ret);
	}
}

int __psp_pipe_nonblocking_write(int fd, const void *buf, size_t len)
{
	int ret;
	int sceuid;
	char *cbuf;
	
	if( fd < 0 || fd > (__PSP_FILENO_MAX - 1) || __psp_descriptormap[fd]->type != __PSP_DESCRIPTOR_TYPE_PIPE ) {
		errno = EBADF;
		return -1;
	}
	
	sceuid = __psp_descriptormap[fd]->sce_descriptor;

	cbuf = (char *)buf;
	ret = sceKernelTrySendMsgPipe(sceuid, cbuf, len, 0, 0);
	
	if (ret == 0) {/* Success - Data */
		return len;
	}
	else if (ret == SCE_KERNEL_ERROR_MPP_FULL) {
		errno = EAGAIN;
		return -1;
	}
	else {/* Error */
		return __psp_set_errno(ret);
	}
}
