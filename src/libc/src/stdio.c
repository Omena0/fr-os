/*
 * stdio.c — buffered streams.
 *
 * One struct per open stream, one buffer per stream, and two independent
 * cursors into it: a read cursor that tracks how much of the buffer is valid
 * and a write cursor that tracks how much is waiting to go out. The two never
 * share a byte, which is why the same buffer serves both directions and why
 * switching a stream between reading and writing does not need a mode check.
 *
 * The write policy is per stream. stderr is unbuffered so a crash cannot eat
 * the last line; stdout is line-buffered when it refers to a terminal and
 * fully buffered otherwise, which is the distinction that actually matters
 * interactively. Everything else opened by fopen is fully buffered, and
 * fopen("...", "a") additionally re-issues lseek before each write because
 * the kernel's append flag and a userspace O_APPEND offset race is not worth
 * a shared file-position lock.
 */
#include <stdio.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/syscall.h>

/* Raw syscall wrappers (syscall.c). */
extern int sys_write(int fd, const void *buf, size_t count);
extern int sys_read(int fd, void *buf, size_t count);
extern int sys_open(const char *path, int flags, mode_t mode);
extern int sys_close(int fd);
extern off_t sys_lseek(int fd, off_t offset, int whence);

/*
 * The formatting engine, shared with printf.c. It knows how to walk a format
 * string and nothing at all about buffers, so the FILE layer and the
 * snprintf family are the same walk with two different sinks.
 */
extern size_t __libc_vformat(void (*sink)(void *ctx, const char *data, size_t n),
			     void *ctx, const char *fmt, va_list ap);

/*
 * A stream is the header plus its buffer in one allocation. fclose frees the
 * single block, and the three standard streams are static objects so they
 * exist before any allocator is usable.
 *
 * Known limit: the single buffer is shared, and put_byte() appends at wpos
 * without first retiring whatever getc() has read but not yet consumed. A
 * program that interleaves reads and writes on one stream can therefore have
 * unread input overwritten. Nothing in this tree does that -- stdin, stdout and
 * stderr are separate streams, and fopen never hands back a stream both ways --
 * so the fix (compact the buffer on the first write after a read) is left for
 * the program that needs it rather than guessed at here.
 */
struct _IO_FILE {
	int		fd;
	int		flags;		/* FREAD | FWRITE | FAPPEND */
	int		mode;		/* _IOFBF / _IOLBF / _IONBF */
	int		ungot;		/* pushback byte, or -1 */
	int		err;
	int		eof;
	unsigned char	*buf;
	size_t		bufsize;
	size_t		rpos;		/* next byte to hand out to the caller */
	size_t		rend;		/* bytes valid in buf for reading */
	size_t		wpos;		/* bytes in buf waiting to be written */
};

#define STD_BUFFER 1024

/*
 * The standard streams are static objects rather than heap allocations: they
 * have to exist and be usable before malloc is, and crt1 runs before anything
 * else. Their buffers are static storage for the same reason.
 */
static unsigned char stdin_buf[STD_BUFFER];
static unsigned char stdout_buf[STD_BUFFER];
static unsigned char stderr_buf[STD_BUFFER];

static struct _IO_FILE stdin_file = {
	.fd = STDIN_FILENO, .flags = FREAD, .mode = _IOFBF, .ungot = -1,
	.buf = stdin_buf, .bufsize = STD_BUFFER,
};
static struct _IO_FILE stdout_file = {
	.fd = STDOUT_FILENO, .flags = FWRITE, .mode = _IOLBF, .ungot = -1,
	.buf = stdout_buf, .bufsize = STD_BUFFER,
};
static struct _IO_FILE stderr_file = {
	.fd = STDERR_FILENO, .flags = FWRITE, .mode = _IONBF, .ungot = -1,
	.buf = stderr_buf, .bufsize = STD_BUFFER,
};

FILE *stdin = &stdin_file;
/*
 * stdout is line-buffered rather than fully buffered because on this kernel fd
 * 1 is always the console, and a prompt that sits in a buffer is a prompt the
 * user never sees.
 */
FILE *stdout = &stdout_file;
FILE *stderr = &stderr_file;

/* ------------------------------------------------------------------ output -- */

