/*
 * Copyright (c) 1999-2002 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 *
 * "Portions Copyright (c) 1999 Apple Computer, Inc.  All Rights
 * Reserved.  This file contains Original Code and/or Modifications of
 * Original Code as defined in and that are subject to the Apple Public
 * Source License Version 1.0 (the 'License').  You may not use this file
 * except in compliance with the License.  Please obtain a copy of the
 * License at http://www.apple.com/publicsource and read it before using
 * this file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE OR NON-INFRINGEMENT.  Please see the
 * License for the specific language governing rights and limitations
 * under the License."
 *
 * @APPLE_LICENSE_HEADER_END@
 */
/*
 * Copyright (c) 1998 Apple Computer, Inc. All Rights Reserved
 *
 *		MODIFICATION HISTORY (most recent first):
 *
 *	   20-Jul-1998	Don Brady		New today.
 */

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <paths.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <syslog.h>
#include <unistd.h>

#include <sys/param.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>

#ifdef __FreeBSD__
#include <sys/disk.h>
#include "SRuntime.h"
#else
#include <IOKit/storage/IOMediaBSDClient.h>
#endif

#include <hfsplus/hfs_format.h>
#include "newfs_hfs.h"

#if __STDC__
#include <stdarg.h>
#else
#include <varargs.h>
#endif

#define ROUNDUP(x, y) (((x) + (y) - 1) / (y) * (y))

static void getnodeopts __P((char *optlist));
static void getclumpopts __P((char *optlist));
static int hfs_newfs __P((char *device, int forceHFS, int isRaw));
static void validate_hfsplus_block_size __P((UInt64 sectorCount,
					     UInt32 sectorSize));
static void hfsplus_params __P((UInt64 sectorCount, UInt32 sectorSize,
				hfsparams_t *defaults));
static void hfs_params __P((UInt32 sectorCount, UInt32 sectorSize,
			    hfsparams_t *defaults));
static UInt32 clumpsizecalc __P((UInt32 clumpblocks));
static UInt32 CalcBTreeClumpSize __P((UInt32 blockSize, UInt32 nodeSize,
				      UInt32 driveBlocks, int catalog));
static UInt32 CalcHFSPlusBTreeClumpSize __P((UInt32 blockSize, UInt32 nodeSize,
					     UInt64 sectors, int catalog));
static void usage __P((void));

char *progname;
char gVolumeName[kHFSPlusMaxFileNameChars] = {kDefaultVolumeNameStr};
char rawdevice[MAXPATHLEN];
char blkdevice[MAXPATHLEN];
UInt32 gBlockSize = 0;
UInt32 gNextCNID = kHFSFirstUserCatalogNodeID;

time_t createtime;

int gNoCreate = FALSE;
int gWrapper = FALSE;
int gUserCatNodeSize = FALSE;

#define JOURNAL_DEFAULT_SIZE (8 * 1024 * 1024)
int gJournaled = FALSE;
char *gJournalDevice = NULL;
int gJournalSize = JOURNAL_DEFAULT_SIZE;

UInt32 catnodesiz = 8192;
UInt32 extnodesiz = 4096;
UInt32 atrnodesiz = 4096;

UInt32 catclumpblks = 0;
UInt32 extclumpblks = 0;
UInt32 atrclumpblks = 0;
UInt32 bmclumpblks = 0;
UInt32 rsrclumpblks = 0;
UInt32 datclumpblks = 0;
UInt32 freewrapperblks = 0; /* minimum free blocks to leave in wrapper */
UInt32 hfsgrowblks = 0;	    /* maximum growable size of wrapper */

int get_num(char *str) {
	int num;
	char *ptr;

	num = strtoul(str, &ptr, 0);

	if (*ptr) {
		if (tolower(*ptr) == 'k') {
			num *= 1024;
		} else if (tolower(*ptr) == 'm') {
			num *= (1024 * 1024);
		} else if (tolower(*ptr) == 'g') {
			num *= (1024 * 1024 * 1024);
		}
	}

	return num;
}

