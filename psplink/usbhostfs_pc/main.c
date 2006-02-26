/*
 * PSPLINK
 * -----------------------------------------------------------------------
 * Licensed under the BSD license, see LICENSE in PSPLINK root for details.
 *
 * main.c - Main code for PC side of USB HostFS
 *
 * Copyright (c) 2006 James F <tyranid@gmail.com>
 *
 * $HeadURL$
 * $Id$
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <usb.h>
#include <limits.h>
#include <fcntl.h>
#include <usbhostfs.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <utime.h>
#include "psp_fileio.h"

#define MAX_FILES 256
#define MAX_DIRS  256
#define MAX_TOKENS 256

/* TODO: Make the response encode the errno so newlib handles it correctly
 * i.e. setting 0x8001<errno>
 */

#define MAX_HOSTDRIVES 8

/* Contains the paths for a single hist drive */
struct HostDrive
{
	char rootdir[PATH_MAX];
	char currdir[PATH_MAX];
};

struct FileHandle
{
	int opened;
	int mode;
};

struct DirHandle
{
	int opened;
	/* Current count of entries left */
	int count;
	/* Current position in the directory entries */
	int pos;
	/* Head of list, each entry will be freed when read */
	SceIoDirent *pDir;
};

struct FileHandle open_files[MAX_FILES];
struct DirHandle  open_dirs[MAX_DIRS];

//char g_rootdir[PATH_MAX];
//char g_currdir[PATH_MAX];

struct HostDrive g_drives[MAX_HOSTDRIVES];

int  g_verbose = 0;

#define V_PRINTF(fmt, ...) { if(g_verbose) { fprintf(stderr, fmt, ## __VA_ARGS__); } }

#ifdef BUILD_BIGENDIAN
uint16_t swap16(uint16_t i)
{
	uint8_t *p = (uint8_t *) &i;
	uint16_t ret;

	ret = (p[0] << 8) | p[1];

	return ret;
}

uint32_t swap32(uint32_t i)
{
	uint8_t *p = (uint8_t *) &i;
	uint32_t ret;

	ret = (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];

	return ret;
}

uint64_t swap64(uint64_t i)
{
	uint8_t *p = (uint8_t *) &i;
	uint64_t ret;

	ret = (uint64_t) p[7] | ((uint64_t) p[6] << 8) | ((uint64_t) p[5] << 16) | ((uint64_t) p[4] << 24) 
		| ((uint64_t) p[3] << 32) | ((uint64_t) p[2] << 40) | ((uint64_t) p[1] << 48) | ((uint64_t) p[0] << 56);

	return ret;
}
#define LE16(x) swap16(x)
#define LE32(x) swap32(x)
#define LE64(x) swap64(x)
#else
#define LE16(x) (x)
#define LE32(x) (x)
#define LE64(x) (x)
#endif

/* Define wrappers for the usb functions we use which can set euid */
int euid_usb_bulk_write(usb_dev_handle *dev, int ep, char *bytes, int size,
	int timeout)
{
	int ret;

	seteuid(0);
	setegid(0);
	ret = usb_bulk_write(dev, ep, bytes, size, timeout);
	seteuid(getuid());
	setegid(getgid());

	return ret;
}

int euid_usb_bulk_read(usb_dev_handle *dev, int ep, char *bytes, int size,
	int timeout)
{
	int ret;

	seteuid(0);
	setegid(0);
	ret = usb_bulk_read(dev, ep, bytes, size, timeout);
	seteuid(getuid());
	setegid(getgid());

	return ret;
}

usb_dev_handle *open_device(struct usb_bus *busses)
{
	struct usb_bus *bus = NULL;
	struct usb_dev_handle *hDev = NULL;

	seteuid(0);
	setegid(0);

	for(bus = busses; bus; bus = bus->next) 
	{
		struct usb_device *dev;

		for(dev = bus->devices; dev; dev = dev->next)
		{
			if((dev->descriptor.idVendor == SONY_VID) 
				&& (dev->descriptor.idProduct == HOSTFSDRIVER_PID))
			{
				hDev = usb_open(dev);
				if(hDev != NULL)
				{
					int ret;
					ret = usb_set_configuration(hDev, 1);
					if(ret == 0)
					{
						ret = usb_claim_interface(hDev, 0);
						if(ret == 0)
						{
							seteuid(getuid());
							setegid(getgid());
							return hDev;
						}
						else
						{
							usb_close(hDev);
							hDev = NULL;
						}
					}
					else
					{
						usb_close(hDev);
						hDev = NULL;
					}
				}
			}
		}
	}
	
	if(hDev)
	{
		usb_close(hDev);
	}

	seteuid(getuid());
	setegid(getgid());

	return NULL;
}

void close_device(struct usb_dev_handle *hDev)
{
	seteuid(0);
	setegid(0);
	if(hDev)
	{
		usb_release_interface(hDev, 0);
		usb_close(hDev);
	}
	seteuid(getuid());
	setegid(getgid());
}

int gen_path(char *path, int dir)
{
	char abspath[PATH_MAX];
	const char *tokens[MAX_TOKENS];
	const char *outtokens[MAX_TOKENS];
	int count;
	int token;
	int pathpos;

	strcpy(abspath, path);
	count = 0;
	tokens[0] = strtok(abspath, "/");
	while((tokens[count]) && (count < (MAX_TOKENS-1)))
	{
		tokens[++count] = strtok(NULL, "/");
	}

	/* Remove any single . and .. */
	pathpos = 0;
	for(token = 0; token < count; token++)
	{
		if(strcmp(tokens[token], ".") == 0)
		{
			/* Do nothing */
		}
		else if(strcmp(tokens[token], "..") == 0)
		{
			/* Decrement the path position if > 0 */
			if(pathpos > 0)
			{
				pathpos--;
			}
		}
		else
		{
			outtokens[pathpos++] = tokens[token];
		}
	}

	strcpy(path, "/");
	for(token = 0; token < pathpos; token++)
	{
		strcat(path, outtokens[token]);
		if((dir) || (token < (pathpos-1)))
		{
			strcat(path, "/");
		}
	}

	return 1;
}

int make_path(unsigned int drive, const char *path, char *retpath, int dir)
{
	char hostpath[PATH_MAX];
	int len;

	if(drive >= MAX_HOSTDRIVES)
	{
		fprintf(stderr, "Host drive number is too large (%d)\n", drive);
		return -1;
	}

	len = snprintf(hostpath, PATH_MAX, "%s%s", g_drives[drive].currdir, path);
	if((len < 0) || (len >= PATH_MAX))
	{
		fprintf(stderr, "Path length too big (%d)\n", len);
		return -1;
	}

	if(gen_path(hostpath, dir) == 0)
	{
		return -1;
	}

	len = snprintf(retpath, PATH_MAX, "%s/%s", g_drives[drive].rootdir, hostpath);
	if((len < 0) || (len >= PATH_MAX))
	{
		fprintf(stderr, "Path length too big (%d)\n", len);
		return -1;
	}

	if(gen_path(retpath, dir) == 0)
	{
		return -1;
	}

	return 0;
}

