PORTNAME=	ufsbe
DISTVERSION=	0.4.0
CATEGORIES=	filesystems sysutils
PKGNAMEPREFIX=	fuse-

MAINTAINER=	ports@FreeBSD.org
COMMENT=	FUSE3 UFS1/UFS2 filesystem with B-epsilon tree blkmap and directory index
WWW=		https://github.com/failedrequest/ufsbe

LICENSE=	BSD2CLAUSE
LICENSE_FILE=	${WRKSRC}/LICENSE

# fuse:3 pulls in fusefs-libs3; libufs is part of the base system
USES=		fuse:3 pkgconfig

# Source tarball is fetched from GitHub releases.
MASTER_SITES=	https://github.com/failedrequest/ufsbe/releases/download/v${DISTVERSION}/

# The top-level directory inside the tarball is ufsbe-<version>
WRKSRC=		${WRKDIR}/${PORTNAME}-${DISTVERSION}

# The project Makefile uses bmake variables; no configure step needed.
NO_CONFIGURE=	yes

# patch-rename-ufsbe renames TARGET from ufs-fuse to ufsbe in Makefile
# and replaces all "ufs-fuse" strings in src/main.c with "ufsbe"
MAKE_ARGS=	TARGET=ufsbe \
		PREFIX="${PREFIX}" \
		STAGEDIR="${STAGEDIR}"

PLIST_FILES=	sbin/ufsbe \
		sbin/mount_ufsbe \
		sbin/newfs_ufsbe \
		sbin/fsck_ufsbe \
		share/man/man8/ufsbe.8.gz \
		share/man/man8/mount_ufsbe.8.gz \
		share/man/man8/newfs_ufsbe.8.gz \
		share/man/man8/fsck_ufsbe.8.gz

# ── install ──────────────────────────────────────────────────────────

do-install:
	${INSTALL_PROGRAM} ${WRKSRC}/ufsbe \
		${STAGEDIR}${PREFIX}/sbin/ufsbe
	${INSTALL_SCRIPT} ${FILESDIR}/mount_ufsbe.sh \
		${STAGEDIR}${PREFIX}/sbin/mount_ufsbe
	${INSTALL_PROGRAM} ${WRKSRC}/newfs_ufsbe \
		${STAGEDIR}${PREFIX}/sbin/newfs_ufsbe
	${INSTALL_PROGRAM} ${WRKSRC}/fsck_ufsbe \
		${STAGEDIR}${PREFIX}/sbin/fsck_ufsbe
	${INSTALL_MAN} ${FILESDIR}/ufsbe.8 \
		${STAGEDIR}${PREFIX}/share/man/man8/ufsbe.8
	${INSTALL_MAN} ${FILESDIR}/mount_ufsbe.8 \
		${STAGEDIR}${PREFIX}/share/man/man8/mount_ufsbe.8
	${INSTALL_MAN} ${WRKSRC}/tools/newfs_ufsbe/newfs_ufsbe.8 \
		${STAGEDIR}${PREFIX}/share/man/man8/newfs_ufsbe.8
	${INSTALL_MAN} ${WRKSRC}/tools/fsck_ufsbe/fsck_ufsbe.8 \
		${STAGEDIR}${PREFIX}/share/man/man8/fsck_ufsbe.8

.include <bsd.port.mk>