int main(int argc, char **argv) {
	extern char *optarg;
	extern int optind;
	int ch;
	int forceHFS;
	char *cp, *special;
	struct statfs *mp;
	int n;

	if (progname = strrchr(*argv, '/'))
		++progname;
	else
		progname = *argv;

	forceHFS = FALSE;

	while ((ch = getopt(argc, argv, "J:hNwb:c:i:n:v:")) != EOF) {
		switch (ch) {
			case 'J':
				gJournaled = TRUE;
				if (isdigit(optarg[0])) {
					gJournalSize = get_num(optarg);
					if (gJournalSize < 512 * 1024) {
						printf(
						    "%s: journal size %dk too "
						    "small.  Reset to %dk.\n",
						    progname,
						    gJournalSize / 1024,
						    JOURNAL_DEFAULT_SIZE /
							1024);
						gJournalSize =
						    JOURNAL_DEFAULT_SIZE;
					}
				} else {
					/* back up because there was no size
					 * argument */
					optind--;
				}
				break;

			case 'N':
				gNoCreate = TRUE;
				break;

			case 'b':
				gBlockSize = atoi(optarg);
				if (gBlockSize < HFSMINBSIZE)
					fatal("%s: bad allocation block size "
					      "(too small)",
					      optarg);
				break;

			case 'c':
				getclumpopts(optarg);
				break;

			case 'h':
				forceHFS = TRUE;
				break;

			case 'i':
				gNextCNID = atoi(optarg);
				/*
				 * make sure its at least
				 * kHFSFirstUserCatalogNodeID
				 */
				if (gNextCNID < kHFSFirstUserCatalogNodeID)
					fatal("%s: starting catalog node id "
					      "too small (must be > 15)",
					      optarg);
				break;

			case 'n':
				getnodeopts(optarg);
				break;

			case 'v':
				if (strlen(optarg) > 0)
					strcpy(gVolumeName, optarg);
				break;

			case 'w':
				gWrapper = TRUE;
				break;

			case '?':
			default:
				usage();
		}
	}

	argc -= optind;
	argv += optind;

	if (argc != 1)
		usage();

	special = argv[0];
	cp = strrchr(special, '/');
	if (cp != 0)
		special = cp + 1;
	if (*special == 'r')
		special++;
#ifdef __FreeBSD__
	(void)sprintf(rawdevice, "%s%s", _PATH_DEV, special);
#else
	(void)sprintf(rawdevice, "%sr%s", _PATH_DEV, special);
#endif
	(void)sprintf(blkdevice, "%s%s", _PATH_DEV, special);

	if (forceHFS && gJournaled) {
		fprintf(stderr, "-h -J: incompatible options specified\n");
		usage();
	}
	if (gWrapper && forceHFS) {
		fprintf(stderr, "-h -w: incompatible options specified\n");
		usage();
	}
	if (!gWrapper && (freewrapperblks || hfsgrowblks)) {
		fprintf(stderr, "f and g clump options require -w option\n");
		exit(1);
	}

	/*
	 * Check if target device is aready mounted
	 */
	n = getmntinfo(&mp, MNT_NOWAIT);
	if (n == 0)
		fatal("%s: getmntinfo: %s", blkdevice, strerror(errno));

	while (--n >= 0) {
		if (strcmp(blkdevice, mp->f_mntfromname) == 0)
			fatal("%s is mounted on %s", blkdevice,
			      mp->f_mntonname);
		++mp;
	}

	if (hfs_newfs(rawdevice, forceHFS, true) < 0) {
		/* On ENXIO error use the block device (to get de-blocking) */
		if (errno == ENXIO) {
			if (hfs_newfs(blkdevice, forceHFS, false) < 0)
				err(1, NULL);
		} else
			err(1, NULL);
	}

	exit(0);
}

static void getnodeopts(char *optlist) {
	char *strp = optlist;
	char *ndarg;
	char *p;
	UInt32 ndsize;

	while ((ndarg = strsep(&strp, ",")) != NULL && *ndarg != NULL) {

		p = strchr(ndarg, '=');
		if (p == NULL)
			usage();

		ndsize = atoi(p + 1);

		switch (*ndarg) {
			case 'c':
				if (ndsize < 4096 || ndsize > 32768 ||
				    (ndsize & ndsize - 1) != 0)
					fatal("%s: invalid catalog b-tree node "
					      "size",
					      ndarg);
				catnodesiz = ndsize;
				gUserCatNodeSize = TRUE;
				break;

			case 'e':
				if (ndsize < 1024 || ndsize > 32768 ||
				    (ndsize & ndsize - 1) != 0)
					fatal("%s: invalid extents b-tree node "
					      "size",
					      ndarg);
				extnodesiz = ndsize;
				break;

			case 'a':
				if (ndsize < 1024 || ndsize > 32768 ||
				    (ndsize & ndsize - 1) != 0)
					fatal("%s: invalid atrribute b-tree "
					      "node size",
					      ndarg);
				atrnodesiz = ndsize;
				break;

			default:
				usage();
		}
	}
}