int open_file(int drive, const char *path, unsigned int mode, unsigned int mask)
{
	char fullpath[PATH_MAX];
	unsigned int real_mode = 0;
	int fd = -1;
	
	if(make_path(drive, path, fullpath, 0) < 0)
	{
		return -1;
	}

	V_PRINTF("open: %s\n", fullpath);

	if((mode & PSP_O_RDWR) == PSP_O_RDWR)
	{
		V_PRINTF("Read/Write mode\n");
		real_mode = O_RDWR;
	}
	else
	{
		if(mode & PSP_O_RDONLY)
		{
			V_PRINTF("Read mode\n");
			real_mode = O_RDONLY;
		}
		else if(mode & PSP_O_WRONLY)
		{
			V_PRINTF("Write mode\n");
			real_mode = O_WRONLY;
		}
		else
		{
			fprintf(stderr, "No access mode specified\n");
			return -1;
		}
	}

	if(mode & PSP_O_APPEND)
	{
		real_mode |= O_APPEND;
	}

	if(mode & PSP_O_CREAT)
	{
		real_mode |= O_CREAT;
	}

	if(mode & PSP_O_TRUNC)
	{
		real_mode |= O_TRUNC;
	}

	if(mode & PSP_O_EXCL)
	{
		real_mode |= O_EXCL;
	}

	fd = open(fullpath, real_mode, mask & ~0111);
	if(fd >= 0)
	{
		if(fd < MAX_FILES)
		{
			open_files[fd].opened = 1;
			open_files[fd].mode = mode;
		}
		else
		{
			close(fd);
			fprintf(stderr, "Error filedescriptor out of range\n");
			fd = -1;
		}
	}

	return fd;
}

void fill_time(time_t t, ScePspDateTime *scetime)
{
	struct tm *filetime;

	memset(scetime, 0, sizeof(*scetime));
	filetime = localtime(&t);
	scetime->year = LE16(filetime->tm_year + 1900);
	scetime->month = LE16(filetime->tm_mon + 1);
	scetime->day = LE16(filetime->tm_mday);
	scetime->hour = LE16(filetime->tm_hour);
	scetime->minute = LE16(filetime->tm_min);
	scetime->second = LE16(filetime->tm_sec);
}

int fill_stat(const char *dirname, const char *name, SceIoStat *scestat)
{
	char path[PATH_MAX];
	struct stat st;
	int len;

	/* If dirname is NULL then name is a preconverted path */
	if(dirname != NULL)
	{
		len = snprintf(path, PATH_MAX, "%s/%s", dirname, name);
		if((len < 0) || (len > PATH_MAX))
		{
			fprintf(stderr, "Couldn't fill in directory name\n");
			return -1;
		}
	}
	else
	{
		strcpy(path, name);
	}

	if(stat(path, &st) < 0)
	{
		fprintf(stderr, "Couldn't stat file %s (%s)\n", path, strerror(errno));
		return -1;
	}

	scestat->size = LE64(st.st_size);
	scestat->mode = 0;
	scestat->attr = 0;
	if(S_ISLNK(st.st_mode))
	{
		scestat->attr = LE32(FIO_SO_IFLNK);
	}
	else if(S_ISDIR(st.st_mode))
	{
		scestat->attr = LE32(FIO_SO_IFDIR);
	}
	else
	{
		scestat->attr = LE32(FIO_SO_IFREG);
	}

	scestat->mode = LE32(st.st_mode & (S_IRWXU | S_IRWXG | S_IRWXO));

	fill_time(st.st_ctime, &scestat->ctime);
	fill_time(st.st_atime, &scestat->atime);
	fill_time(st.st_mtime, &scestat->mtime);

	return 0;
}

int dir_open(int drive, const char *dirname)
{
	char fulldir[PATH_MAX];
	struct dirent **entries;
	int ret = -1;
	int i;
	int did;
	int dirnum;

	do
	{
		for(did = 0; did < MAX_DIRS; did++)
		{
			if(!open_dirs[did].opened)
			{
				break;
			}
		}

		if(did == MAX_DIRS)
		{
			fprintf(stderr, "Could not find free directory handle\n");
			break;
		}

		if(make_path(drive, dirname, fulldir, 1) < 0)
		{
			break;
		}

		V_PRINTF("dopen: %s, fsnum %d\n", fulldir, drive);

		memset(&open_dirs[did], 0, sizeof(open_dirs[did]));

		dirnum = scandir(fulldir, &entries, NULL, alphasort);
		if(dirnum <= 0)
		{
			fprintf(stderr, "Could not scan directory (%s)\n", strerror(errno));
			break;
		}

		V_PRINTF("Number of dir entries %d\n", dirnum);

		open_dirs[did].pDir = malloc(sizeof(SceIoDirent) * dirnum);
		if(open_dirs[did].pDir != NULL)
		{
			memset(open_dirs[did].pDir, 0, sizeof(SceIoDirent) * dirnum);
			for(i = 0; i < dirnum; i++)
			{
				strcpy(open_dirs[did].pDir[i].name, entries[i]->d_name);
				V_PRINTF("Dirent %d: %s\n", i, entries[i]->d_name);
				if(fill_stat(fulldir, entries[i]->d_name, &open_dirs[did].pDir[i].stat) < 0)
				{
					fprintf(stderr, "Error filling in directory structure\n");
					break;
				}
			}

			if(i == dirnum)
			{
				ret = did;
				open_dirs[did].pos = 0;
				open_dirs[did].count = dirnum;
				open_dirs[did].opened = 1;
			}
			else
			{
				free(open_dirs[did].pDir);
			}
		}
		else
		{
			fprintf(stderr, "Could not allocate memory for directories\n");
		}

		if(ret < 0)
		{
			for(i = 0; i < dirnum; i++)
			{
				free(entries[i]);
			}
			free(entries);
		}
	}
	while(0);

	return ret;
}

int dir_close(int did)
{
	int ret = -1;
	if((did >= 0) && (did < MAX_DIRS))
	{
		if(open_dirs[did].opened)
		{
			if(open_dirs[did].pDir)
			{
				free(open_dirs[did].pDir);
			}

			open_dirs[did].opened = 0;
			open_dirs[did].count = 0;
			open_dirs[did].pos = 0;
			open_dirs[did].pDir = NULL;

			ret = 0;
		}
	}

	return ret;
}

int handle_hello(struct usb_dev_handle *hDev)
{
	struct HostFsHelloResp resp;

	memset(&resp, 0, sizeof(resp));
	resp.cmd.magic = LE32(HOSTFS_MAGIC);
	resp.cmd.command = LE32(HOSTFS_CMD_HELLO);

	return usb_bulk_write(hDev, 0x2, (char *) &resp, sizeof(resp), 10000);
}

