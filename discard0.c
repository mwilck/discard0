#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <syslog.h>
#include <getopt.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <assert.h>
#include <inttypes.h>
#include <sys/sysmacros.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <linux/fs.h>

#define log(lvl, fmt, ...) do {					\
		if (LOG_PRI(lvl) < opts.loglevel)	\
			fprintf(stderr, fmt, ##__VA_ARGS__);	\
	} while (0)

static struct discard_opts {
	const char *name;
	uint64_t dev_size;
	unsigned int size;
	unsigned int granularity;
	int major;
	int minor;
	int fd;
	int loglevel;
	bool dry;
} opts = {
	.loglevel = LOG_PRI(LOG_NOTICE) + 1,
	.dry = false
};

static char *_get_sysfs_attr(const char *filename)
{
	int fd;
	char buf[512];
	int n;
	char *ret = NULL;
	char *p;

	if (filename == NULL || *filename == '\0')
		return NULL;

	fd = open(filename, O_RDONLY);
	if (fd == -1) {
		log(LOG_DEBUG, "cannot open %s: %s\n", filename,
		    strerror(errno));
		return NULL;
	}

	n = read(fd, buf, sizeof(buf));
	if (n <= 0)
		goto out;
	p = memchr(buf, '\n', n);
	if (p)
		*p = '\0';
	else if (n == sizeof(buf))
		buf[sizeof(buf) - 1] = '\0';
	else
		buf[n] = '\0';
	ret = strdup(buf);
out:
	close(fd);
	return ret;
}

static char *bdev_sysfs_name(int major, int minor, const char *attr,
			     char *buf, int buflen)
{
	static const char block[] = "/sys/dev/block";
	int n;

	n = snprintf(buf, buflen, "%s/%d:%d/%s",
		     block, major, minor, attr);
	return n < buflen ? buf : NULL;
}

static char *get_bdev_sysfs_attr(int major, int minor, const char *attr)
{
	char path[PATH_MAX];
	char * res;

	res = _get_sysfs_attr(bdev_sysfs_name(major, minor, attr,
					      path, sizeof(path)));
	if (res == NULL)
		log(LOG_ERR, "%s: %d:%d %s -> FAIL",
		    __func__, major, minor, attr);
	else
		log(LOG_DEBUG, "%s: %d:%d %s -> \"%s\"\n",
		    __func__, major, minor, attr, res);
	return res;
}

int get_ulong_bdev_sysfs_attr(int major, int minor, const char *attr,
			      unsigned long *res)
{
	char *str, *eptr;
	unsigned long r;

	if (attr == NULL || *attr == '\0')
		return -1;

	str = get_bdev_sysfs_attr(major, minor, attr);
	if (str == NULL)
		return -1;

	r = strtoul(str, &eptr, 10);
	if (*str == '\0' || *eptr != '\0') {
		log(LOG_CRIT, "%s: %s: invalid value \"%s\"",
		    __func__, attr, str);
		free(str);
		return -1;
	}
	free(str);

	log(LOG_DEBUG, "%s: %d:%d: %s => %lu\n",
	    __func__, major, minor, attr, r);
	*res = r;

	return 0;
}

static void usage(const char *me)
{
	fprintf(stderr, "usage: %s [options] <BLOCKDEV>\n\n"
		"Options:\n"
		"\t-v: increase verbosity level\n"
		"\t-q: decrease verbosity level\n"
		"\t-y: don't ask for confirmation\n"
		"\t-n: dry-run\n", me);
}

static bool confirm(const char *name)
{
	char *line = NULL;
	size_t len = 0;
	ssize_t n;
	bool ret = false;

	if (!isatty(fileno(stdin)))
		return false;

	printf(" *** CAUTION: this program may destroy your data ***\n\n"
	       "Make sure device %s reliably returns all zeroes for discarded blocks.\n"
	       "Otherwise, data on this device may be corrupted.\n\n"
	       "Type 'YES' to confirm: ", name);

	n = getline(&line, &len, stdin);
	ret = (n == 4 && !strcmp(line, "YES\n"));

	free(line);
	return ret;
}

static int parse_opts(int argc, char *const argv[])
{
	static const char optstring[] = "vqyn";
	struct stat st;
	unsigned long val;
	bool force = false;

	do {
		int opt = getopt(argc, argv, optstring);

		switch (opt) {
		case -1:
			goto getopt_done;
		case 'v':
			opts.loglevel++;
			break;
		case 'q':
			opts.loglevel--;
			break;
		case 'n':
			opts.dry = true;
			break;
		case 'y':
			force = true;
			break;
		case '?':
		default:
			usage(argv[0]);
			return -1;
		}
	} while (true);

getopt_done:
	if (optind >= argc) {
		usage(argv[0]);
		return -1;
	}

	opts.name = argv[optind];
	if (stat(opts.name, &st) == -1) {
		log(LOG_CRIT, "%s: %s: %s\n", __func__, argv[optind],
		    strerror(errno));
		return -1;
	}

	if (!S_ISBLK(st.st_mode)) {
		log(LOG_CRIT, "%s: %s is not a block device\n",
		    __func__, opts.name);
		return -1;
	}
	opts.major = major(st.st_rdev);
	opts.minor = minor(st.st_rdev);
	opts.fd = open(opts.name, O_RDWR|O_EXCL);

	if (opts.fd == -1) {
		log(LOG_CRIT, "%s: unable to open %s: %s\n",
		    __func__, opts.name, strerror(errno));
		return -1;
	}

	if (get_ulong_bdev_sysfs_attr(opts.major, opts.minor,
				      "queue/minimum_io_size", &val) != 0)
		return -1;
	else
		opts.size = val;

	if (get_ulong_bdev_sysfs_attr(opts.major, opts.minor,
				      "queue/discard_granularity", &val) != 0)
		return -1;
	else
		opts.granularity = val;

	if (ioctl(opts.fd, BLKGETSIZE64, &opts.dev_size) == -1) {
		log(LOG_CRIT, "%s: %d:%d: failed to get size\n", __func__,
		    opts.major, opts.minor);
		return -1;
	}

	log(LOG_INFO,
	    "%s: %d:%d: dev size %"PRIu64" IO size %u, granularity %u\n",
	    opts.name, opts.major, opts.minor, opts.dev_size, opts.size,
	    opts.granularity);

	if (!opts.dry && !force && !confirm(opts.name))
		return -1;
	if (opts.dry)
		log(LOG_NOTICE, "%s: DRY RUN. Not changing any data.\n",
		    argv[0]);

	return 0;
}