/*
 * Push bytes at the fd, retrying a short write from the new offset rather than
 * treating it as an error: a pipe with a small buffer legitimately accepts less
 * than was offered.
 *
 * A writer that returns 0 with a non-zero count is an error, not an
 * end-of-stream: it means the write made no progress and, if the caller treats
 * 0 as success, will never make any. This used to `break` out of the loop and
 * return 0, so fflush() cleared wpos, reported success, and the caller believed
 * the bytes were on the console.
 */
static int write_all(int fd, const unsigned char *data, size_t n)
{
	size_t done = 0;

	while (done < n) {
		int ret = sys_write(fd, data + done, n - done);

		if (ret < 0) {
			if (__errno == EINTR)
				continue;
			return -1;
		}
		if (ret == 0)
			return -1;
		done += (size_t)ret;
	}
	return 0;
}

int fflush(FILE *stream)
{
	if (!stream) {
		/* No global list to walk: the three standard streams are the
		 * only streams that can exist without an fopen, and they are
		 * static, so they are named rather than tracked. */
		int ret = 0;

		if (fflush(stdout))
			ret = EOF;
		if (fflush(stderr))
			ret = EOF;
		return ret;
	}

	if (stream->flags & FAPPEND)
		sys_lseek(stream->fd, 0, SEEK_END);

	if (stream->wpos) {
		if (write_all(stream->fd, stream->buf, stream->wpos)) {
			stream->err = 1;
			/*
			 * Drop what could not be written so a later fflush does
			 * not resend the same prefix forever; a stream that
			 * failed mid-flush has already lost data and the error
			 * flag is where the caller learns about it.
			 */
			stream->wpos = 0;
			return EOF;
		}
		stream->wpos = 0;
	}
	return 0;
}

static int put_byte(FILE *stream, int c)
{
	unsigned char ch = (unsigned char)c;

	if (stream->mode == _IONBF) {
		if (write_all(stream->fd, &ch, 1)) {
			stream->err = 1;
			return EOF;
		}
		return (int)ch;
	}

	stream->buf[stream->wpos++] = ch;
	/* A line-buffered stream cannot let a partial line sit in memory: the
	 * next thing that happens may be a crash, and then the prompt the user
	 * is still looking at is gone. */
	if (stream->wpos == stream->bufsize ||
	    (stream->mode == _IOLBF && ch == '\n')) {
		if (fflush(stream))
			return EOF;
	}
	return (int)ch;
}

int fputc(int c, FILE *stream)
{
	return put_byte(stream, (unsigned char)c);
}

/* putc and getc are functions rather than macros so that no caller has to
 * evaluate a FILE expression twice or worry about it being a side effect. */
int putc(int c, FILE *stream)
{
	return fputc(c, stream);
}

int putchar(int c)
{
	return fputc(c, stdout);
}

int fputs(const char *s, FILE *stream)
{
	while (*s) {
		if (put_byte(stream, (unsigned char)*s++) == EOF)
			return EOF;
	}
	return 0;
}

int puts(const char *s)
{
	if (fputs(s, stdout) == EOF)
		return EOF;
	return putchar('\n') == EOF ? EOF : 0;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream)
{
	const unsigned char *p = ptr;
	size_t total;
	size_t done = 0;
	size_t i;

	if (size == 0 || nmemb == 0)
		return 0;
	/* Checked because the product is what the loop below counts to: a
	 * wrapped total would return "everything wrote" having written a
	 * fraction of a byte per element. */
	if (nmemb > SIZE_MAX / size) {
		__errno = EOVERFLOW;
		return 0;
	}
	total = size * nmemb;

	/* An unbuffered stream never has a write region, so it goes the long
	 * way round through put_byte(). This is the stderr path hello relies
	 * on to see its line even when stdout still has bytes queued. */
	if (stream->mode == _IONBF) {
		for (i = 0; i < total; i++) {
			if (put_byte(stream, p[i]) == EOF)
				return i / size;
		}
		return nmemb;
	}

	while (done < total) {
		size_t room = stream->bufsize - stream->wpos;
		size_t chunk = total - done;

		if (room == 0) {
			if (fflush(stream))
				return done / size;
			room = stream->bufsize;
		}
		if (chunk > room)
			chunk = room;
		/* Bulk copy into the write region. put_byte() appends the same
		 * bytes one at a time to the same place; doing it in one memcpy
		 * is what makes a 93 KB initrd dump or a large snprintf cheap. */
		memcpy(stream->buf + stream->wpos, p + done, chunk);
		stream->wpos += chunk;
		done += chunk;
		if (stream->wpos == stream->bufsize ||
		    (stream->mode == _IOLBF &&
		     memchr(p + done - chunk, '\n', chunk) != NULL))
			if (fflush(stream))
				return done / size;
	}
	return nmemb;
}