static void getclumpopts(char *optlist) {
	char *strp = optlist;
	char *ndarg;
	char *p;
	UInt32 clpblocks;

	while ((ndarg = strsep(&strp, ",")) != NULL && *ndarg != NULL) {

		p = strchr(ndarg, '=');
		if (p == NULL)
			usage();

		clpblocks = atoi(p + 1);

		switch (*ndarg) {
			case 'a':
				atrclumpblks = clpblocks;
				break;
			case 'b':
				bmclumpblks = clpblocks;
				break;
			case 'c':
				catclumpblks = clpblocks;
				break;
			case 'd':
				datclumpblks = clpblocks;
				break;
			case 'e':
				extclumpblks = clpblocks;
				break;
			case 'f': /* free blocks to leave in wrapper */
				freewrapperblks = clpblocks;
				break;
			case 'g': /* maximum growable size of hfs wrapper */
				hfsgrowblks = clpblocks;
				break;
			case 'r':
				rsrclumpblks = clpblocks;
				break;

			default:
				usage();
		}
	}
}

/*
 * Validate the HFS Plus allocation block size in gBlockSize.  If none was
 * specified, then calculate a suitable default.
 *
 * Modifies the global variable gBlockSize.
 */
static void validate_hfsplus_block_size(UInt64 sectorCount, UInt32 sectorSize) {
	if (gBlockSize == 0) {
		/* Compute a default allocation block size based on volume size
		 */
		gBlockSize = DFL_BLKSIZE; /* Prefer the default of 4K */

		/* Use a larger power of two if total blocks would overflow 32
		 * bits */
		while ((sectorCount / (gBlockSize / sectorSize)) > 0xFFFFFFFF) {
			gBlockSize <<= 1; /* Must be a power of two */
		}
	} else {
		/* Make sure a user-specified block size is reasonable */
		if ((gBlockSize & gBlockSize - 1) != 0)
			fatal("%s: bad HFS Plus allocation block size (must be "
			      "a power of two)",
			      optarg);

		if ((sectorCount / (gBlockSize / sectorSize)) > 0xFFFFFFFF)
			fatal("%s: block size is too small for %lld sectors",
			      optarg, gBlockSize, sectorCount);

		if (gBlockSize < HFSOPTIMALBLKSIZE)
			warnx("Warning: %ld is a non-optimal block size (4096 "
			      "would be a better choice)",
			      gBlockSize);
	}
}