int handle_open(struct usb_dev_handle *hDev, struct HostFsOpenCmd *cmd, int cmdlen)
{
	struct HostFsOpenResp resp;
	int  ret = -1;
	char path[HOSTFS_PATHMAX];

	memset(&resp, 0, sizeof(resp));
	resp.cmd.magic = LE32(HOSTFS_MAGIC);
	resp.cmd.command = LE32(HOSTFS_CMD_OPEN);
	resp.res = LE32(-1);

	do
	{
		if(cmdlen != sizeof(struct HostFsOpenCmd)) 
		{
			fprintf(stderr, "Error, invalid open command size %d\n", cmdlen);
			break;
		}

		if(LE32(cmd->cmd.extralen) == 0)
		{
			fprintf(stderr, "Error, no filename passed with open command\n");
			break;
		}

		/* TODO: Should check that length is within a valid range */

		ret = euid_usb_bulk_read(hDev, 0x81, path, LE32(cmd->cmd.extralen), 10000);
		if(ret != LE32(cmd->cmd.extralen))
		{
			fprintf(stderr, "Error reading open data cmd->extralen %d, ret %d\n", LE32(cmd->cmd.extralen), ret);
			break;
		}

		V_PRINTF("Open command mode %08X mask %08X name %s\n", LE32(cmd->mode), LE32(cmd->mask), path);
		resp.res = LE32(open_file(LE32(cmd->fsnum), path, LE32(cmd->mode), LE32(cmd->mask)));

		ret = euid_usb_bulk_write(hDev, 0x2, (char *) &resp, sizeof(resp), 10000);
	}
	while(0);

	return ret;
}

int handle_dopen(struct usb_dev_handle *hDev, struct HostFsDopenCmd *cmd, int cmdlen)
{
	struct HostFsDopenResp resp;
	int  ret = -1;
	char path[HOSTFS_PATHMAX];

	memset(&resp, 0, sizeof(resp));
	resp.cmd.magic = LE32(HOSTFS_MAGIC);
	resp.cmd.command = LE32(HOSTFS_CMD_DOPEN);
	resp.res = LE32(-1);

	do
	{
		if(cmdlen != sizeof(struct HostFsDopenCmd)) 
		{
			fprintf(stderr, "Error, invalid dopen command size %d\n", cmdlen);
			break;
		}

		if(LE32(cmd->cmd.extralen) == 0)
		{
			fprintf(stderr, "Error, no dirname passed with dopen command\n");
			break;
		}

		/* TODO: Should check that length is within a valid range */

		ret = euid_usb_bulk_read(hDev, 0x81, path, LE32(cmd->cmd.extralen), 10000);
		if(ret != LE32(cmd->cmd.extralen))
		{
			fprintf(stderr, "Error reading open data cmd->extralen %d, ret %d\n", LE32(cmd->cmd.extralen), ret);
			break;
		}

		V_PRINTF("Dopen command name %s\n", path);
		resp.res = LE32(dir_open(LE32(cmd->fsnum), path));

		ret = euid_usb_bulk_write(hDev, 0x2, (char *) &resp, sizeof(resp), 10000);
	}
	while(0);

	return ret;
}

int fixed_write(int fd, const void *data, int len)
{
	int byteswrite = 0;
	while(byteswrite < len)
	{
		int ret;

		ret = write(fd, data+byteswrite, len-byteswrite);
		if(ret < 0)
		{
			if(errno != EINTR)
			{
				fprintf(stderr, "Error writing to file (%s)\n", strerror(errno));
				byteswrite = -1;
				break;
			}
		}
		else if(ret == 0) /* EOF? */
		{
			break;
		}
		else
		{
			byteswrite += ret;
		}
	}

	return byteswrite;
}

int handle_write(struct usb_dev_handle *hDev, struct HostFsWriteCmd *cmd, int cmdlen)
{
	static char write_block[HOSTFS_MAX_BLOCK];
	struct HostFsWriteResp resp;
	int  fid;
	int  ret = -1;

	memset(&resp, 0, sizeof(resp));
	resp.cmd.magic = LE32(HOSTFS_MAGIC);
	resp.cmd.command = LE32(HOSTFS_CMD_WRITE);
	resp.res = LE32(-1);

	do
	{
		if(cmdlen != sizeof(struct HostFsWriteCmd)) 
		{
			fprintf(stderr, "Error, invalid write command size %d\n", cmdlen);
			break;
		}

		/* TODO: Check upper bound */
		if(LE32(cmd->cmd.extralen) <= 0)
		{
			fprintf(stderr, "Error extralen invalid (%d)\n", LE32(cmd->cmd.extralen));
			break;
		}

		/* TODO: Should check that length is within a valid range */

		ret = euid_usb_bulk_read(hDev, 0x81, write_block, LE32(cmd->cmd.extralen), 10000);
		if(ret != LE32(cmd->cmd.extralen))
		{
			fprintf(stderr, "Error reading write data cmd->extralen %d, ret %d\n", LE32(cmd->cmd.extralen), ret);
			break;
		}

		fid = LE32(cmd->fid);

		V_PRINTF("Write command fid: %d, length: %d\n", fid, LE32(cmd->cmd.extralen));

		if((fid >= 0) && (fid < MAX_FILES))
		{
			if(open_files[fid].opened)
			{
				resp.res = LE32(fixed_write(fid, write_block, LE32(cmd->cmd.extralen)));
			}
			else
			{
				fprintf(stderr, "Error fid not open %d\n", fid);
			}
		}
		else
		{
			fprintf(stderr, "Error invalid fid %d\n", fid);
		}

		ret = euid_usb_bulk_write(hDev, 0x2, (char *) &resp, sizeof(resp), 10000);
	}
	while(0);

	return ret;
}

int fixed_read(int fd, void *data, int len)
{
	int bytesread = 0;

	while(bytesread < len)
	{
		int ret;

		ret = read(fd, data+bytesread, len-bytesread);
		if(ret < 0)
		{
			if(errno != EINTR)
			{
				bytesread = -1;
				break;
			}
		}
		else if(ret == 0)
		{
			/* No more to read */
			break;
		}
		else
		{
			bytesread += ret;
		}
	}

	return bytesread;
}