enum {
	IS_ZERO = 0,
	IS_NOT_ZERO = 1,
	READ_ERR = 2,
	NEED_SEEK = -3
};

static int is_chunk_zero(int fd, char *buf, int len)
{
	int n;
	unsigned long *p;

	n = read(fd, buf, len);

	if (n < 0) {
		log(LOG_ERR, "%s: read: %s\n", __func__, strerror(errno));
		return READ_ERR;
	} else if (n < len) {
		log(LOG_NOTICE, "%s: short read: %d\n", __func__, n);
		return NEED_SEEK;
	} else if (n % sizeof(unsigned long) != 0) {
		log(LOG_NOTICE, "%s: unaligned: %d\n", __func__, n);
		/* play safe */
		return IS_NOT_ZERO;
	}

	for (p = (unsigned long*)buf;
	     p < (unsigned long*)buf + n / sizeof(unsigned long); p++)
		if (*p != 0UL)
			return IS_NOT_ZERO;

	return IS_ZERO;
}

#define OFS_INVAL (~0ULL)

static int discard0(void)
{
	char *fbuf;
	int ret = -1;
	off_t ofs = 0;
	uint64_t range[2] = { OFS_INVAL, OFS_INVAL };
	uint64_t freed = 0;
	bool need_seek = true, last = false;
	unsigned int chunksz = opts.granularity;

	if (chunksz < sysconf(_SC_PAGESIZE))
		chunksz = sysconf(_SC_PAGESIZE);
	assert (chunksz % opts.granularity == 0);
	log(LOG_INFO, "%s: chunk size is %u\n", __func__, chunksz);

	fbuf = malloc(chunksz);
	if (fbuf == NULL) {
		log(LOG_CRIT, "%s: malloc: %s\n", __func__, strerror(errno));
		goto out;
	}

	for (ofs = 0; ofs < opts.dev_size; ofs += chunksz) {
		int r;
		int len = chunksz;

		if (ofs + len >= opts.dev_size) {
			last = true;
			len = opts.dev_size - ofs;
		}

		if (need_seek) {
			if (lseek(opts.fd, ofs, SEEK_SET) == (off_t)-1) {
				log(LOG_CRIT, "%s: lseek: %s\n",
				    __func__, strerror(errno));
				goto out;
			}
			need_seek = false;
		}

		r = is_chunk_zero(opts.fd, fbuf, len);

		switch (r) {
		case IS_ZERO:
			log(LOG_DEBUG,
			    "%s: %"PRId64"-%"PRId64" is a zero chunk\n",
			    __func__, ofs, ofs + len);
			if (range[0] == OFS_INVAL)
				range[0] = ofs;
			if (last)
				range[1] = opts.dev_size - range[0];
			break;
		case NEED_SEEK:
			need_seek = true;
			/* fallthrough */
		case IS_NOT_ZERO:
			log(LOG_DEBUG,
			    "%s: %"PRId64"-%"PRId64" is not a zero chunk\n",
			    __func__, ofs, ofs + len);
			if (range[0] != OFS_INVAL)
				range[1] = ofs - range[0];
			break;
		case READ_ERR:
			goto out;
		}

		if (range[0] != OFS_INVAL && range[1] != OFS_INVAL) {
			int res;

			log(LOG_NOTICE,
			    "%s: found zero range %" PRIu64 " - %" PRIu64
			    " (%" PRIu64 " blocks)\n",
			    __func__, range[0], range[0] + range[1],
			    range[1] >> 9);

			if (!opts.dry) {
				res = ioctl(opts.fd, BLKDISCARD, &range);
				if (res == -1) {
					log(LOG_ERR, "%s: discard: %s\n",
					    __func__, strerror(errno));
					goto out;
				}
			}
			freed += range[1];

			range[0] = range[1] = OFS_INVAL;
		}
	}
	ret = 0;
out:
	free(fbuf);
	close(opts.fd);
	printf("%" PRIu64 " storage bytes discarded\n", freed);
	return ret;
}

int main (int argc, char *const argv[])
{
	if (parse_opts(argc, argv) != 0)
		return 1;
	return discard0();
}