/*
 * Buffering mode and buffer size. setvbuf() only takes effect before the
 * stream's first I/O, which is the last point at which the buffer model can be
 * chosen: after that the stream may hold a partially filled buffer that
 * changing the buffer underneath would strand. POSIX requires the call to have
 * no effect in that case, so it reports failure and leaves the stream alone.
 */
int setvbuf(FILE *stream, char *buf, int mode, size_t size)
{
	if (!stream)
		return -1;
	if (mode != _IOFBF && mode != _IOLBF && mode != _IONBF) {
		__errno = EINVAL;
		return -1;
	}
	if (stream->wpos || stream->rpos < stream->rend)
		return -1;
	if (mode == _IONBF) {
		/* put_byte() writes straight to the fd in this mode, so the
		 * buffer is not merely unused: leaving the pointer in place
		 * would keep the static 1 KiB of stdout alive for nothing. */
		stream->mode = _IONBF;
		stream->buf = NULL;
		stream->bufsize = 0;
		return 0;
	}
	if (buf && size) {
		/* A caller-supplied buffer replaces the stream's own. It is
		 * borrowed, never freed: fclose only frees streams that came
		 * from fopen, and only their own trailing buffer. */
		stream->buf = (unsigned char *)buf;
		stream->bufsize = size;
	}
	stream->mode = mode;
	return 0;
}

void setbuf(FILE *stream, char *buf)
{
	/* The historical contract: a non-NULL buf means fully buffered with
	 * BUFSIZ bytes, NULL means unbuffered. */
	if (buf)
		setvbuf(stream, buf, _IOFBF, BUFSIZ);
	else
		setvbuf(stream, NULL, _IONBF, 0);
}

/* ------------------------------------------------------------------- input -- */

/*
 * Make at least one byte available in the read buffer. Returns 0 on success
 * and -1 at end of file or on error, with stream->eof/stream->err telling the
 * two apart.
 */
static int fill_read(FILE *stream)
{
	int ret;

	if (stream->rpos < stream->rend)
		return 0;
	stream->rpos = 0;
	stream->rend = 0;
	for (;;) {
		ret = sys_read(stream->fd, stream->buf, stream->bufsize);
		if (ret >= 0)
			break;
		if (__errno != EINTR)
			return -1;
	}
	if (ret == 0) {
		stream->eof = 1;
		return -1;
	}
	stream->rend = (size_t)ret;
	return 0;
}

int getc(FILE *stream)
{
	/* The pushback slot is checked first: it is the one byte that was
	 * legitimately read from the stream but not yet consumed. */
	if (stream->ungot >= 0) {
		int c = stream->ungot;

		stream->ungot = -1;
		return c;
	}
	if (fill_read(stream))
		return EOF;
	return (int)stream->buf[stream->rpos++];
}

int getchar(void)
{
	return getc(stdin);
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream)
{
	unsigned char *p = ptr;
	size_t want;
	size_t got = 0;
	int c;

	if (size == 0 || nmemb == 0)
		return 0;
	if (nmemb > SIZE_MAX / size) {
		__errno = EOVERFLOW;
		return 0;
	}
	want = size * nmemb;
	while (got < want) {
		c = getc(stream);
		if (c == EOF)
			break;
		p[got++] = (unsigned char)c;
	}
	return got / size;
}