int handle_read(struct usb_dev_handle *hDev, struct HostFsReadCmd *cmd, int cmdlen)
{
	static char read_block[HOSTFS_MAX_BLOCK];
	struct HostFsReadResp resp;
	int  fid;
	int  ret = -1;

	memset(&resp, 0, sizeof(resp));
	resp.cmd.magic = LE32(HOSTFS_MAGIC);
	resp.cmd.command = LE32(HOSTFS_CMD_READ);
	resp.res = LE32(-1);

	do
	{
		if(cmdlen != sizeof(struct HostFsReadCmd)) 
		{
			fprintf(stderr, "Error, invalid read command size %d\n", cmdlen);
			break;
		}

		/* TODO: Check upper bound */
		if(LE32(cmd->len) <= 0)
		{
			fprintf(stderr, "Error extralen invalid (%d)\n", LE32(cmd->len));
			break;
		}

		fid = LE32(cmd->fid);
		V_PRINTF("Read command fid: %d, length: %d\n", fid, LE32(cmd->len));

		if((fid >= 0) && (fid < MAX_FILES))
		{
			if(open_files[fid].opened)
			{
				resp.res = LE32(fixed_read(fid, read_block, LE32(cmd->len)));
				if(resp.res >= 0)
				{
					resp.cmd.extralen = resp.res;
				}
			}
			else
			{
				fprintf(stderr, "Error fid not open %d\n", fid);
			}
		}
		else
		{
			fprintf(stderr, "Error invalid fid %d\n", fid);
		}

		ret = euid_usb_bulk_write(hDev, 0x2, (char *) &resp, sizeof(resp), 10000);
		if(ret < 0)
		{
			fprintf(stderr, "Error writing read response (%d)\n", ret);
			break;
		}

		if(LE32(resp.cmd.extralen) > 0)
		{
			ret = euid_usb_bulk_write(hDev, 0x2, read_block, LE32(resp.cmd.extralen), 10000);
		}
	}
	while(0);

	return ret;
}

int handle_close(struct usb_dev_handle *hDev, struct HostFsCloseCmd *cmd, int cmdlen)
{
	struct HostFsCloseResp resp;
	int  ret = -1;
	int  fid;

	memset(&resp, 0, sizeof(resp));
	resp.cmd.magic = LE32(HOSTFS_MAGIC);
	resp.cmd.command = LE32(HOSTFS_CMD_CLOSE);
	resp.res = LE32(-1);

	do
	{
		if(cmdlen != sizeof(struct HostFsCloseCmd)) 
		{
			fprintf(stderr, "Error, invalid close command size %d\n", cmdlen);
			break;
		}

		fid = LE32(cmd->fid);
		V_PRINTF("Close command fid: %d\n", fid);
		if((fid > STDERR_FILENO) && (fid < MAX_FILES) && (open_files[fid].opened))
		{
			resp.res = LE32(close(fid));
			open_files[fid].opened = 0;
		}
		else
		{
			fprintf(stderr, "Error invalid file id in close command (%d)\n", fid);
		}

		ret = euid_usb_bulk_write(hDev, 0x2, (char *) &resp, sizeof(resp), 10000);
	}
	while(0);

	return ret;
}

int handle_dclose(struct usb_dev_handle *hDev, struct HostFsDcloseCmd *cmd, int cmdlen)
{
	struct HostFsDcloseResp resp;
	int  ret = -1;
	int  did;

	memset(&resp, 0, sizeof(resp));
	resp.cmd.magic = LE32(HOSTFS_MAGIC);
	resp.cmd.command = LE32(HOSTFS_CMD_DCLOSE);
	resp.res = LE32(-1);

	do
	{
		if(cmdlen != sizeof(struct HostFsDcloseCmd)) 
		{
			fprintf(stderr, "Error, invalid close command size %d\n", cmdlen);
			break;
		}

		did = LE32(cmd->did);
		V_PRINTF("Dclose command did: %d\n", did);
		resp.res = dir_close(did);

		ret = euid_usb_bulk_write(hDev, 0x2, (char *) &resp, sizeof(resp), 10000);
	}
	while(0);


	return ret;
}

int handle_dread(struct usb_dev_handle *hDev, struct HostFsDreadCmd *cmd, int cmdlen)
{
	struct HostFsDreadResp resp;
	SceIoDirent *dir = NULL;
	int  ret = -1;
	int  did;

	memset(&resp, 0, sizeof(resp));
	resp.cmd.magic = LE32(HOSTFS_MAGIC);
	resp.cmd.command = LE32(HOSTFS_CMD_READ);
	resp.res = LE32(-1);

	do
	{
		if(cmdlen != sizeof(struct HostFsDreadCmd)) 
		{
			fprintf(stderr, "Error, invalid dread command size %d\n", cmdlen);
			break;
		}

		did = LE32(cmd->did);
		V_PRINTF("Dread command did: %d\n", did);

		if((did >= 0) && (did < MAX_FILES))
		{
			if(open_dirs[did].opened)
			{
				if(open_dirs[did].pos < open_dirs[did].count)
				{
					dir = &open_dirs[did].pDir[open_dirs[did].pos++];
					resp.cmd.extralen = LE32(sizeof(SceIoDirent));
					resp.res = LE32(open_dirs[did].count - open_dirs[did].pos + 1);
				}
				else
				{
					resp.res = LE32(0);
				}
			}
			else
			{
				fprintf(stderr, "Error did not open %d\n", did);
			}
		}
		else
		{
			fprintf(stderr, "Error invalid did %d\n", did);
		}

		ret = euid_usb_bulk_write(hDev, 0x2, (char *) &resp, sizeof(resp), 10000);
		if(ret < 0)
		{
			fprintf(stderr, "Error writing dread response (%d)\n", ret);
			break;
		}

		if(LE32(resp.cmd.extralen) > 0)
		{
			ret = euid_usb_bulk_write(hDev, 0x2, (char *) dir, LE32(resp.cmd.extralen), 10000);
		}
	}
	while(0);

	return ret;
}

int handle_lseek(struct usb_dev_handle *hDev, struct HostFsLseekCmd *cmd, int cmdlen)
{
	struct HostFsLseekResp resp;
	int  ret = -1;
	int  fid;

	memset(&resp, 0, sizeof(resp));
	resp.cmd.magic = LE32(HOSTFS_MAGIC);
	resp.cmd.command = LE32(HOSTFS_CMD_LSEEK);
	resp.res = LE32(-1);
	resp.ofs = LE32(0);

	do
	{
		if(cmdlen != sizeof(struct HostFsLseekCmd)) 
		{
			fprintf(stderr, "Error, invalid lseek command size %d\n", cmdlen);
			break;
		}

		fid = LE32(cmd->fid);
		V_PRINTF("Lseek command fid: %d, ofs: %lld, whence: %d\n", fid, LE64(cmd->ofs), LE32(cmd->whence));
		if((fid > STDERR_FILENO) && (fid < MAX_FILES) && (open_files[fid].opened))
		{
			/* TODO: Probably should ensure whence is mapped across, just in case */
			resp.ofs = LE64((int64_t) lseek(fid, (off_t) LE64(cmd->ofs), LE32(cmd->whence)));
			if(LE64(resp.ofs) < 0)
			{
				resp.res = LE32(-1);
			}
			else
			{
				resp.res = LE32(0);
			}
		}
		else
		{
			fprintf(stderr, "Error invalid file id in close command (%d)\n", fid);
		}

		ret = euid_usb_bulk_write(hDev, 0x2, (char *) &resp, sizeof(resp), 10000);
	}
	while(0);

	return ret;
}

