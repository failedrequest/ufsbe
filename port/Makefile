PORTNAME=	ufsbe
DISTVERSION=	0.4.0
CATEGORIES=	filesystems sysutils
PKGNAMEPREFIX=	fuse-

MAINTAINER=	ports@FreeBSD.org
COMMENT=	FUSE3 UFS1/UFS2 filesystem with B-epsilon tree blkmap and directory index
WWW=		https://github.com/example/ufs-fuse

LICENSE=	BSD2CLAUSE
LICENSE_FILE=	${WRKSRC}/LICENSE

# fuse:3 pulls in fusefs-libs3; libufs is part of the base system
USES=		fuse:3 pkgconfig

# Source tarball is fetched from wherever you host it.
# While the project lives locally, point MASTER_SITES at a local file server
# or replace with your GitHub/hosting URL.
MASTER_SITES=	https://example.com/releases/

# Rename the top-level directory inside the tarball
WRKSRC=		${WRKDIR}/${PORTNAME}-${DISTVERSION}

# The project Makefile uses bmake variables (${.IMPSRC}, ${.TARGET}) which
# are already correct for FreeBSD make.  We only need to override the install
# step; there is no configure step.
NO_CONFIGURE=	yes

# Pass extra variables into the project Makefile
# patch-rename-ufsbe renames TARGET from ufs-fuse to ufsbe in Makefile
# and replaces all "ufs-fuse" strings in src/main.c with "ufsbe"
MAKE_ARGS=	TARGET=ufsbe \
		PREFIX="${PREFIX}" \
		STAGEDIR="${STAGEDIR}"

PLIST_FILES=	sbin/ufsbe \
		sbin/mount_ufsbe \
		share/man/man8/ufsbe.8.gz \
		share/man/man8/mount_ufsbe.8.gz

# ── install ──────────────────────────────────────────────────────────
# The upstream Makefile does not have an install target; we provide one.

do-install:
	${INSTALL_PROGRAM} ${WRKSRC}/ufsbe \
		${STAGEDIR}${PREFIX}/sbin/ufsbe
	${INSTALL_SCRIPT} ${FILESDIR}/mount_ufsbe.sh \
		${STAGEDIR}${PREFIX}/sbin/mount_ufsbe
	${INSTALL_MAN} ${FILESDIR}/ufsbe.8 \
		${STAGEDIR}${PREFIX}/share/man/man8/ufsbe.8
	${INSTALL_MAN} ${FILESDIR}/mount_ufsbe.8 \
		${STAGEDIR}${PREFIX}/share/man/man8/mount_ufsbe.8

.include <bsd.port.mk>