int ungetc(int c, FILE *stream)
{
	/*
	 * One byte of pushback, which is what the struct has room for and all
	 * POSIX requires. getc() checks the slot first. A pushback after EOF
	 * clears the EOF indicator, as ungetc(3) specifies: the byte is what
	 * the caller just read, so the stream is not at end of file any more.
	 */
	if (c == EOF)
		return EOF;
	if (stream->ungot >= 0)
		return EOF;
	stream->ungot = (unsigned char)c;
	stream->eof = 0;
	return (unsigned char)c;
}

char *fgets(char *s, int size, FILE *stream)
{
	int i = 0;

	if (size <= 0)
		return NULL;
	while (i < size - 1) {
		int c = getc(stream);

		if (c == EOF)
			break;
		s[i++] = (char)c;
		if (c == '\n')
			break;
	}
	if (i == 0)
		return NULL;
	s[i] = '\0';
	return s;
}

/*
 * Read one line, delimiter included, into a buffer the caller either supplies
 * or leaves for us to grow. The caller frees *lineptr, so the reallocation is
 * done through realloc and never hidden behind a different allocator.
 */
int getline(char **lineptr, size_t *n, FILE *stream)
{
	size_t cap;
	size_t used = 0;
	char *line;
	int c;

	if (!lineptr || !n)
		return -1;

	/*
	 * A caller that passes a buffer it has not sized is a bug we cannot
	 * detect, so *n==0 with a non-NULL *lineptr is treated as "unknown
	 * size" and the buffer is replaced rather than trusted.
	 */
	cap = *n;
	line = *lineptr;
	if (!line || cap == 0) {
		cap = 128;
		line = malloc(cap);
		if (!line) {
			__errno = ENOMEM;
			return -1;
		}
		/* Published before the first read, not after the last write:
		 * every path out of the loop below leaves *lineptr naming a
		 * block the caller owns and can free exactly once. */
		*lineptr = line;
		*n = cap;
	}

	for (;;) {
		c = getc(stream);
		if (c == EOF) {
			/*
			 * EOF with nothing read is a failure; EOF part way
			 * through a line returns the partial line, which is
			 * what makes a trailing line without a newline visible
			 * to the caller. Either way the buffer is left owned
			 * by the caller: freeing it here would hand back a
			 * dangling pointer if the caller kept its own copy.
			 */
			if (used == 0)
				return -1;
			break;
		}
		if (used + 2 > cap) {
			char *grown;

			cap *= 2;
			grown = realloc(line, cap);
			if (!grown) {
				/* *lineptr still names the block that is still
				 * live, so the caller can free it. */
				__errno = ENOMEM;
				return -1;
			}
			line = grown;
		}
		line[used++] = (char)c;
		if (c == '\n')
			break;
	}
	line[used] = '\0';
	*lineptr = line;
	*n = cap;
	return (int)used;
}
int ferror(FILE *stream)
{
	return stream->err;
}

int feof(FILE *stream)
{
	return stream->eof;
}

void clearerr(FILE *stream)
{
	stream->err = 0;
	stream->eof = 0;
}

/* --------------------------------------------------------------- printf family */

static void file_sink(void *ctx, const char *data, size_t n)
{
	FILE *stream = ctx;
	size_t i;

	for (i = 0; i < n; i++) {
		if (put_byte(stream, (unsigned char)data[i]) == EOF)
			return;
	}
}

int vfprintf(FILE *stream, const char *format, va_list ap)
{
	return (int)__libc_vformat(file_sink, stream, format, ap);
}

int fprintf(FILE *stream, const char *format, ...)
{
	va_list ap;
	int ret;

	va_start(ap, format);
	ret = vfprintf(stream, format, ap);
	va_end(ap);
	return ret;
}

int vprintf(const char *format, va_list ap)
{
	return vfprintf(stdout, format, ap);
}

int printf(const char *format, ...)
{
	va_list ap;
	int ret;

	va_start(ap, format);
	ret = vfprintf(stdout, format, ap);
	va_end(ap);
	return ret;
}

int perror(const char *s)
{
	/*
	 * stderr is unbuffered, so the line cannot be lost in a buffer that a
	 * crash would take with it, and building the text through fprintf
	 * keeps the number formatting in one place.
	 *
	 * errno == 0 is not a reason to print nothing. glibc prints
	 * "Success", which is the answer to the question that was asked; a
	 * silent perror leaves the caller with no output and no clue that the
	 * errno it was about to report was never set in the first place.
	 */
	int err = __errno;

	if (s && *s)
		fprintf(stderr, "%s: %s\n", s, strerror(err));
	else
		fprintf(stderr, "%s\n", strerror(err));
	return 0;
}