int handle_remove(struct usb_dev_handle *hDev, struct HostFsRemoveCmd *cmd, int cmdlen)
{
	struct HostFsRemoveResp resp;
	int  ret = -1;
	char path[HOSTFS_PATHMAX];
	char fullpath[PATH_MAX];

	memset(&resp, 0, sizeof(resp));
	resp.cmd.magic = LE32(HOSTFS_MAGIC);
	resp.cmd.command = LE32(HOSTFS_CMD_REMOVE);
	resp.res = LE32(-1);

	do
	{
		if(cmdlen != sizeof(struct HostFsRemoveCmd)) 
		{
			fprintf(stderr, "Error, invalid remove command size %d\n", cmdlen);
			break;
		}

		if(LE32(cmd->cmd.extralen) == 0)
		{
			fprintf(stderr, "Error, no filename passed with remove command\n");
			break;
		}

		/* TODO: Should check that length is within a valid range */

		ret = euid_usb_bulk_read(hDev, 0x81, path, LE32(cmd->cmd.extralen), 10000);
		if(ret != LE32(cmd->cmd.extralen))
		{
			fprintf(stderr, "Error reading remove data cmd->extralen %d, ret %d\n", LE32(cmd->cmd.extralen), ret);
			break;
		}

		V_PRINTF("Remove command name %s\n", path);
		if(make_path(LE32(cmd->fsnum), path, fullpath, 0) == 0)
		{
			resp.res = LE32(unlink(fullpath));
		}

		ret = euid_usb_bulk_write(hDev, 0x2, (char *) &resp, sizeof(resp), 10000);
	}
	while(0);

	return ret;
}

int handle_rmdir(struct usb_dev_handle *hDev, struct HostFsRmdirCmd *cmd, int cmdlen)
{
	struct HostFsRmdirResp resp;
	int  ret = -1;
	char path[HOSTFS_PATHMAX];
	char fullpath[PATH_MAX];

	memset(&resp, 0, sizeof(resp));
	resp.cmd.magic = LE32(HOSTFS_MAGIC);
	resp.cmd.command = LE32(HOSTFS_CMD_RMDIR);
	resp.res = LE32(-1);

	do
	{
		if(cmdlen != sizeof(struct HostFsRmdirCmd)) 
		{
			fprintf(stderr, "Error, invalid rmdir command size %d\n", cmdlen);
			break;
		}

		if(LE32(cmd->cmd.extralen) == 0)
		{
			fprintf(stderr, "Error, no filename passed with rmdir command\n");
			break;
		}

		/* TODO: Should check that length is within a valid range */

		ret = euid_usb_bulk_read(hDev, 0x81, path, LE32(cmd->cmd.extralen), 10000);
		if(ret != LE32(cmd->cmd.extralen))
		{
			fprintf(stderr, "Error reading rmdir data cmd->extralen %d, ret %d\n", LE32(cmd->cmd.extralen), ret);
			break;
		}

		V_PRINTF("Rmdir command name %s\n", path);
		if(make_path(LE32(cmd->fsnum), path, fullpath, 0) == 0)
		{
			resp.res = LE32(rmdir(fullpath));
		}

		ret = euid_usb_bulk_write(hDev, 0x2, (char *) &resp, sizeof(resp), 10000);
	}
	while(0);

	return ret;
}

int handle_mkdir(struct usb_dev_handle *hDev, struct HostFsMkdirCmd *cmd, int cmdlen)
{
	struct HostFsMkdirResp resp;
	int  ret = -1;
	char path[HOSTFS_PATHMAX];
	char fullpath[PATH_MAX];

	memset(&resp, 0, sizeof(resp));
	resp.cmd.magic = LE32(HOSTFS_MAGIC);
	resp.cmd.command = LE32(HOSTFS_CMD_MKDIR);
	resp.res = LE32(-1);

	do
	{
		if(cmdlen != sizeof(struct HostFsMkdirCmd)) 
		{
			fprintf(stderr, "Error, invalid mkdir command size %d\n", cmdlen);
			break;
		}

		if(LE32(cmd->cmd.extralen) == 0)
		{
			fprintf(stderr, "Error, no filename passed with mkdir command\n");
			break;
		}

		/* TODO: Should check that length is within a valid range */

		ret = euid_usb_bulk_read(hDev, 0x81, path, LE32(cmd->cmd.extralen), 10000);
		if(ret != LE32(cmd->cmd.extralen))
		{
			fprintf(stderr, "Error reading mkdir data cmd->extralen %d, ret %d\n", LE32(cmd->cmd.extralen), ret);
			break;
		}

		V_PRINTF("Mkdir command mode %08X, name %s\n", LE32(cmd->mode), path);
		if(make_path(LE32(cmd->fsnum), path, fullpath, 0) == 0)
		{
			resp.res = LE32(mkdir(fullpath, LE32(cmd->mode)));
		}

		ret = euid_usb_bulk_write(hDev, 0x2, (char *) &resp, sizeof(resp), 10000);
	}
	while(0);

	return ret;
}

int handle_getstat(struct usb_dev_handle *hDev, struct HostFsGetstatCmd *cmd, int cmdlen)
{
	struct HostFsGetstatResp resp;
	SceIoStat st;
	int  ret = -1;
	char path[HOSTFS_PATHMAX];
	char fullpath[PATH_MAX];

	memset(&resp, 0, sizeof(resp));
	resp.cmd.magic = LE32(HOSTFS_MAGIC);
	resp.cmd.command = LE32(HOSTFS_CMD_GETSTAT);
	resp.res = LE32(-1);
	memset(&st, 0, sizeof(st));

	do
	{
		if(cmdlen != sizeof(struct HostFsGetstatCmd)) 
		{
			fprintf(stderr, "Error, invalid getstat command size %d\n", cmdlen);
			break;
		}

		if(LE32(cmd->cmd.extralen) == 0)
		{
			fprintf(stderr, "Error, no filename passed with getstat command\n");
			break;
		}

		/* TODO: Should check that length is within a valid range */

		ret = euid_usb_bulk_read(hDev, 0x81, path, LE32(cmd->cmd.extralen), 10000);
		if(ret != LE32(cmd->cmd.extralen))
		{
			fprintf(stderr, "Error reading getstat data cmd->extralen %d, ret %d\n", LE32(cmd->cmd.extralen), ret);
			break;
		}

		V_PRINTF("Getstat command name %s\n", path);
		if(make_path(LE32(cmd->fsnum), path, fullpath, 0) == 0)
		{
			resp.res = LE32(fill_stat(NULL, fullpath, &st));
			if(LE32(resp.res) == 0)
			{
				resp.cmd.extralen = LE32(sizeof(st));
			}
		}

		ret = euid_usb_bulk_write(hDev, 0x2, (char *) &resp, sizeof(resp), 10000);
		if(ret < 0)
		{
			fprintf(stderr, "Error writing getstat response (%d)\n", ret);
			break;
		}

		if(LE32(resp.cmd.extralen) > 0)
		{
			ret = euid_usb_bulk_write(hDev, 0x2, (char *) &st, sizeof(st), 10000);
		}
	}
	while(0);

	return ret;
}