static int hfs_newfs(char *device, int forceHFS, int isRaw) {
	struct stat stbuf;
	DriveInfo dip;
	int fso = 0;
	int retval = 0;
	hfsparams_t defaults = {0};
#ifdef __FreeBSD__
	u_int secsize;
	off_t mediasize;
#else
	u_int64_t maxSectorsPerIO;
#endif

	if (gNoCreate) {
		fso = open(device, O_RDONLY | O_NDELAY, 0);
	} else {
		fso = open(device, O_WRONLY | O_NDELAY, 0);
	}

	if (fso < 0)
		fatal("%s: %s", device, strerror(errno));

	if (fstat(fso, &stbuf) < 0)
		fatal("%s: %s", device, strerror(errno));

#ifdef __FreeBSD__
	if (ioctl(fso, DIOCGSECTORSIZE, &secsize) < 0)
		fatal("%s: %s", device, strerror(errno));

	if (ioctl(fso, DIOCGMEDIASIZE, &mediasize) < 0)
		fatal("%s: %s", device, strerror(errno));

	dip.totalSectors = mediasize / secsize;
	dip.sectorSize = secsize;
	dip.sectorsPerIO = MAXPHYS / dip.sectorSize;
#else  /* !__FreeBSD__ */
	if (ioctl(fso, DKIOCGETBLOCKCOUNT, &dip.totalSectors) < 0)
		fatal("%s: %s", device, strerror(errno));

	if (ioctl(fso, DKIOCGETBLOCKSIZE, &dip.sectorSize) < 0)
		fatal("%s: %s", device, strerror(errno));

	if (ioctl(fso, DKIOCGETMAXBLOCKCOUNTWRITE, &maxSectorsPerIO) < 0)
		dip.sectorsPerIO =
		    (128 * 1024) / dip.sectorSize; /* use 128K as default */
	else
		dip.sectorsPerIO = maxSectorsPerIO;
#endif /* __FreeBSD__ */
	/*
	 * The make_hfs code currentlydoes 512 byte sized I/O.
	 * If the sector size is bigger than 512, start over
	 * using the block device (to get de-blocking).
	 */
	if (dip.sectorSize != kBytesPerSector) {
		if (isRaw) {
			close(fso);
			errno = ENXIO;
			return (-1);
		} else {
			if ((dip.sectorSize % kBytesPerSector) != 0)
				fatal("%d is an unsupported sector size\n",
				      dip.sectorSize);

			dip.totalSectors *= (dip.sectorSize / kBytesPerSector);
			dip.sectorsPerIO *= (dip.sectorSize / kBytesPerSector);
			dip.sectorSize = kBytesPerSector;
		}
	}

	dip.fd = fso;
	dip.sectorOffset = 0;
	time(&createtime);

	if (gWrapper && (dip.totalSectors >= kMaxWrapableSectors)) {
		gWrapper = 0;
		fprintf(stderr,
			"%s: WARNING: wrapper option ignored since volume size "
			"> 256GB\n",
			progname);
	}

	/*
	 * If we're going to make an HFS Plus disk (with or without a wrapper),
	 * validate the HFS Plus allocation block size.  This will also
	 * calculate a default allocation block size if none (or zero) was
	 * specified.
	 */
	if (!forceHFS)
		validate_hfsplus_block_size(dip.totalSectors, dip.sectorSize);

	/* Make an HFS disk */
	if (forceHFS || gWrapper) {
		hfs_params(dip.totalSectors, dip.sectorSize, &defaults);
		if (gNoCreate == 0) {
			UInt32 totalSectors, sectorOffset;

			retval = make_hfs(&dip, &defaults, &totalSectors,
					  &sectorOffset);
			if (retval)
				fatal("%s: %s", device, strerror(errno));

			if (gWrapper) {
				dip.totalSectors = totalSectors;
				dip.sectorOffset = sectorOffset;
			} else {
				printf(
				    "Initialized %s as a %ld MB HFS volume\n",
				    device, (long)(dip.totalSectors / 2048));
			}
		}
	}

	/* Make an HFS Plus disk */
	if (gWrapper || !forceHFS) {

		if ((dip.totalSectors / 2048) < MINHFSPLUSSIZEMB)
			fatal("%s: partition is too small (minimum is %d MB)",
			      device, MINHFSPLUSSIZEMB);

		/*
		 * Above 512GB, enforce partition size to be a multiple of 4K.
		 *
		 * Strictly speaking, the threshold could be as high as 1TB
		 * volume size, but this keeps us well away from any potential
		 * edge cases.  Besides, partitions this large should be 4K
		 * aligned for performance.
		 */
		if ((dip.totalSectors >= 0x40000000) && (dip.totalSectors & 7))
			fatal("%s: partition size must be a multiple of 4K",
			      device);

		hfsplus_params(dip.totalSectors, dip.sectorSize, &defaults);
		if (gNoCreate == 0) {
			retval = make_hfsplus(&dip, &defaults);
			if (retval == 0) {
				printf("Initialized %s as a %ld %s HFS Plus "
				       "volume",
				       device,
				       (dip.totalSectors > 0x2000000)
					   ? (long)((dip.totalSectors +
						     (1024 * 1024)) /
						    (2048 * 1024))
					   : (long)((dip.totalSectors + 1024) /
						    2048),
				       (dip.totalSectors > 0x2000000) ? "GB"
								      : "MB");
				if (gJournaled)
					printf(" with a %dk journal\n",
					       (int)defaults.journalSize /
						   1024);
				else
					printf("\n");
			}
		}
	}

	if (retval)
		fatal("%s: %s", device, strerror(errno));

	if (fso > 0) {
		close(fso);
	}

	return retval;
}

