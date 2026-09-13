CC       = cc
FUSE3_CFLAGS != pkg-config fuse3 --cflags
FUSE3_LIBS   != pkg-config fuse3 --libs

CFLAGS   = -O2 -g -Wall -Wextra -Wno-unused-parameter \
           ${FUSE3_CFLAGS} \
           -DFUSE_USE_VERSION=35 \
           -I./src

LDFLAGS  = ${FUSE3_LIBS} -lufs

# ── ufsbe daemon ─────────────────────────────────────────────────────

SRCS     = src/main.c      \
           src/block.c     \
           src/super.c     \
           src/betree.c    \
           src/inode.c     \
           src/dir.c       \
           src/softdep.c   \
           src/journal.c   \
           src/fuse_ops.c

OBJS     = ${SRCS:.c=.o}
TARGET   = ufsbe

# ── newfs_ufsbe ──────────────────────────────────────────────────────

NEWFS_SRCS = src/newfs_ufsbe.c
NEWFS_OBJS = ${NEWFS_SRCS:.c=.o}
NEWFS_TARGET = newfs_ufsbe
NEWFS_CFLAGS = -O2 -g -Wall -Wextra -Wno-unused-parameter -I./src
NEWFS_LDFLAGS = -lufs

# ── fsck_ufsbe ───────────────────────────────────────────────────────

FSCK_SRCS = src/fsck_ufsbe.c
FSCK_OBJS = ${FSCK_SRCS:.c=.o}
FSCK_TARGET = fsck_ufsbe
FSCK_CFLAGS = -O2 -g -Wall -Wextra -Wno-unused-parameter -I./src
FSCK_LDFLAGS = -lufs

# ── install paths ─────────────────────────────────────────────────────

PREFIX   ?= /usr/local
SBINDIR   = ${PREFIX}/sbin
MANDIR    = ${PREFIX}/share/man
STAGEDIR ?=

# ── targets ──────────────────────────────────────────────────────────

.PHONY: all clean install install-ufsbe install-newfs install-fsck

all: ${TARGET} ${NEWFS_TARGET} ${FSCK_TARGET}

# ufsbe
${TARGET}: ${OBJS}
	${CC} -o ${TARGET} ${OBJS} ${LDFLAGS}

# newfs_ufsbe
${NEWFS_TARGET}: ${NEWFS_OBJS}
	${CC} -o ${NEWFS_TARGET} ${NEWFS_OBJS} ${NEWFS_LDFLAGS}

# fsck_ufsbe
${FSCK_TARGET}: ${FSCK_OBJS}
	${CC} -o ${FSCK_TARGET} ${FSCK_OBJS} ${FSCK_LDFLAGS}

# Compile rules
.c.o:
	${CC} ${CFLAGS} -c ${.IMPSRC} -o ${.TARGET}

src/newfs_ufsbe.o: src/newfs_ufsbe.c
	${CC} ${NEWFS_CFLAGS} -c src/newfs_ufsbe.c -o src/newfs_ufsbe.o

src/fsck_ufsbe.o: src/fsck_ufsbe.c
	${CC} ${FSCK_CFLAGS} -c src/fsck_ufsbe.c -o src/fsck_ufsbe.o

# ── install ──────────────────────────────────────────────────────────

install: install-ufsbe install-newfs install-fsck

install-ufsbe: ${TARGET}
	install -d ${STAGEDIR}${SBINDIR}
	install -s -m 0555 ${TARGET} ${STAGEDIR}${SBINDIR}/ufsbe
	install -m 0555 port/files/mount_ufsbe.sh \
	    ${STAGEDIR}${SBINDIR}/mount_ufsbe
	install -d ${STAGEDIR}${MANDIR}/man8
	install -m 0444 port/files/ufsbe.8 \
	    ${STAGEDIR}${MANDIR}/man8/ufsbe.8
	install -m 0444 port/files/mount_ufsbe.8 \
	    ${STAGEDIR}${MANDIR}/man8/mount_ufsbe.8

install-newfs: ${NEWFS_TARGET}
	install -d ${STAGEDIR}${SBINDIR}
	install -s -m 0555 ${NEWFS_TARGET} \
	    ${STAGEDIR}${SBINDIR}/newfs_ufsbe
	install -d ${STAGEDIR}${MANDIR}/man8
	install -m 0444 src/newfs_ufsbe.8 \
	    ${STAGEDIR}${MANDIR}/man8/newfs_ufsbe.8

install-fsck: ${FSCK_TARGET}
	install -d ${STAGEDIR}${SBINDIR}
	install -s -m 0555 ${FSCK_TARGET} \
	    ${STAGEDIR}${SBINDIR}/fsck_ufsbe
	install -d ${STAGEDIR}${MANDIR}/man8
	install -m 0444 src/fsck_ufsbe.8 \
	    ${STAGEDIR}${MANDIR}/man8/fsck_ufsbe.8

# ── clean ─────────────────────────────────────────────────────────────

clean:
	rm -f ${OBJS} ${TARGET}
	rm -f ${NEWFS_OBJS} ${NEWFS_TARGET}
	rm -f ${FSCK_OBJS} ${FSCK_TARGET}