int psp_settime(const char *path, const struct HostFsTimeStamp *ts, int set)
{
	time_t convtime;
	struct tm stime;
	struct utimbuf tbuf;
	struct stat st;

	stime.tm_year = LE16(ts->year) - 1900;
	stime.tm_mon = LE16(ts->month) - 1;
	stime.tm_mday = LE16(ts->day);
	stime.tm_hour = LE16(ts->hour);
	stime.tm_min = LE16(ts->minute);
	stime.tm_sec = LE16(ts->second);

	if(stat(path, &st) < 0)
	{
		return -1;
	}

	tbuf.actime = st.st_atime;
	tbuf.modtime = st.st_mtime;

	convtime = mktime(&stime);
	if(convtime == (time_t)-1)
	{
		return -1;
	}

	if(set == PSP_CHSTAT_ATIME)
	{
		tbuf.actime = convtime;
	}
	else if(set == PSP_CHSTAT_MTIME)
	{
		tbuf.modtime = convtime;
	}
	else
	{
		return -1;
	}

	return utime(path, &tbuf);

}

int psp_chstat(const char *path, struct HostFsChstatCmd *cmd)
{
	int ret = 0;

	if(LE32(cmd->bits) & PSP_CHSTAT_MODE)
	{
		int mask;

		mask = LE32(cmd->mode) & (FIO_S_IRWXU | FIO_S_IRWXG | FIO_S_IRWXO);
		ret = chmod(path, mask);
		if(ret < 0)
		{
			V_PRINTF("Could not set file mask\n");
			return -1;
		}
	}

	if(LE32(cmd->bits) & PSP_CHSTAT_SIZE)
	{
		/* Do a truncate */
	}

	if(LE32(cmd->bits) & PSP_CHSTAT_ATIME)
	{
		if(psp_settime(path, &cmd->atime, PSP_CHSTAT_ATIME) < 0)
		{
			V_PRINTF("Could not set access time\n");
			return -1;
		}
	}

	if(LE32(cmd->bits) & PSP_CHSTAT_MTIME)
	{
		if(psp_settime(path, &cmd->mtime, PSP_CHSTAT_MTIME) < 0)
		{
			V_PRINTF("Could not set modification time\n");
			return -1;
		}
	}

	return 0;
}

int handle_chstat(struct usb_dev_handle *hDev, struct HostFsChstatCmd *cmd, int cmdlen)
{
	struct HostFsChstatResp resp;
	int  ret = -1;
	char path[HOSTFS_PATHMAX];
	char fullpath[PATH_MAX];

	memset(&resp, 0, sizeof(resp));
	resp.cmd.magic = LE32(HOSTFS_MAGIC);
	resp.cmd.command = LE32(HOSTFS_CMD_CHSTAT);
	resp.res = LE32(-1);

	do
	{
		if(cmdlen != sizeof(struct HostFsChstatCmd)) 
		{
			fprintf(stderr, "Error, invalid chstat command size %d\n", cmdlen);
			break;
		}

		if(LE32(cmd->cmd.extralen) == 0)
		{
			fprintf(stderr, "Error, no filename passed with chstat command\n");
			break;
		}

		/* TODO: Should check that length is within a valid range */

		ret = euid_usb_bulk_read(hDev, 0x81, path, LE32(cmd->cmd.extralen), 10000);
		if(ret != LE32(cmd->cmd.extralen))
		{
			fprintf(stderr, "Error reading chstat data cmd->extralen %d, ret %d\n", LE32(cmd->cmd.extralen), ret);
			break;
		}

		V_PRINTF("Chstat command name %s, bits %08X\n", path, LE32(cmd->bits));
		if(make_path(LE32(cmd->fsnum), path, fullpath, 0) == 0)
		{
			resp.res = LE32(psp_chstat(fullpath, cmd));
		}

		ret = euid_usb_bulk_write(hDev, 0x2, (char *) &resp, sizeof(resp), 10000);
	}
	while(0);

	return ret;
}

int handle_rename(struct usb_dev_handle *hDev, struct HostFsRenameCmd *cmd, int cmdlen)
{
	struct HostFsRenameResp resp;
	int  ret = -1;
	char path[HOSTFS_PATHMAX];
	char oldpath[PATH_MAX];
	char newpath[PATH_MAX];
	int  oldpathlen;
	int  newpathlen;

	memset(&resp, 0, sizeof(resp));
	resp.cmd.magic = LE32(HOSTFS_MAGIC);
	resp.cmd.command = LE32(HOSTFS_CMD_RENAME);
	resp.res = LE32(-1);

	do
	{
		if(cmdlen != sizeof(struct HostFsRenameCmd)) 
		{
			fprintf(stderr, "Error, invalid mkdir command size %d\n", cmdlen);
			break;
		}

		if(LE32(cmd->cmd.extralen) == 0)
		{
			fprintf(stderr, "Error, no filenames passed with rename command\n");
			break;
		}

		/* TODO: Should check that length is within a valid range */

		memset(path, 0, sizeof(path));
		ret = euid_usb_bulk_read(hDev, 0x81, path, LE32(cmd->cmd.extralen), 10000);
		if(ret != LE32(cmd->cmd.extralen))
		{
			fprintf(stderr, "Error reading rename data cmd->extralen %d, ret %d\n", LE32(cmd->cmd.extralen), ret);
			break;
		}

		/* Really should check this better ;) */
		oldpathlen = strlen(path);
		newpathlen = strlen(path+oldpathlen+1);

		V_PRINTF("Rename command oldname %s, newname %s\n", path, path+oldpathlen+1);
		if(!make_path(LE32(cmd->fsnum), path, oldpath, 0) && !make_path(LE32(cmd->fsnum), path+oldpathlen+1, newpath, 0))
		{
			resp.res = LE32(rename(oldpath, newpath));
		}

		ret = euid_usb_bulk_write(hDev, 0x2, (char *) &resp, sizeof(resp), 10000);
	}
	while(0);

	return ret;
}