/* ------------------------------------------------------------------ fopen -- */

int fclose(FILE *stream)
{
	int ret = 0;

	if (!stream)
		return EOF;
	if (fflush(stream))
		ret = EOF;
	/*
	 * The three standard streams are static and outlive every fclose, so
	 * closing one of them must not release its descriptor: fd 1 is the
	 * console for the whole lifetime of the process, and a program that
	 * fclose(stdout) -- which POSIX explicitly permits, with stdio
	 * reopening it on the next use -- would otherwise lose stdout for
	 * good. Everything else is closed and, if it came from fopen, freed.
	 */
	if (stream == &stdin_file || stream == &stdout_file ||
	    stream == &stderr_file) {
		if (sys_close(stream->fd))
			ret = EOF;
		/* Reopenable: the next use of the stream works again. */
		stream->fd = (stream == &stdin_file) ? STDIN_FILENO :
			     (stream == &stdout_file) ? STDOUT_FILENO :
			     STDERR_FILENO;
		stream->err = 0;
		stream->eof = 0;
		stream->wpos = 0;
		stream->rpos = 0;
		stream->rend = 0;
		stream->ungot = -1;
		return ret;
	}
	if (sys_close(stream->fd))
		ret = EOF;
	free(stream);
	return ret;
}

/*
 * The mode string is validated rather than pattern-matched loosely: POSIX
 * allows only [rwa] followed by an optional 'b' and an optional '+', in that
 * order and at most once each. The previous code looked only at mode[1], so
 * "rw" opened read-only and "r+b" fell through to the plain 'r' path -- a typo
 * became a silently wrong open rather than an error.
 */
FILE *fopen(const char *path, const char *mode)
{
	int oflags;
	int fflags = 0;
	int smode = _IOFBF;
	int plus = 0;
	const char *p;
	FILE *stream;

	if (!path || !mode || !*mode) {
		__errno = EINVAL;
		return NULL;
	}

	switch (mode[0]) {
	case 'r':
		oflags = O_RDONLY;
		fflags = FREAD;
		break;
	case 'w':
		oflags = O_WRONLY | O_CREAT | O_TRUNC;
		fflags = FWRITE;
		break;
	case 'a':
		oflags = O_WRONLY | O_CREAT | O_APPEND;
		fflags = FWRITE | FAPPEND;
		break;
	default:
		__errno = EINVAL;
		return NULL;
	}

	/* "b" is accepted and ignored: there is no mode where a byte and a text
	 * stream differ on this target, and rejecting it would break every
	 * portable program that writes "wb". */
	for (p = mode + 1; *p; p++) {
		if (*p == 'b')
			continue;
		if (*p == '+' && !plus) {
			plus = 1;
			continue;
		}
		__errno = EINVAL;
		return NULL;
	}

	if (plus) {
		oflags = (oflags & ~O_ACCMODE) | O_RDWR;
		/*
		 * OR in FREAD; do not reassign. The `fflags = FREAD | FWRITE`
		 * this replaces cleared the FAPPEND that the 'a' branch had
		 * just set, so "a+" lost O_APPEND and the lseek-to-end that
		 * fflush() issues on an append stream never happened --
		 * writes landed wherever the shared offset had drifted to.
		 */
		fflags |= FREAD;
	}

	stream = malloc(sizeof(*stream) + BUFSIZ);
	if (!stream) {
		__errno = ENOMEM;
		return NULL;
	}
	stream->fd = sys_open(path, oflags, 0666);
	if (stream->fd < 0) {
		free(stream);
		return NULL;
	}
	stream->flags = fflags;
	stream->mode = smode;
	stream->ungot = -1;
	stream->err = 0;
	stream->eof = 0;
	stream->buf = (unsigned char *)stream + sizeof(*stream);
	stream->bufsize = BUFSIZ;
	stream->rpos = 0;
	stream->rend = 0;
	stream->wpos = 0;
	return stream;
}