static void hfsplus_params(UInt64 sectorCount, UInt32 sectorSize,
			   hfsparams_t *defaults) {
	UInt32 totalBlocks;
	UInt32 minClumpSize;
	UInt32 clumpSize;
	UInt32 oddBitmapBytes;

	defaults->signature = kHFSPlusSigWord;
	defaults->flags = 0;
	defaults->blockSize = gBlockSize;
	defaults->nextFreeFileID = gNextCNID;
	defaults->createDate =
	    createtime + MAC_GMT_FACTOR; /* Mac OS GMT time */
	defaults->hfsAlignment = 0;
	defaults->journaledHFS = gJournaled;
	defaults->journalDevice = gJournalDevice;
	defaults->journalSize = gJournalSize;

	strncpy(defaults->volumeName, gVolumeName,
		sizeof(defaults->volumeName) - 1);

	if (rsrclumpblks == 0) {
		if (gBlockSize > DFL_BLKSIZE)
			defaults->rsrcClumpSize = ROUNDUP(
			    kHFSPlusRsrcClumpFactor * DFL_BLKSIZE, gBlockSize);
		else
			defaults->rsrcClumpSize =
			    kHFSPlusRsrcClumpFactor * gBlockSize;
	} else
		defaults->rsrcClumpSize = clumpsizecalc(rsrclumpblks);

	if (datclumpblks == 0) {
		if (gBlockSize > DFL_BLKSIZE)
			defaults->dataClumpSize = ROUNDUP(
			    kHFSPlusRsrcClumpFactor * DFL_BLKSIZE, gBlockSize);
		else
			defaults->dataClumpSize =
			    kHFSPlusRsrcClumpFactor * gBlockSize;
	} else
		defaults->dataClumpSize = clumpsizecalc(datclumpblks);

	/*
	 * The default  b-tree node size is 8K.  However, if the
	 * volume is small (< 1 GB) we use 4K instead.
	 */
	if (!gUserCatNodeSize) {
		if ((gBlockSize < HFSOPTIMALBLKSIZE) ||
		    ((UInt64)(sectorCount * sectorSize) < (UInt64)0x40000000))
			catnodesiz = 4096;
	}

	if (catclumpblks == 0) {
		clumpSize = CalcHFSPlusBTreeClumpSize(gBlockSize, catnodesiz,
						      sectorCount, TRUE);
	} else {
		clumpSize = clumpsizecalc(catclumpblks);

		if (clumpSize % catnodesiz != 0)
			fatal("c=%ld: clump size is not a multiple of node "
			      "size\n",
			      clumpSize / gBlockSize);
	}
	defaults->catalogClumpSize = clumpSize;
	defaults->catalogNodeSize = catnodesiz;
	if (gBlockSize < 4096 && gBlockSize < catnodesiz)
		warnx("Warning: block size %ld is less than catalog b-tree "
		      "node size %ld",
		      gBlockSize, catnodesiz);

	if (extclumpblks == 0) {
		clumpSize = CalcHFSPlusBTreeClumpSize(gBlockSize, extnodesiz,
						      sectorCount, FALSE);
	} else {
		clumpSize = clumpsizecalc(extclumpblks);
		if (clumpSize % extnodesiz != 0)
			fatal("e=%ld: clump size is not a multiple of node "
			      "size\n",
			      clumpSize / gBlockSize);
	}
	defaults->extentsClumpSize = clumpSize;
	defaults->extentsNodeSize = extnodesiz;
	if (gBlockSize < extnodesiz)
		warnx("Warning: block size %ld is less than extents b-tree "
		      "node size %ld",
		      gBlockSize, extnodesiz);

	if (atrclumpblks == 0) {
		clumpSize = 0;
	} else {
		clumpSize = clumpsizecalc(atrclumpblks);
		if (clumpSize % atrnodesiz != 0)
			fatal("a=%ld: clump size is not a multiple of node "
			      "size\n",
			      clumpSize / gBlockSize);
	}
	defaults->attributesClumpSize = clumpSize;
	defaults->attributesNodeSize = atrnodesiz;

	/*
	 * Calculate the number of blocks needed for bitmap (rounded up to a
	 * multiple of the block size).
	 */

	/*
	 * Figure out how many bytes we need for the given totalBlocks
	 * Note: this minimum value may be too large when it counts the
	 * space used by the wrapper
	 */
	totalBlocks = sectorCount / (gBlockSize / sectorSize);

	minClumpSize =
	    totalBlocks >> 3; /* convert bits to bytes by dividing by 8 */
	if (totalBlocks & 7)
		++minClumpSize; /* round up to whole bytes */

	/* Round up to a multiple of blockSize */
	if ((oddBitmapBytes = minClumpSize % gBlockSize))
		minClumpSize = minClumpSize - oddBitmapBytes + gBlockSize;

	if (bmclumpblks == 0) {
		clumpSize = minClumpSize;
	} else {
		clumpSize = clumpsizecalc(bmclumpblks);

		if (clumpSize < minClumpSize)
			fatal("b=%ld: bitmap clump size is too small\n",
			      clumpSize / gBlockSize);
	}
	defaults->allocationClumpSize = clumpSize;

	if (gNoCreate) {
		if (!gWrapper)
			printf("%qd sectors (%lu bytes per sector)\n",
			       sectorCount, sectorSize);
		printf("HFS Plus format parameters:\n");
		printf("\tvolume name: \"%s\"\n", gVolumeName);
		printf("\tblock-size: %lu\n", defaults->blockSize);
		printf("\ttotal blocks: %lu\n", totalBlocks);
		if (gJournaled)
			printf("\tjournal-size: %dk\n",
			       (int)defaults->journalSize / 1024);
		printf("\tfirst free catalog node id: %lu\n",
		       defaults->nextFreeFileID);
		printf("\tcatalog b-tree node size: %lu\n",
		       defaults->catalogNodeSize);
		printf("\tinitial catalog file size: %lu\n",
		       defaults->catalogClumpSize);
		printf("\textents b-tree node size: %lu\n",
		       defaults->extentsNodeSize);
		printf("\tinitial extents file size: %lu\n",
		       defaults->extentsClumpSize);
		printf("\tinitial allocation file size: %lu (%lu blocks)\n",
		       defaults->allocationClumpSize,
		       defaults->allocationClumpSize / gBlockSize);
		printf("\tdata fork clump size: %lu\n",
		       defaults->dataClumpSize);
		printf("\tresource fork clump size: %lu\n",
		       defaults->rsrcClumpSize);
	}
}