int handle_chdir(struct usb_dev_handle *hDev, struct HostFsChdirCmd *cmd, int cmdlen)
{
	struct HostFsChdirResp resp;
	int  ret = -1;
	char path[HOSTFS_PATHMAX];
	int fsnum;

	memset(&resp, 0, sizeof(resp));
	resp.cmd.magic = LE32(HOSTFS_MAGIC);
	resp.cmd.command = LE32(HOSTFS_CMD_CHDIR);
	resp.res = -1;

	do
	{
		if(cmdlen != sizeof(struct HostFsChdirCmd)) 
		{
			fprintf(stderr, "Error, invalid chdir command size %d\n", cmdlen);
			break;
		}

		if(LE32(cmd->cmd.extralen) == 0)
		{
			fprintf(stderr, "Error, no filename passed with mkdir command\n");
			break;
		}

		/* TODO: Should check that length is within a valid range */

		ret = euid_usb_bulk_read(hDev, 0x81, path, LE32(cmd->cmd.extralen), 10000);
		if(ret != LE32(cmd->cmd.extralen))
		{
			fprintf(stderr, "Error reading chdir data cmd->extralen %d, ret %d\n", LE32(cmd->cmd.extralen), ret);
			break;
		}

		V_PRINTF("Chdir command name %s\n", path);
		
		fsnum = LE32(cmd->fsnum);
		if((fsnum >= 0) && (fsnum < MAX_HOSTDRIVES))
		{
			strcpy(g_drives[fsnum].currdir, path);
			resp.res = 0;
		}

		ret = euid_usb_bulk_write(hDev, 0x2, (char *) &resp, sizeof(resp), 10000);
	}
	while(0);

	return ret;
}

int handle_ioctl(struct usb_dev_handle *hDev, struct HostFsIoctlCmd *cmd, int cmdlen)
{
	static char inbuf[64*1024];
	static char outbuf[64*1024];
	int inlen;
	struct HostFsIoctlResp resp;
	int  ret = -1;

	memset(&resp, 0, sizeof(resp));
	resp.cmd.magic = LE32(HOSTFS_MAGIC);
	resp.cmd.command = LE32(HOSTFS_CMD_IOCTL);
	resp.res = LE32(-1);

	do
	{
		if(cmdlen != sizeof(struct HostFsIoctlCmd)) 
		{
			fprintf(stderr, "Error, invalid ioctl command size %d\n", cmdlen);
			break;
		}

		inlen = LE32(cmd->cmd.extralen);
		if(inlen > 0)
		{
			/* TODO: Should check that length is within a valid range */

			ret = euid_usb_bulk_read(hDev, 0x81, inbuf, inlen, 10000);
			if(ret != inlen)
			{
				fprintf(stderr, "Error reading ioctl data cmd->extralen %d, ret %d\n", inlen, ret);
				break;
			}
		}

		V_PRINTF("Ioctl command fid %d, cmdno %d, inlen %d\n", LE32(cmd->fid), LE32(cmd->cmdno), inlen);

		ret = euid_usb_bulk_write(hDev, 0x2, (char *) &resp, sizeof(resp), 10000);
		if(ret < 0)
		{
			fprintf(stderr, "Error writing ioctl response (%d)\n", ret);
			break;
		}

		if(LE32(resp.cmd.extralen) > 0)
		{
			ret = euid_usb_bulk_write(hDev, 0x2, (char *) outbuf, LE32(resp.cmd.extralen), 10000);
		}
	}
	while(0);

	return ret;
}

int handle_devctl(struct usb_dev_handle *hDev, struct HostFsDevctlCmd *cmd, int cmdlen)
{
	static char inbuf[64*1024];
	static char outbuf[64*1024];
	int inlen;
	struct HostFsDevctlResp resp;
	int  ret = -1;

	memset(&resp, 0, sizeof(resp));
	resp.cmd.magic = LE32(HOSTFS_MAGIC);
	resp.cmd.command = LE32(HOSTFS_CMD_DEVCTL);
	resp.res = LE32(-1);

	do
	{
		if(cmdlen != sizeof(struct HostFsDevctlCmd)) 
		{
			fprintf(stderr, "Error, invalid devctl command size %d\n", cmdlen);
			break;
		}

		inlen = LE32(cmd->cmd.extralen);
		if(inlen > 0)
		{
			/* TODO: Should check that length is within a valid range */

			ret = euid_usb_bulk_read(hDev, 0x81, inbuf, inlen, 10000);
			if(ret != inlen)
			{
				fprintf(stderr, "Error reading devctl data cmd->extralen %d, ret %d\n", inlen, ret);
				break;
			}
		}

		V_PRINTF("Devctl command cmdno %d, inlen %d\n", LE32(cmd->cmdno), inlen);

		ret = euid_usb_bulk_write(hDev, 0x2, (char *) &resp, sizeof(resp), 10000);
		if(ret < 0)
		{
			fprintf(stderr, "Error writing devctl response (%d)\n", ret);
			break;
		}

		if(LE32(resp.cmd.extralen) > 0)
		{
			ret = euid_usb_bulk_write(hDev, 0x2, (char *) outbuf, LE32(resp.cmd.extralen), 10000);
		}
	}
	while(0);

	return ret;
}

usb_dev_handle *wait_for_device(void)
{
	usb_dev_handle *hDev = NULL;

	while(hDev == NULL)
	{
		usb_find_busses();
		usb_find_devices();

		hDev = open_device(usb_get_busses());
		if(hDev)
		{
			fprintf(stderr, "Connected to device\n");
			break;
		}

		/* Sleep for two seconds */
		if(sleep(2) < 0)
		{
			break;
		}
	}

	return hDev;
}

int init_hostfs(void)
{
	int i;

	memset(open_files, 0, sizeof(open_files));
	memset(open_dirs, 0, sizeof(open_dirs));
	for(i = 0; i < MAX_HOSTDRIVES; i++)
	{
		strcpy(g_drives[i].currdir, "/");
	}

	return 0;
}

void close_hostfs(void)
{
	int i;

	for(i = 0; i < MAX_FILES; i++)
	{
		if(open_files[i].opened)
		{
			close(i);
			open_files[i].opened = 0;
		}
	}

	for(i = 0; i < MAX_DIRS; i++)
	{
		if(open_dirs[i].opened)
		{
			dir_close(i);
		}
	}
}

