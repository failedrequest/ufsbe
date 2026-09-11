CC       = cc
FUSE3_CFLAGS != pkg-config fuse3 --cflags
FUSE3_LIBS   != pkg-config fuse3 --libs

CFLAGS   = -O2 -g -Wall -Wextra -Wno-unused-parameter \
           ${FUSE3_CFLAGS} \
           -DFUSE_USE_VERSION=35 \
           -I./src

LDFLAGS  = ${FUSE3_LIBS} -lufs

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
TARGET   = ufs-fuse

.PHONY: all clean

all: ${TARGET}

${TARGET}: ${OBJS}
	${CC} -o ${TARGET} ${OBJS} ${LDFLAGS}

.c.o:
	${CC} ${CFLAGS} -c ${.IMPSRC} -o ${.TARGET}

clean:
	rm -f ${OBJS} ${TARGET}