static void hfs_params(UInt32 sectorCount, UInt32 sectorSize,
		       hfsparams_t *defaults) {
	UInt32 alBlkSize;
	UInt32 vSectorCount;
	UInt32 defaultBlockSize;

	defaults->signature = kHFSSigWord;
	defaults->flags = 0;
	defaults->nextFreeFileID = gNextCNID;
	defaults->createDate =
	    createtime + MAC_GMT_FACTOR; /* Mac OS GMT time */
	defaults->catalogNodeSize = kHFSNodeSize;
	defaults->extentsNodeSize = kHFSNodeSize;
	defaults->attributesNodeSize = 0;
	defaults->attributesClumpSize = 0;

	strncpy(defaults->volumeName, gVolumeName,
		sizeof(defaults->volumeName) - 1);

	/* Compute the default allocation block size */
	if (gWrapper && hfsgrowblks) {
		defaults->flags |= kMakeMaxHFSBitmap;
		vSectorCount = ((UInt64)hfsgrowblks * 512) / sectorSize;
		defaultBlockSize = sectorSize * ((vSectorCount >> 16) + 1);
	} else
		defaultBlockSize = sectorSize * ((sectorCount >> 16) + 1);

	if (gWrapper) {
		defaults->flags |= kMakeHFSWrapper;

		/* round alBlkSize up to multiple of HFS Plus blockSize */
		alBlkSize = ((defaultBlockSize + gBlockSize - 1) / gBlockSize) *
			    gBlockSize;

		if (gBlockSize > 4096)
			defaults->hfsAlignment =
			    4096 / sectorSize; /* Align to 4K boundary */
		else
			defaults->hfsAlignment =
			    gBlockSize /
			    sectorSize; /* Align to blockSize boundary */
	} else {
		/* If allocation block size is undefined or invalid calculate
		 * it�*/
		alBlkSize = gBlockSize;
		defaults->hfsAlignment = 0;
	}

	if (alBlkSize == 0 || (alBlkSize & 0x1FF) != 0 ||
	    alBlkSize < defaultBlockSize)
		alBlkSize = defaultBlockSize;

	if ((alBlkSize & 0x0000FFFF) ==
	    0) /* we cannot allow the lower word to be zero! */
		alBlkSize += sectorSize; /* if it is, increase by one block */

	defaults->blockSize = alBlkSize;

	defaults->dataClumpSize = alBlkSize * 4;
	defaults->rsrcClumpSize = alBlkSize * 4;
	if (gWrapper || defaults->dataClumpSize > 0x100000)
		defaults->dataClumpSize = alBlkSize;

	if (gWrapper) {
		if (alBlkSize == kHFSNodeSize) {
			defaults->extentsClumpSize =
			    (2 * kHFSNodeSize); /* header + root/leaf */
			defaults->catalogClumpSize =
			    (4 * kHFSNodeSize); /* header + root + 2 leaves */
		} else {
			defaults->extentsClumpSize = alBlkSize;
			defaults->catalogClumpSize = alBlkSize;
		}
	} else {
		defaults->catalogClumpSize = CalcBTreeClumpSize(
		    alBlkSize, sectorSize, sectorCount, TRUE);
		defaults->extentsClumpSize = CalcBTreeClumpSize(
		    alBlkSize, sectorSize, sectorCount, FALSE);
	}

	/* convert wrapper free blocks to an hfs allocation block count */
	defaults->hfsWrapperFreeBlks =
	    ((freewrapperblks * 512) + defaults->blockSize - 1) /
	    defaults->blockSize;

	if (gNoCreate) {
		printf("%ld sectors at %ld bytes per sector\n", sectorCount,
		       sectorSize);
		printf("%s format parameters:\n",
		       gWrapper ? "HFS Wrapper" : "HFS");
		printf("\tvolume name: \"%s\"\n", gVolumeName);
		printf("\tblock-size: %ld\n", defaults->blockSize);
		printf("\ttotal blocks: %ld\n",
		       sectorCount / (alBlkSize / sectorSize));
		printf("\tfirst free catalog node id: %ld\n",
		       defaults->nextFreeFileID);
		printf("\tinitial catalog file size: %ld\n",
		       defaults->catalogClumpSize);
		printf("\tinitial extents file size: %ld\n",
		       defaults->extentsClumpSize);
		printf("\tfile clump size: %ld\n", defaults->dataClumpSize);
		if (defaults->hfsWrapperFreeBlks)
			printf("\twrapper free space: %ld\n",
			       defaults->hfsWrapperFreeBlks * alBlkSize);
		if (hfsgrowblks)
			printf("\twrapper growable from %ld to %ld sectors\n",
			       sectorCount, hfsgrowblks);
	}
}