int start_hostfs(void)
{
	usb_dev_handle *hDev = NULL;
	char data[512];
	int readlen;
	int hellorecv = 0;

	while(1)
	{
		init_hostfs();

		hDev = wait_for_device();

		if(hDev)
		{
			while(1)
			{
				struct HostFsCmd *cmd;

				readlen = euid_usb_bulk_read(hDev, 0x81, data, 512, 0);
				if(readlen == 0)
				{
					fprintf(stderr, "Read cancelled (remote disconnected)\n");
					break;
				}

				if(readlen < sizeof(struct HostFsCmd))
				{
					fprintf(stderr, "Error reading command header %d\n", readlen);
					break;
				}

				cmd = (struct HostFsCmd *) data;

				V_PRINTF("Magic: %08X\n", LE32(cmd->magic));
				V_PRINTF("Command Num: %08X\n", LE32(cmd->command));
				V_PRINTF("Extra Len: %d\n", LE32(cmd->extralen));

				if(LE32(cmd->magic) != HOSTFS_MAGIC)
				{
					fprintf(stderr, "Error, invalid magic for command %08X\n", LE32(cmd->magic));
					continue;
				}

				if((hellorecv) || (LE32(cmd->command) == HOSTFS_CMD_HELLO))
				{
					switch(LE32(cmd->command))
					{
						case HOSTFS_CMD_HELLO: if(handle_hello(hDev) < 0)
											   {
												   fprintf(stderr, "Error sending hello response\n");
											   }
											   hellorecv = 1;
											   break;
						case HOSTFS_CMD_OPEN:  if(handle_open(hDev, (struct HostFsOpenCmd *) cmd, readlen) < 0)
											   {
												   fprintf(stderr, "Error in open command\n");
											   }
											   break;
						case HOSTFS_CMD_CLOSE: if(handle_close(hDev, (struct HostFsCloseCmd *) cmd, readlen) < 0)
											   {
												   fprintf(stderr, "Error in close command\n");
											   }
											   break;
						case HOSTFS_CMD_WRITE: if(handle_write(hDev, (struct HostFsWriteCmd *) cmd, readlen) < 0)
											   {
												   fprintf(stderr, "Error in write command\n");
											   }
											   break;
						case HOSTFS_CMD_READ:  if(handle_read(hDev, (struct HostFsReadCmd *) cmd, readlen) < 0)
											   {
												   fprintf(stderr, "Error in read command\n");
											   }
											   break;
						case HOSTFS_CMD_LSEEK: if(handle_lseek(hDev, (struct HostFsLseekCmd *) cmd, readlen) < 0)
											   {
												   fprintf(stderr, "Error in lseek command\n");
											   }
											   break;
						case HOSTFS_CMD_DOPEN: if(handle_dopen(hDev, (struct HostFsDopenCmd *) cmd, readlen) < 0)
											   {
												   fprintf(stderr, "Error in dopen command\n");
											   }
											   break;
						case HOSTFS_CMD_DCLOSE: if(handle_dclose(hDev, (struct HostFsDcloseCmd *) cmd, readlen) < 0)
												{
													fprintf(stderr, "Error in dclose command\n");
												}
												break;
						case HOSTFS_CMD_DREAD: if(handle_dread(hDev, (struct HostFsDreadCmd *) cmd, readlen) < 0)
											   {
													fprintf(stderr, "Error in dread command\n");
											   }
											   break;
						case HOSTFS_CMD_REMOVE: if(handle_remove(hDev, (struct HostFsRemoveCmd *) cmd, readlen) < 0)
												{
													fprintf(stderr, "Error in remove command\n");
												}
												break;
						case HOSTFS_CMD_RMDIR: if(handle_rmdir(hDev, (struct HostFsRmdirCmd *) cmd, readlen) < 0)
												{
													fprintf(stderr, "Error in rmdir command\n");
												}
												break;
						case HOSTFS_CMD_MKDIR: if(handle_mkdir(hDev, (struct HostFsMkdirCmd *) cmd, readlen) < 0)
												{
													fprintf(stderr, "Error in mkdir command\n");
												}
												break;
						case HOSTFS_CMD_CHDIR: if(handle_chdir(hDev, (struct HostFsChdirCmd *) cmd, readlen) < 0)
												{
													fprintf(stderr, "Error in chdir command\n");
												}
												break;
						case HOSTFS_CMD_RENAME: if(handle_rename(hDev, (struct HostFsRenameCmd *) cmd, readlen) < 0)
												{
													fprintf(stderr, "Error in rename command\n");
												}
												break;
						case HOSTFS_CMD_GETSTAT:if(handle_getstat(hDev, (struct HostFsGetstatCmd *) cmd, readlen) < 0)
												{
													fprintf(stderr, "Error in getstat command\n");
												}
												break;
						case HOSTFS_CMD_CHSTAT: if(handle_chstat(hDev, (struct HostFsChstatCmd *) cmd, readlen) < 0)
												{
													fprintf(stderr, "Error in chstat command\n");
												}
												break;
						case HOSTFS_CMD_IOCTL: if(handle_ioctl(hDev, (struct HostFsIoctlCmd *) cmd, readlen) < 0)
											   {
												   fprintf(stderr, "Error in ioctl command\n");
											   }
											   break;
						case HOSTFS_CMD_DEVCTL: if(handle_devctl(hDev, (struct HostFsDevctlCmd *) cmd, readlen) < 0)
											   {
												   fprintf(stderr, "Error in devctl command\n");
											   }
											   break;
						default: fprintf(stderr, "Error, unknown command %08X\n", cmd->command);
								 break;
					};
				}
			}

			close_device(hDev);
		}

		close_hostfs();
	}

	return 0;
}

int parse_args(int argc, char **argv)
{
	char rootdir[PATH_MAX];
	int i;

	if(getcwd(rootdir, PATH_MAX) < 0)
	{
		fprintf(stderr, "Could not get current path\n");
		return 0;
	}

	for(i = 0; i < MAX_HOSTDRIVES; i++)
	{
		strcpy(g_drives[i].rootdir, rootdir);
	}

	while(1)
	{
		int ch;

		ch = getopt(argc, argv, "vh");
		if(ch == -1)
		{
			break;
		}

		switch(ch)
		{
			case 'v': g_verbose = 1;
					  break;
			case 'h': return 0;
			default:  printf("Unknown option\n");
					  return 0;
					  break;
		};
	}

	argc -= optind;
	argv += optind;

	if(argc > 0)
	{
		if(argc > MAX_HOSTDRIVES)
		{
			argc = MAX_HOSTDRIVES;
		}

		for(i = 0; i < argc; i++)
		{
			if(argv[i][0] != '/')
			{
				char tmpdir[PATH_MAX];
				snprintf(tmpdir, PATH_MAX, "%s/%s", rootdir, argv[i]);
				strcpy(g_drives[i].rootdir, tmpdir);
			}
			else
			{
				strcpy(g_drives[i].rootdir, argv[i]);
			}
			gen_path(g_drives[i].rootdir, 0);
			V_PRINTF("Root %d: %s\n", i, g_drives[i].rootdir);
		}
	}
	else
	{
		V_PRINTF("Root directory: %s\n", rootdir);
	}

	return 1;
}

void print_help(void)
{
	fprintf(stderr, "Usage: usbhostfs_pc [options] [rootdir0..rootdir%d]\n", MAX_HOSTDRIVES-1);
	fprintf(stderr, "Options:\n");
	fprintf(stderr, "-v                : Set verbose mode\n");
	fprintf(stderr, "-h                : Print this help\n");
}

int main(int argc, char **argv)
{
	printf("USBHostFS (c) TyRaNiD 2k6\n");
	if(parse_args(argc, argv))
	{
		/* Mask out any executable bits, as they don't make sense */
		usb_init();
		start_hostfs();
	}
	else
	{
		print_help();
	}

	return 0;
}