static UInt32 clumpsizecalc(UInt32 clumpblocks) {
	UInt64 clumpsize;

	clumpsize = (UInt64)clumpblocks * (UInt64)gBlockSize;

	if (clumpsize & (UInt64)0xFFFFFFFF00000000)
		fatal("=%ld: too many blocks for clump size!", clumpblocks);

	return ((UInt32)clumpsize);
}

/*
 * CalcBTreeClumpSize
 *
 * This routine calculates the file clump size for both the catalog and
 * extents overflow files. In general, this is 1/128 the size of the
 * volume up to a maximum of 6 MB.  For really large HFS volumes it will
 * be just 1 allocation block.
 */
static UInt32 CalcBTreeClumpSize(UInt32 blockSize, UInt32 nodeSize,
				 UInt32 driveBlocks, int catalog) {
	UInt32 clumpSectors;
	UInt32 maximumClumpSectors;
	UInt32 sectorsPerBlock = blockSize >> kLog2SectorSize;
	UInt32 sectorsPerNode = nodeSize >> kLog2SectorSize;
	UInt32 nodeBitsInHeader;
	UInt32 limitClumpSectors;

	if (catalog)
		limitClumpSectors =
		    6 * 1024 * 1024 / 512; /* overall limit of 6MB */
	else
		limitClumpSectors =
		    4 * 1024 * 1024 / 512; /* overall limit of 4MB */
	/*
	 * For small node sizes (eg., HFS, or default HFS Plus extents), then
	 * the clump size will be as big as the header's map record can handle.
	 * (That is, as big as possible, without requiring a map node.)
	 *
	 * But for a 32K node size, this works out to nearly 8GB.  We need to
	 * restrict it further. To avoid arithmetic overflow, we'll calculate
	 * things in terms of 512-byte sectors.
	 */
	nodeBitsInHeader =
	    8 * (nodeSize - sizeof(BTNodeDescriptor) - sizeof(BTHeaderRec) -
		 kBTreeHeaderUserBytes - (4 * sizeof(SInt16)));
	maximumClumpSectors = nodeBitsInHeader * sectorsPerNode;

	if (maximumClumpSectors > limitClumpSectors)
		maximumClumpSectors = limitClumpSectors;

	/*
	 * For very large HFS volumes, the allocation block size might be larger
	 * than the arbitrary limit we set above.  Since we have to allocate at
	 * least one allocation block, then use that as the clump size.
	 *
	 * Otherwise, we want to use about 1/128 of the volume, again subject to
	 * the above limit. To avoid arithmetic overflow, we continue to work
	 * with sectors.
	 *
	 * But for very small volumes (less than 64K), we'll just use 4
	 * allocation blocks.  And that will typically be 2KB.
	 */
	if (sectorsPerBlock >= maximumClumpSectors) {
		clumpSectors =
		    sectorsPerBlock; /* for really large volumes just use one
					allocation block (HFS only) */
	} else {
		/*
		 * For large volumes, the default is 1/128 of the volume size,
		 * up to the maximumClumpSize
		 */
		if (driveBlocks > 128) {
			clumpSectors =
			    (driveBlocks /
			     128); /* the default is 1/128 of the volume size */

			if (clumpSectors > maximumClumpSectors)
				clumpSectors = maximumClumpSectors;
		} else {
			clumpSectors =
			    sectorsPerBlock *
			    4; /* for really small volumes (ie < 64K) */
		}
	}

	/*
	 * And we need to round up to something that is a multiple of both the
	 * node size and the allocation block size so that it will occupy a
	 * whole number of allocation blocks, and so a whole number of nodes
	 * will fit.
	 *
	 * For HFS, the node size is always 512, and the allocation block size
	 * is always a multiple of 512.  For HFS Plus, both the node size and
	 * allocation block size are powers of 2.  So, it suffices to round up
	 * to whichever value is larger (since the larger value is always a
	 * multiple of the smaller value).
	 */

	if (sectorsPerNode > sectorsPerBlock)
		clumpSectors = (clumpSectors / sectorsPerNode) *
			       sectorsPerNode; /* truncate to nearest node*/
	else
		clumpSectors = (clumpSectors / sectorsPerBlock) *
			       sectorsPerBlock; /* truncate to nearest node and
						   allocation block */

	/* Finally, convert the clump size to bytes. */
	return clumpSectors << kLog2SectorSize;
}

#define CLUMP_ENTRIES 15

short clumptbl[CLUMP_ENTRIES * 2] = {
    /*
     *	    Volume	 Catalog	 Extents
     *	     Size	Clump (MB)	Clump (MB)
     */
    /*   1GB */ 4,   4,
    /*   2GB */ 6,   4,
    /*   4GB */ 8,   4,
    /*   8GB */ 11,  5,
    /*  16GB */ 14,  5,
    /*  32GB */ 19,  6,
    /*  64GB */ 25,  7,
    /* 128GB */ 34,  8,
    /* 256GB */ 45,  9,
    /* 512GB */ 60,  11,
    /*   1TB */ 80,  14,
    /*   2TB */ 107, 16,
    /*   4TB */ 144, 20,
    /*   8TB */ 192, 25,
    /*  16TB */ 256, 32};

/*
 * CalcHFSPlusBTreeClumpSize
 *
 * This routine calculates the file clump size for either
 * the catalog file or the extents overflow file.
 */
static UInt32 CalcHFSPlusBTreeClumpSize(UInt32 blockSize, UInt32 nodeSize,
					UInt64 sectors, int catalog) {
	UInt32 mod = MAX(nodeSize, blockSize);
	UInt32 clumpSize;
	int i;

	/*
	 * The default clump size is 0.8% of the volume size. And
	 * it must also be a multiple of the node and block size.
	 */
	if (sectors < 0x200000) {
		clumpSize = sectors << 2; /*  0.8 %  */
	} else {
		/* turn exponent into table index... */
		for (i = 0, sectors = sectors >> 22;
		     sectors && (i < CLUMP_ENTRIES - 1);
		     ++i, sectors = sectors >> 1)
			;

		if (catalog)
			clumpSize = clumptbl[0 + (i) * 2] * 1024 * 1024;
		else
			clumpSize = clumptbl[1 + (i) * 2] * 1024 * 1024;
	}

	/*
	 * Round the clump size to a multiple of node of node and block size.
	 * NOTE: This rounds down.
	 */
	clumpSize /= mod;
	clumpSize *= mod;

	/*
	 * Rounding down could have rounded down to 0 if the block size was
	 * greater than the clump size.  If so, just use one block.
	 */
	if (clumpSize == 0)
		clumpSize = mod;

	return (clumpSize);
}

/* VARARGS */
void
#if __STDC__
fatal(const char *fmt, ...)
#else
    fatal(fmt, va_alist) char *fmt;
va_dcl
#endif
{
	va_list ap;

#if __STDC__
	va_start(ap, fmt);
#else
	va_start(ap);
#endif
	if (fcntl(STDERR_FILENO, F_GETFL) < 0) {
		openlog(progname, LOG_CONS, LOG_DAEMON);
		vsyslog(LOG_ERR, fmt, ap);
		closelog();
	} else {
		vwarnx(fmt, ap);
	}
	va_end(ap);
	exit(1);
	/* NOTREACHED */
}

void usage() {
	fprintf(stderr,
		"usage: %s [-h | -w] [-N] [hfsplus-options] special-device\n",
		progname);

	fprintf(stderr, "  options:\n");
	fprintf(
	    stderr,
	    "\t-h create an HFS format filesystem (HFS Plus is the default)\n");
	fprintf(stderr,
		"\t-N do not create file system, just print out parameters\n");
	fprintf(stderr,
		"\t-w add a HFS wrapper (i.e. Native Mac OS 9 bootable)\n");

	fprintf(stderr, "  where hfsplus-options are:\n");
	fprintf(stderr,
		"\t-J [journal-size] make this HFS+ volume journaled\n");
	fprintf(stderr, "\t-b allocation block size (4096 optimal)\n");
	fprintf(stderr, "\t-c clump size list (comma separated)\n");
	fprintf(stderr, "\t\te=blocks (extents file)\n");
	fprintf(stderr, "\t\tc=blocks (catalog file)\n");
	fprintf(stderr, "\t\ta=blocks (attributes file)\n");
	fprintf(stderr, "\t\tb=blocks (bitmap file)\n");
	fprintf(stderr, "\t\td=blocks (user data fork)\n");
	fprintf(stderr, "\t\tr=blocks (user resource fork)\n");
	fprintf(stderr, "\t-i starting catalog node id\n");
	fprintf(stderr, "\t-n b-tree node size list (comma separated)\n");
	fprintf(stderr, "\t\te=size (extents b-tree)\n");
	fprintf(stderr, "\t\tc=size (catalog b-tree)\n");
	fprintf(stderr, "\t\ta=size (attributes b-tree)\n");
	fprintf(stderr, "\t-v volume name (in ascii or UTF-8)\n");

	fprintf(stderr, "  examples:\n");
	fprintf(stderr, "\t%s -v Untitled /dev/rdisk0s7 \n", progname);
	fprintf(stderr, "\t%s -v Untitled -n c=4096,e=1024 /dev/rdisk0s7 \n",
		progname);
	fprintf(stderr, "\t%s -w -v Untitled -c b=64,c=1024 /dev/rdisk0s7 \n\n",
		progname);

	exit(1);
}
