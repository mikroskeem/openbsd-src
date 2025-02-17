/*	$OpenBSD: sd.c,v 1.278 2019/08/28 15:17:23 krw Exp $	*/
/*	$NetBSD: sd.c,v 1.111 1997/04/02 02:29:41 mycroft Exp $	*/

/*-
 * Copyright (c) 1998, 2003, 2004 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Charles M. Hannum.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Originally written by Julian Elischer (julian@dialix.oz.au)
 * for TRW Financial Systems for use under the MACH(2.5) operating system.
 *
 * TRW Financial Systems, in accordance with their agreement with Carnegie
 * Mellon University, makes this software available to CMU to distribute
 * or use in any manner that they see fit as long as this message is kept with
 * the software. For this reason TFS also grants any other persons or
 * organisations permission to use or modify this software.
 *
 * TFS supplies this software to be publicly redistributed
 * on the understanding that TFS is not responsible for the correct
 * functioning of this software in any circumstances.
 *
 * Ported to run under 386BSD by Julian Elischer (julian@dialix.oz.au) Sept 1992
 */

#include <sys/stdint.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/timeout.h>
#include <sys/fcntl.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mtio.h>
#include <sys/mutex.h>
#include <sys/buf.h>
#include <sys/uio.h>
#include <sys/malloc.h>
#include <sys/pool.h>
#include <sys/errno.h>
#include <sys/device.h>
#include <sys/disklabel.h>
#include <sys/disk.h>
#include <sys/conf.h>
#include <sys/scsiio.h>
#include <sys/dkio.h>
#include <sys/reboot.h>

#include <scsi/scsi_all.h>
#include <scsi/scsi_disk.h>
#include <scsi/scsiconf.h>
#include <scsi/sdvar.h>

#include <ufs/ffs/fs.h>			/* for BBSIZE and SBSIZE */

#include <sys/vnode.h>

int	sdmatch(struct device *, void *, void *);
void	sdattach(struct device *, struct device *, void *);
int	sdactivate(struct device *, int);
int	sddetach(struct device *, int);

void	sdminphys(struct buf *);
int	sdgetdisklabel(dev_t, struct sd_softc *, struct disklabel *, int);
void	sdstart(struct scsi_xfer *);
int	sd_interpret_sense(struct scsi_xfer *);
int	sd_read_cap_10(struct sd_softc *, int);
int	sd_read_cap_16(struct sd_softc *, int);
int	sd_size(struct sd_softc *, int);
int	sd_thin_pages(struct sd_softc *, int);
int	sd_vpd_block_limits(struct sd_softc *, int);
int	sd_vpd_thin(struct sd_softc *, int);
int	sd_thin_params(struct sd_softc *, int);
int	sd_get_parms(struct sd_softc *, struct disk_parms *, int);
int	sd_flush(struct sd_softc *, int);

void	viscpy(u_char *, u_char *, int);

int	sd_ioctl_inquiry(struct sd_softc *, struct dk_inquiry *);
int	sd_ioctl_cache(struct sd_softc *, long, struct dk_cache *);

void	sd_cmd_rw6(struct scsi_xfer *, int, u_int64_t, u_int);
void	sd_cmd_rw10(struct scsi_xfer *, int, u_int64_t, u_int);
void	sd_cmd_rw12(struct scsi_xfer *, int, u_int64_t, u_int);
void	sd_cmd_rw16(struct scsi_xfer *, int, u_int64_t, u_int);

void	sd_buf_done(struct scsi_xfer *);

struct cfattach sd_ca = {
	sizeof(struct sd_softc), sdmatch, sdattach,
	sddetach, sdactivate
};

struct cfdriver sd_cd = {
	NULL, "sd", DV_DISK
};

const struct scsi_inquiry_pattern sd_patterns[] = {
	{T_DIRECT, T_FIXED,
	 "",         "",                 ""},
	{T_DIRECT, T_REMOV,
	 "",         "",                 ""},
	{T_RDIRECT, T_FIXED,
	 "",         "",                 ""},
	{T_RDIRECT, T_REMOV,
	 "",         "",                 ""},
	{T_OPTICAL, T_FIXED,
	 "",         "",                 ""},
	{T_OPTICAL, T_REMOV,
	 "",         "",                 ""},
};

#define sdlookup(unit) (struct sd_softc *)disk_lookup(&sd_cd, (unit))

int
sdmatch(struct device *parent, void *match, void *aux)
{
	struct scsi_attach_args *sa = aux;
	int priority;

	(void)scsi_inqmatch(sa->sa_inqbuf,
	    sd_patterns, nitems(sd_patterns),
	    sizeof(sd_patterns[0]), &priority);

	return (priority);
}

/*
 * The routine called by the low level scsi routine when it discovers
 * a device suitable for this driver.
 */
void
sdattach(struct device *parent, struct device *self, void *aux)
{
	struct sd_softc *sc = (struct sd_softc *)self;
	struct scsi_attach_args *sa = aux;
	struct disk_parms *dp = &sc->params;
	struct scsi_link *link = sa->sa_sc_link;
	int sd_autoconf = scsi_autoconf | SCSI_SILENT |
	    SCSI_IGNORE_ILLEGAL_REQUEST | SCSI_IGNORE_MEDIA_CHANGE;
	struct dk_cache dkc;
	int error, result, sortby = BUFQ_DEFAULT;

	SC_DEBUG(link, SDEV_DB2, ("sdattach:\n"));

	/*
	 * Store information needed to contact our base driver
	 */
	sc->sc_link = link;
	link->interpret_sense = sd_interpret_sense;
	link->device_softc = sc;

	if ((link->flags & SDEV_ATAPI) && (link->flags & SDEV_REMOVABLE))
		link->quirks |= SDEV_NOSYNCCACHE;

	if (!(link->inqdata.flags & SID_RelAdr))
		link->quirks |= SDEV_ONLYBIG;

	/*
	 * Note if this device is ancient.  This is used in sdminphys().
	 */
	if (!(link->flags & SDEV_ATAPI) && SCSI0(sa->sa_inqbuf->version))
		sc->flags |= SDF_ANCIENT;

	/*
	 * Use the subdriver to request information regarding
	 * the drive. We cannot use interrupts yet, so the
	 * request must specify this.
	 */
	printf("\n");

	scsi_xsh_set(&sc->sc_xsh, link, sdstart);
	timeout_set(&sc->sc_timeout, (void (*)(void *))scsi_xsh_add,
	    &sc->sc_xsh);

	/* Spin up non-UMASS devices ready or not. */
	if ((link->flags & SDEV_UMASS) == 0)
		scsi_start(link, SSS_START, sd_autoconf);

	/*
	 * Some devices (e.g. BlackBerry Pearl) won't admit they have
	 * media loaded unless its been locked in.
	 */
	if ((link->flags & SDEV_REMOVABLE) != 0)
		scsi_prevent(link, PR_PREVENT, sd_autoconf);

	/* Check that it is still responding and ok. */
	error = scsi_test_unit_ready(sc->sc_link, TEST_READY_RETRIES * 3,
	    sd_autoconf);

	if (error)
		result = SDGP_RESULT_OFFLINE;
	else
		result = sd_get_parms(sc, &sc->params, sd_autoconf);

	if ((link->flags & SDEV_REMOVABLE) != 0)
		scsi_prevent(link, PR_ALLOW, sd_autoconf);

	switch (result) {
	case SDGP_RESULT_OK:
		printf("%s: %lluMB, %lu bytes/sector, %llu sectors",
		    sc->sc_dev.dv_xname,
		    dp->disksize / (1048576 / dp->secsize), dp->secsize,
		    dp->disksize);
		if (ISSET(sc->flags, SDF_THIN)) {
			sortby = BUFQ_FIFO;
			printf(", thin");
		}
		if (ISSET(link->flags, SDEV_READONLY)) {
			printf(", readonly");
		}
		printf("\n");
		break;

	case SDGP_RESULT_OFFLINE:
		break;

#ifdef DIAGNOSTIC
	default:
		panic("sdattach: unknown result (%#x) from get_parms", result);
		break;
#endif
	}

	/*
	 * Initialize disk structures.
	 */
	sc->sc_dk.dk_name = sc->sc_dev.dv_xname;
	bufq_init(&sc->sc_bufq, sortby);

	/*
	 * Enable write cache by default.
	 */
	memset(&dkc, 0, sizeof(dkc));
	if (sd_ioctl_cache(sc, DIOCGCACHE, &dkc) == 0 && dkc.wrcache == 0) {
		dkc.wrcache = 1;
		sd_ioctl_cache(sc, DIOCSCACHE, &dkc);
	}

	/* Attach disk. */
	disk_attach(&sc->sc_dev, &sc->sc_dk);
}

int
sdactivate(struct device *self, int act)
{
	struct scsi_link *link;
	struct sd_softc *sc = (struct sd_softc *)self;

	if (sc->flags & SDF_DYING)
		return (ENXIO);
	link = sc->sc_link;

	switch (act) {
	case DVACT_SUSPEND:
		/*
		 * We flush the cache, since we our next step before
		 * DVACT_POWERDOWN might be a hibernate operation.
		 */
		if ((sc->flags & SDF_DIRTY) != 0)
			sd_flush(sc, SCSI_AUTOCONF);
		break;
	case DVACT_POWERDOWN:
		/*
		 * Stop the disk.  Stopping the disk should flush the
		 * cache, but we are paranoid so we flush the cache
		 * first.  We're cold at this point, so we poll for
		 * completion.
		 */
		if ((sc->flags & SDF_DIRTY) != 0)
			sd_flush(sc, SCSI_AUTOCONF);
		if (boothowto & RB_POWERDOWN)
			scsi_start(link, SSS_STOP,
			    SCSI_IGNORE_ILLEGAL_REQUEST |
			    SCSI_IGNORE_NOT_READY | SCSI_AUTOCONF);
		break;
	case DVACT_RESUME:
		scsi_start(link, SSS_START,
		    SCSI_IGNORE_ILLEGAL_REQUEST | SCSI_AUTOCONF);
		break;
	case DVACT_DEACTIVATE:
		sc->flags |= SDF_DYING;
		timeout_del(&sc->sc_timeout);
		scsi_xsh_del(&sc->sc_xsh);
		break;
	}
	return (0);
}

int
sddetach(struct device *self, int flags)
{
	struct sd_softc *sc = (struct sd_softc *)self;

	bufq_drain(&sc->sc_bufq);

	disk_gone(sdopen, self->dv_unit);

	/* Detach disk. */
	bufq_destroy(&sc->sc_bufq);
	disk_detach(&sc->sc_dk);

	return (0);
}

/*
 * Open the device. Make sure the partition info is as up-to-date as can be.
 */
int
sdopen(dev_t dev, int flag, int fmt, struct proc *p)
{
	struct scsi_link *link;
	struct sd_softc *sc;
	int error = 0, part, rawopen, unit;

	unit = DISKUNIT(dev);
	part = DISKPART(dev);

	rawopen = (part == RAW_PART) && (fmt == S_IFCHR);

	sc = sdlookup(unit);
	if (sc == NULL)
		return (ENXIO);
	if (sc->flags & SDF_DYING) {
		device_unref(&sc->sc_dev);
		return (ENXIO);
	}
	link = sc->sc_link;

	if (ISSET(flag, FWRITE) && ISSET(link->flags, SDEV_READONLY)) {
		device_unref(&sc->sc_dev);
		return (EACCES);
	}

	SC_DEBUG(link, SDEV_DB1,
	    ("sdopen: dev=0x%x (unit %d (of %d), partition %d)\n", dev, unit,
	    sd_cd.cd_ndevs, part));

	if ((error = disk_lock(&sc->sc_dk)) != 0) {
		device_unref(&sc->sc_dev);
		return (error);
	}

	if (sc->sc_dk.dk_openmask != 0) {
		/*
		 * If any partition is open, but the disk has been invalidated,
		 * disallow further opens of non-raw partition.
		 */
		if (sc->flags & SDF_DYING) {
			error = ENXIO;
			goto die;
		}
		if ((link->flags & SDEV_MEDIA_LOADED) == 0) {
			if (rawopen)
				goto out;
			error = EIO;
			goto bad;
		}
	} else {
		/* Spin up non-UMASS devices ready or not. */
		if (sc->flags & SDF_DYING) {
			error = ENXIO;
			goto die;
		}
		if ((link->flags & SDEV_UMASS) == 0)
			scsi_start(link, SSS_START, (rawopen ? SCSI_SILENT :
			    0) | SCSI_IGNORE_ILLEGAL_REQUEST |
			    SCSI_IGNORE_MEDIA_CHANGE);

		/* Use sd_interpret_sense() for sense errors.
		 *
		 * But only after spinning the disk up! Just in case a broken
		 * device returns "Initialization command required." and causes
		 * a loop of scsi_start() calls.
		 */
		if (sc->flags & SDF_DYING) {
			error = ENXIO;
			goto die;
		}
		link->flags |= SDEV_OPEN;

		/*
		 * Try to prevent the unloading of a removable device while
		 * it's open. But allow the open to proceed if the device can't
		 * be locked in.
		 */
		if ((link->flags & SDEV_REMOVABLE) != 0) {
			scsi_prevent(link, PR_PREVENT, SCSI_SILENT |
			    SCSI_IGNORE_ILLEGAL_REQUEST |
			    SCSI_IGNORE_MEDIA_CHANGE);
		}

		/* Check that it is still responding and ok. */
		if (sc->flags & SDF_DYING) {
			error = ENXIO;
			goto die;
		}
		error = scsi_test_unit_ready(link,
		    TEST_READY_RETRIES, SCSI_SILENT |
		    SCSI_IGNORE_ILLEGAL_REQUEST | SCSI_IGNORE_MEDIA_CHANGE);
		if (error) {
			if (rawopen) {
				error = 0;
				goto out;
			} else
				goto bad;
		}

		/* Load the physical device parameters. */
		if (sc->flags & SDF_DYING) {
			error = ENXIO;
			goto die;
		}
		link->flags |= SDEV_MEDIA_LOADED;
		if (sd_get_parms(sc, &sc->params, (rawopen ? SCSI_SILENT : 0))
		    == SDGP_RESULT_OFFLINE) {
			if (sc->flags & SDF_DYING) {
				error = ENXIO;
				goto die;
			}
			link->flags &= ~SDEV_MEDIA_LOADED;
			error = ENXIO;
			goto bad;
		}
		SC_DEBUG(link, SDEV_DB3, ("Params loaded\n"));

		/* Load the partition info if not already loaded. */
		error = sdgetdisklabel(dev, sc, sc->sc_dk.dk_label, 0);
		if (error == EIO || error == ENXIO)
			goto bad;
		SC_DEBUG(link, SDEV_DB3, ("Disklabel loaded\n"));
	}

out:
	if ((error = disk_openpart(&sc->sc_dk, part, fmt, 1)) != 0)
		goto bad;

	SC_DEBUG(link, SDEV_DB3, ("open complete\n"));

	/* It's OK to fall through because dk_openmask is now non-zero. */
bad:
	if (sc->sc_dk.dk_openmask == 0) {
		if (sc->flags & SDF_DYING) {
			error = ENXIO;
			goto die;
		}
		if ((link->flags & SDEV_REMOVABLE) != 0)
			scsi_prevent(link, PR_ALLOW, SCSI_SILENT |
			    SCSI_IGNORE_ILLEGAL_REQUEST |
			    SCSI_IGNORE_MEDIA_CHANGE);
		if (sc->flags & SDF_DYING) {
			error = ENXIO;
			goto die;
		}
		link->flags &= ~(SDEV_OPEN | SDEV_MEDIA_LOADED);
	}

die:
	disk_unlock(&sc->sc_dk);
	device_unref(&sc->sc_dev);
	return (error);
}

/*
 * Close the device. Only called if we are the last occurrence of an open
 * device.  Convenient now but usually a pain.
 */
int
sdclose(dev_t dev, int flag, int fmt, struct proc *p)
{
	struct scsi_link *link;
	struct sd_softc *sc;
	int part = DISKPART(dev);
	int error = 0;

	sc = sdlookup(DISKUNIT(dev));
	if (sc == NULL)
		return (ENXIO);
	if (sc->flags & SDF_DYING) {
		device_unref(&sc->sc_dev);
		return (ENXIO);
	}
	link = sc->sc_link;

	disk_lock_nointr(&sc->sc_dk);

	disk_closepart(&sc->sc_dk, part, fmt);

	if (((flag & FWRITE) != 0 || sc->sc_dk.dk_openmask == 0) &&
	    (sc->flags & SDF_DIRTY) != 0)
		sd_flush(sc, 0);

	if (sc->sc_dk.dk_openmask == 0) {
		if (sc->flags & SDF_DYING) {
			error = ENXIO;
			goto die;
		}
		if ((link->flags & SDEV_REMOVABLE) != 0)
			scsi_prevent(link, PR_ALLOW,
			    SCSI_IGNORE_ILLEGAL_REQUEST |
			    SCSI_IGNORE_NOT_READY | SCSI_SILENT);
		if (sc->flags & SDF_DYING) {
			error = ENXIO;
			goto die;
		}
		link->flags &= ~(SDEV_OPEN | SDEV_MEDIA_LOADED);

		if (link->flags & SDEV_EJECTING) {
			scsi_start(link, SSS_STOP|SSS_LOEJ, 0);
			if (sc->flags & SDF_DYING) {
				error = ENXIO;
				goto die;
			}
			link->flags &= ~SDEV_EJECTING;
		}

		timeout_del(&sc->sc_timeout);
		scsi_xsh_del(&sc->sc_xsh);
	}

die:
	disk_unlock(&sc->sc_dk);
	device_unref(&sc->sc_dev);
	return (error);
}

/*
 * Actually translate the requested transfer into one the physical driver
 * can understand.  The transfer is described by a buf and will include
 * only one physical transfer.
 */
void
sdstrategy(struct buf *bp)
{
	struct scsi_link *link;
	struct sd_softc *sc;
	int s;

	sc = sdlookup(DISKUNIT(bp->b_dev));
	if (sc == NULL) {
		bp->b_error = ENXIO;
		goto bad;
	}
	if (sc->flags & SDF_DYING) {
		bp->b_error = ENXIO;
		goto bad;
	}
	link = sc->sc_link;

	SC_DEBUG(link, SDEV_DB2, ("sdstrategy: %ld bytes @ blk %lld\n",
	    bp->b_bcount, (long long)bp->b_blkno));
	/*
	 * If the device has been made invalid, error out
	 */
	if ((link->flags & SDEV_MEDIA_LOADED) == 0) {
		if (link->flags & SDEV_OPEN)
			bp->b_error = EIO;
		else
			bp->b_error = ENODEV;
		goto bad;
	}

	/* Validate the request. */
	if (bounds_check_with_label(bp, sc->sc_dk.dk_label) == -1)
		goto done;

	/* Place it in the queue of disk activities for this disk. */
	bufq_queue(&sc->sc_bufq, bp);

	/*
	 * Tell the device to get going on the transfer if it's
	 * not doing anything, otherwise just wait for completion
	 */
	scsi_xsh_add(&sc->sc_xsh);

	device_unref(&sc->sc_dev);
	return;

bad:
	SET(bp->b_flags, B_ERROR);
	bp->b_resid = bp->b_bcount;
done:
	s = splbio();
	biodone(bp);
	splx(s);
	if (sc != NULL)
		device_unref(&sc->sc_dev);
}

void
sd_cmd_rw6(struct scsi_xfer *xs, int read, u_int64_t secno, u_int nsecs)
{
	struct scsi_rw *cmd = (struct scsi_rw *)xs->cmd;

	cmd->opcode = read ? READ_COMMAND : WRITE_COMMAND;
	_lto3b(secno, cmd->addr);
	cmd->length = nsecs;

	xs->cmdlen = sizeof(*cmd);
}

void
sd_cmd_rw10(struct scsi_xfer *xs, int read, u_int64_t secno, u_int nsecs)
{
	struct scsi_rw_big *cmd = (struct scsi_rw_big *)xs->cmd;

	cmd->opcode = read ? READ_BIG : WRITE_BIG;
	_lto4b(secno, cmd->addr);
	_lto2b(nsecs, cmd->length);

	xs->cmdlen = sizeof(*cmd);
}

void
sd_cmd_rw12(struct scsi_xfer *xs, int read, u_int64_t secno, u_int nsecs)
{
	struct scsi_rw_12 *cmd = (struct scsi_rw_12 *)xs->cmd;

	cmd->opcode = read ? READ_12 : WRITE_12;
	_lto4b(secno, cmd->addr);
	_lto4b(nsecs, cmd->length);

	xs->cmdlen = sizeof(*cmd);
}

void
sd_cmd_rw16(struct scsi_xfer *xs, int read, u_int64_t secno, u_int nsecs)
{
	struct scsi_rw_16 *cmd = (struct scsi_rw_16 *)xs->cmd;

	cmd->opcode = read ? READ_16 : WRITE_16;
	_lto8b(secno, cmd->addr);
	_lto4b(nsecs, cmd->length);

	xs->cmdlen = sizeof(*cmd);
}

/*
 * sdstart looks to see if there is a buf waiting for the device
 * and that the device is not already busy. If both are true,
 * It dequeues the buf and creates a scsi command to perform the
 * transfer in the buf. The transfer request will call scsi_done
 * on completion, which will in turn call this routine again
 * so that the next queued transfer is performed.
 * The bufs are queued by the strategy routine (sdstrategy)
 *
 * This routine is also called after other non-queued requests
 * have been made of the scsi driver, to ensure that the queue
 * continues to be drained.
 */
void
sdstart(struct scsi_xfer *xs)
{
	struct scsi_link *link = xs->sc_link;
	struct sd_softc *sc = link->device_softc;
	struct buf *bp;
	u_int64_t secno;
	int nsecs;
	int read;
	struct partition *p;

	if (sc->flags & SDF_DYING) {
		scsi_xs_put(xs);
		return;
	}
	if ((link->flags & SDEV_MEDIA_LOADED) == 0) {
		bufq_drain(&sc->sc_bufq);
		scsi_xs_put(xs);
		return;
	}

	bp = bufq_dequeue(&sc->sc_bufq);
	if (bp == NULL) {
		scsi_xs_put(xs);
		return;
	}

	secno = DL_BLKTOSEC(sc->sc_dk.dk_label, bp->b_blkno);

	p = &sc->sc_dk.dk_label->d_partitions[DISKPART(bp->b_dev)];
	secno += DL_GETPOFFSET(p);
	nsecs = howmany(bp->b_bcount, sc->sc_dk.dk_label->d_secsize);
	read = bp->b_flags & B_READ;

	/*
	 *  Fill out the scsi command.  If the transfer will
	 *  fit in a "small" cdb, use it.
	 */
	if (!(link->flags & SDEV_ATAPI) &&
	    !(link->quirks & SDEV_ONLYBIG) &&
	    ((secno & 0x1fffff) == secno) &&
	    ((nsecs & 0xff) == nsecs))
		sd_cmd_rw6(xs, read, secno, nsecs);
	else if (((secno & 0xffffffff) == secno) &&
	    ((nsecs & 0xffff) == nsecs))
		sd_cmd_rw10(xs, read, secno, nsecs);
	else if (((secno & 0xffffffff) == secno) &&
	    ((nsecs & 0xffffffff) == nsecs))
		sd_cmd_rw12(xs, read, secno, nsecs);
	else
		sd_cmd_rw16(xs, read, secno, nsecs);

	xs->flags |= (read ? SCSI_DATA_IN : SCSI_DATA_OUT);
	xs->timeout = 60000;
	xs->data = bp->b_data;
	xs->datalen = bp->b_bcount;

	xs->done = sd_buf_done;
	xs->cookie = bp;
	xs->bp = bp;

	/* Instrumentation. */
	disk_busy(&sc->sc_dk);

	/* Mark disk as dirty. */
	if (!read)
		sc->flags |= SDF_DIRTY;

	scsi_xs_exec(xs);

	/* move onto the next io */
	if (ISSET(sc->flags, SDF_WAITING))
		CLR(sc->flags, SDF_WAITING);
	else if (bufq_peek(&sc->sc_bufq))
		scsi_xsh_add(&sc->sc_xsh);
}

void
sd_buf_done(struct scsi_xfer *xs)
{
	struct sd_softc *sc = xs->sc_link->device_softc;
	struct buf *bp = xs->cookie;
	int error, s;

	switch (xs->error) {
	case XS_NOERROR:
		bp->b_error = 0;
		CLR(bp->b_flags, B_ERROR);
		bp->b_resid = xs->resid;
		break;

	case XS_SENSE:
	case XS_SHORTSENSE:
#ifdef SCSIDEBUG
		scsi_sense_print_debug(xs);
#endif
		error = sd_interpret_sense(xs);
		if (error == 0) {
			bp->b_error = 0;
			CLR(bp->b_flags, B_ERROR);
			bp->b_resid = xs->resid;
			break;
		}
		if (error != ERESTART) {
			bp->b_error = error;
			SET(bp->b_flags, B_ERROR);
			xs->retries = 0;
		}
		goto retry;

	case XS_BUSY:
		if (xs->retries) {
			if (scsi_delay(xs, 1) != ERESTART)
				xs->retries = 0;
		}
		goto retry;

	case XS_TIMEOUT:
retry:
		if (xs->retries--) {
			scsi_xs_exec(xs);
			return;
		}
		/* FALLTHROUGH */

	default:
		if (bp->b_error == 0)
			bp->b_error = EIO;
		SET(bp->b_flags, B_ERROR);
		bp->b_resid = bp->b_bcount;
		break;
	}

	disk_unbusy(&sc->sc_dk, bp->b_bcount - xs->resid, bp->b_blkno,
	    bp->b_flags & B_READ);

	s = splbio();
	biodone(bp);
	splx(s);
	scsi_xs_put(xs);
}

void
sdminphys(struct buf *bp)
{
	struct scsi_link *link;
	struct sd_softc *sc;
	long max;

	sc = sdlookup(DISKUNIT(bp->b_dev));
	if (sc == NULL)
		return;  /* XXX - right way to fail this? */
	if (sc->flags & SDF_DYING) {
		device_unref(&sc->sc_dev);
		return;
	}
	link = sc->sc_link;

	/*
	 * If the device is ancient, we want to make sure that
	 * the transfer fits into a 6-byte cdb.
	 *
	 * XXX Note that the SCSI-I spec says that 256-block transfers
	 * are allowed in a 6-byte read/write, and are specified
	 * by setting the "length" to 0.  However, we're conservative
	 * here, allowing only 255-block transfers in case an
	 * ancient device gets confused by length == 0.  A length of 0
	 * in a 10-byte read/write actually means 0 blocks.
	 */
	if (sc->flags & SDF_ANCIENT) {
		max = sc->sc_dk.dk_label->d_secsize * 0xff;

		if (bp->b_bcount > max)
			bp->b_bcount = max;
	}

	(*link->adapter->scsi_minphys)(bp, link);

	device_unref(&sc->sc_dev);
}

int
sdread(dev_t dev, struct uio *uio, int ioflag)
{
	return (physio(sdstrategy, dev, B_READ, sdminphys, uio));
}

int
sdwrite(dev_t dev, struct uio *uio, int ioflag)
{
	return (physio(sdstrategy, dev, B_WRITE, sdminphys, uio));
}

/*
 * Perform special action on behalf of the user
 * Knows about the internals of this device
 */
int
sdioctl(dev_t dev, u_long cmd, caddr_t addr, int flag, struct proc *p)
{
	struct scsi_link *link;
	struct sd_softc *sc;
	struct disklabel *lp;
	int error = 0;
	int part = DISKPART(dev);

	sc = sdlookup(DISKUNIT(dev));
	if (sc == NULL)
		return (ENXIO);
	if (sc->flags & SDF_DYING) {
		device_unref(&sc->sc_dev);
		return (ENXIO);
	}
	link = sc->sc_link;

	SC_DEBUG(link, SDEV_DB2, ("sdioctl 0x%lx\n", cmd));

	/*
	 * If the device is not valid.. abandon ship
	 */
	if ((link->flags & SDEV_MEDIA_LOADED) == 0) {
		switch (cmd) {
		case DIOCLOCK:
		case DIOCEJECT:
		case SCIOCIDENTIFY:
		case SCIOCCOMMAND:
		case SCIOCDEBUG:
			if (part == RAW_PART)
				break;
		/* FALLTHROUGH */
		default:
			if ((link->flags & SDEV_OPEN) == 0) {
				error = ENODEV;
				goto exit;
			} else {
				error = EIO;
				goto exit;
			}
		}
	}

	switch (cmd) {
	case DIOCRLDINFO:
		lp = malloc(sizeof(*lp), M_TEMP, M_WAITOK);
		sdgetdisklabel(dev, sc, lp, 0);
		memcpy(sc->sc_dk.dk_label, lp, sizeof(*lp));
		free(lp, M_TEMP, sizeof(*lp));
		goto exit;

	case DIOCGPDINFO:
		sdgetdisklabel(dev, sc, (struct disklabel *)addr, 1);
		goto exit;

	case DIOCGDINFO:
		*(struct disklabel *)addr = *(sc->sc_dk.dk_label);
		goto exit;

	case DIOCGPART:
		((struct partinfo *)addr)->disklab = sc->sc_dk.dk_label;
		((struct partinfo *)addr)->part =
		    &sc->sc_dk.dk_label->d_partitions[DISKPART(dev)];
		goto exit;

	case DIOCWDINFO:
	case DIOCSDINFO:
		if ((flag & FWRITE) == 0) {
			error = EBADF;
			goto exit;
		}

		if ((error = disk_lock(&sc->sc_dk)) != 0)
			goto exit;

		error = setdisklabel(sc->sc_dk.dk_label,
		    (struct disklabel *)addr, sc->sc_dk.dk_openmask);
		if (error == 0) {
			if (cmd == DIOCWDINFO)
				error = writedisklabel(DISKLABELDEV(dev),
				    sdstrategy, sc->sc_dk.dk_label);
		}

		disk_unlock(&sc->sc_dk);
		goto exit;

	case DIOCLOCK:
		error = scsi_prevent(link,
		    (*(int *)addr) ? PR_PREVENT : PR_ALLOW, 0);
		goto exit;

	case MTIOCTOP:
		if (((struct mtop *)addr)->mt_op != MTOFFL) {
			error = EIO;
			goto exit;
		}
		/* FALLTHROUGH */
	case DIOCEJECT:
		if ((link->flags & SDEV_REMOVABLE) == 0) {
			error = ENOTTY;
			goto exit;
		}
		link->flags |= SDEV_EJECTING;
		goto exit;

	case DIOCINQ:
		error = scsi_do_ioctl(link, cmd, addr, flag);
		if (error == ENOTTY)
			error = sd_ioctl_inquiry(sc,
			    (struct dk_inquiry *)addr);
		goto exit;

	case DIOCSCACHE:
		if (!ISSET(flag, FWRITE)) {
			error = EBADF;
			goto exit;
		}
		/* FALLTHROUGH */
	case DIOCGCACHE:
		error = sd_ioctl_cache(sc, cmd, (struct dk_cache *)addr);
		goto exit;

	case DIOCCACHESYNC:
		if (!ISSET(flag, FWRITE)) {
			error = EBADF;
			goto exit;
		}
		if ((sc->flags & SDF_DIRTY) != 0 || *(int *)addr != 0)
			error = sd_flush(sc, 0);
		goto exit;

	default:
		if (part != RAW_PART) {
			error = ENOTTY;
			goto exit;
		}
		error = scsi_do_ioctl(link, cmd, addr, flag);
	}

 exit:
	device_unref(&sc->sc_dev);
	return (error);
}

int
sd_ioctl_inquiry(struct sd_softc *sc, struct dk_inquiry *di)
{
	struct scsi_link *link;
	struct scsi_vpd_serial *vpd;

	vpd = dma_alloc(sizeof(*vpd), PR_WAITOK | PR_ZERO);

	if (sc->flags & SDF_DYING) {
		dma_free(vpd, sizeof(*vpd));
		return (ENXIO);
	}
	link = sc->sc_link;

	bzero(di, sizeof(struct dk_inquiry));
	scsi_strvis(di->vendor, link->inqdata.vendor,
	    sizeof(link->inqdata.vendor));
	scsi_strvis(di->product, link->inqdata.product,
	    sizeof(link->inqdata.product));
	scsi_strvis(di->revision, link->inqdata.revision,
	    sizeof(link->inqdata.revision));

	/* the serial vpd page is optional */
	if (scsi_inquire_vpd(link, vpd, sizeof(*vpd), SI_PG_SERIAL, 0) == 0)
		scsi_strvis(di->serial, vpd->serial, sizeof(vpd->serial));
	else
		strlcpy(di->serial, "(unknown)", sizeof(vpd->serial));

	dma_free(vpd, sizeof(*vpd));
	return (0);
}

int
sd_ioctl_cache(struct sd_softc *sc, long cmd, struct dk_cache *dkc)
{
	struct scsi_link *link;
	union scsi_mode_sense_buf *buf;
	struct page_caching_mode *mode = NULL;
	u_int wrcache, rdcache;
	int big;
	int rv;

	if (sc->flags & SDF_DYING)
		return (ENXIO);
	link = sc->sc_link;

	if (ISSET(link->flags, SDEV_UMASS))
		return (EOPNOTSUPP);

	/* see if the adapter has special handling */
	rv = scsi_do_ioctl(link, cmd, (caddr_t)dkc, 0);
	if (rv != ENOTTY)
		return (rv);

	buf = dma_alloc(sizeof(*buf), PR_WAITOK);
	if (buf == NULL)
		return (ENOMEM);

	if (sc->flags & SDF_DYING) {
		rv = ENXIO;
		goto done;
	}
	rv = scsi_do_mode_sense(link, PAGE_CACHING_MODE,
	    buf, (void **)&mode, NULL, NULL, NULL,
	    sizeof(*mode) - 4, scsi_autoconf | SCSI_SILENT, &big);
	if (rv != 0)
		goto done;

	if ((mode == NULL) || (!DISK_PGCODE(mode, PAGE_CACHING_MODE))) {
		rv = EIO;
		goto done;
	}

	wrcache = (ISSET(mode->flags, PG_CACHE_FL_WCE) ? 1 : 0);
	rdcache = (ISSET(mode->flags, PG_CACHE_FL_RCD) ? 0 : 1);

	switch (cmd) {
	case DIOCGCACHE:
		dkc->wrcache = wrcache;
		dkc->rdcache = rdcache;
		break;

	case DIOCSCACHE:
		if (dkc->wrcache == wrcache && dkc->rdcache == rdcache)
			break;

		if (dkc->wrcache)
			SET(mode->flags, PG_CACHE_FL_WCE);
		else
			CLR(mode->flags, PG_CACHE_FL_WCE);

		if (dkc->rdcache)
			CLR(mode->flags, PG_CACHE_FL_RCD);
		else
			SET(mode->flags, PG_CACHE_FL_RCD);

		if (sc->flags & SDF_DYING) {
			rv = ENXIO;
			goto done;
		}
		if (big) {
			rv = scsi_mode_select_big(link, SMS_PF,
			    &buf->hdr_big, scsi_autoconf | SCSI_SILENT, 20000);
		} else {
			rv = scsi_mode_select(link, SMS_PF,
			    &buf->hdr, scsi_autoconf | SCSI_SILENT, 20000);
		}
		break;
	}

done:
	dma_free(buf, sizeof(*buf));
	return (rv);
}

/*
 * Load the label information on the named device
 */
int
sdgetdisklabel(dev_t dev, struct sd_softc *sc, struct disklabel *lp,
    int spoofonly)
{
	struct scsi_link *link;
	size_t len;
	char packname[sizeof(lp->d_packname) + 1];
	char product[17], vendor[9];

	if (sc->flags & SDF_DYING)
		return (ENXIO);
	link = sc->sc_link;

	bzero(lp, sizeof(struct disklabel));

	lp->d_secsize = sc->params.secsize;
	lp->d_ntracks = sc->params.heads;
	lp->d_nsectors = sc->params.sectors;
	lp->d_ncylinders = sc->params.cyls;
	lp->d_secpercyl = lp->d_ntracks * lp->d_nsectors;
	if (lp->d_secpercyl == 0) {
		lp->d_secpercyl = 100;
		/* as long as it's not 0 - readdisklabel divides by it */
	}

	lp->d_type = DTYPE_SCSI;
	if ((link->inqdata.device & SID_TYPE) == T_OPTICAL)
		strncpy(lp->d_typename, "SCSI optical",
		    sizeof(lp->d_typename));
	else
		strncpy(lp->d_typename, "SCSI disk",
		    sizeof(lp->d_typename));

	/*
	 * Try to fit '<vendor> <product>' into d_packname. If that doesn't fit
	 * then leave out '<vendor> ' and use only as much of '<product>' as
	 * does fit.
	 */
	viscpy(vendor, link->inqdata.vendor, 8);
	viscpy(product, link->inqdata.product, 16);
	len = snprintf(packname, sizeof(packname), "%s %s", vendor, product);
	if (len > sizeof(lp->d_packname)) {
		strlcpy(packname, product, sizeof(packname));
		len = strlen(packname);
	}
	/*
	 * It is safe to use len as the count of characters to copy because
	 * packname is sizeof(lp->d_packname)+1, the string in packname is
	 * always null terminated and len does not count the terminating null.
	 * d_packname is not a null terminated string.
	 */
	memcpy(lp->d_packname, packname, len);

	DL_SETDSIZE(lp, sc->params.disksize);
	lp->d_version = 1;
	lp->d_flags = 0;

	/* XXX - these values for BBSIZE and SBSIZE assume ffs */
	lp->d_bbsize = BBSIZE;
	lp->d_sbsize = SBSIZE;

	lp->d_magic = DISKMAGIC;
	lp->d_magic2 = DISKMAGIC;
	lp->d_checksum = dkcksum(lp);

	/*
	 * Call the generic disklabel extraction routine
	 */
	return readdisklabel(DISKLABELDEV(dev), sdstrategy, lp, spoofonly);
}


/*
 * Check Errors
 */
int
sd_interpret_sense(struct scsi_xfer *xs)
{
	struct scsi_sense_data *sense = &xs->sense;
	struct scsi_link *link = xs->sc_link;
	u_int8_t serr = sense->error_code & SSD_ERRCODE;
	int retval;

	/*
	 * Let the generic code handle everything except a few categories of
	 * LUN not ready errors on open devices.
	 */
	if (((link->flags & SDEV_OPEN) == 0) ||
	    (serr != SSD_ERRCODE_CURRENT && serr != SSD_ERRCODE_DEFERRED) ||
	    ((sense->flags & SSD_KEY) != SKEY_NOT_READY) ||
	    (sense->extra_len < 6))
		return (scsi_interpret_sense(xs));

	if ((xs->flags & SCSI_IGNORE_NOT_READY) != 0)
		return (0);

	switch (ASC_ASCQ(sense)) {
	case SENSE_NOT_READY_BECOMING_READY:
		SC_DEBUG(link, SDEV_DB1, ("becoming ready.\n"));
		retval = scsi_delay(xs, 5);
		break;

	case SENSE_NOT_READY_INIT_REQUIRED:
		SC_DEBUG(link, SDEV_DB1, ("spinning up\n"));
		retval = scsi_start(link, SSS_START,
		    SCSI_IGNORE_ILLEGAL_REQUEST | SCSI_NOSLEEP);
		if (retval == 0)
			retval = ERESTART;
		else if (retval == ENOMEM)
			/* Can't issue the command. Fall back on a delay. */
			retval = scsi_delay(xs, 5);
		else
			SC_DEBUG(link, SDEV_DB1, ("spin up failed (%#x)\n",
			    retval));
		break;

	default:
		retval = scsi_interpret_sense(xs);
		break;
	}

	return (retval);
}

daddr_t
sdsize(dev_t dev)
{
	struct disklabel *lp;
	struct sd_softc *sc;
	int part, omask;
	daddr_t size;

	sc = sdlookup(DISKUNIT(dev));
	if (sc == NULL)
		return -1;
	if (sc->flags & SDF_DYING) {
		size = -1;
		goto exit;
	}

	part = DISKPART(dev);
	omask = sc->sc_dk.dk_openmask & (1 << part);

	if (omask == 0 && sdopen(dev, 0, S_IFBLK, NULL) != 0) {
		size = -1;
		goto exit;
	}

	lp = sc->sc_dk.dk_label;
	if (sc->flags & SDF_DYING) {
		size = -1;
		goto exit;
	}
	if ((sc->sc_link->flags & SDEV_MEDIA_LOADED) == 0)
		size = -1;
	else if (lp->d_partitions[part].p_fstype != FS_SWAP)
		size = -1;
	else
		size = DL_SECTOBLK(lp, DL_GETPSIZE(&lp->d_partitions[part]));
	if (omask == 0 && sdclose(dev, 0, S_IFBLK, NULL) != 0)
		size = -1;

 exit:
	device_unref(&sc->sc_dev);
	return size;
}

/* #define SD_DUMP_NOT_TRUSTED if you just want to watch */
static int sddoingadump;

/*
 * dump all of physical memory into the partition specified, starting
 * at offset 'dumplo' into the partition.
 */
int
sddump(dev_t dev, daddr_t blkno, caddr_t va, size_t size)
{
	struct sd_softc *sc;	/* disk unit to do the I/O */
	struct disklabel *lp;	/* disk's disklabel */
	int	unit, part;
	u_int32_t sectorsize;	/* size of a disk sector */
	u_int64_t nsects;	/* number of sectors in partition */
	u_int64_t sectoff;	/* sector offset of partition */
	u_int64_t totwrt;	/* total number of sectors left to write */
	u_int32_t nwrt;		/* current number of sectors to write */
	struct scsi_xfer *xs;	/* ... convenience */
	int rv;

	/* Check if recursive dump; if so, punt. */
	if (sddoingadump)
		return EFAULT;
	if (blkno < 0)
		return EINVAL;

	/* Mark as active early. */
	sddoingadump = 1;

	unit = DISKUNIT(dev);	/* Decompose unit & partition. */
	part = DISKPART(dev);

	/* Check for acceptable drive number. */
	if (unit >= sd_cd.cd_ndevs || (sc = sd_cd.cd_devs[unit]) == NULL)
		return ENXIO;

	/*
	 * XXX Can't do this check, since the media might have been
	 * XXX marked `invalid' by successful unmounting of all
	 * XXX filesystems.
	 */
#if 0
	/* Make sure it was initialized. */
	if ((sc->sc_link->flags & SDEV_MEDIA_LOADED) != SDEV_MEDIA_LOADED)
		return ENXIO;
#endif

	/* Convert to disk sectors.  Request must be a multiple of size. */
	lp = sc->sc_dk.dk_label;
	sectorsize = lp->d_secsize;
	if ((size % sectorsize) != 0)
		return EFAULT;
	if ((blkno % DL_BLKSPERSEC(lp)) != 0)
		return EFAULT;
	totwrt = size / sectorsize;
	blkno = DL_BLKTOSEC(lp, blkno);

	nsects = DL_GETPSIZE(&lp->d_partitions[part]);
	sectoff = DL_GETPOFFSET(&lp->d_partitions[part]);

	/* Check transfer bounds against partition size. */
	if ((blkno + totwrt) > nsects)
		return EINVAL;

	/* Offset block number to start of partition. */
	blkno += sectoff;

	while (totwrt > 0) {
		if (totwrt > UINT32_MAX)
			nwrt = UINT32_MAX;
		else
			nwrt = totwrt;

#ifndef	SD_DUMP_NOT_TRUSTED
		xs = scsi_xs_get(sc->sc_link, SCSI_NOSLEEP);
		if (xs == NULL)
			return (ENOMEM);

		xs->timeout = 10000;
		xs->flags |= SCSI_DATA_OUT;
		xs->data = va;
		xs->datalen = nwrt * sectorsize;

		sd_cmd_rw10(xs, 0, blkno, nwrt); /* XXX */

		rv = scsi_xs_sync(xs);
		scsi_xs_put(xs);
		if (rv != 0)
			return (ENXIO);
#else	/* SD_DUMP_NOT_TRUSTED */
		/* Let's just talk about this first... */
		printf("sd%d: dump addr 0x%x, blk %lld\n", unit, va,
		    (long long)blkno);
		delay(500 * 1000);	/* half a second */
#endif	/* SD_DUMP_NOT_TRUSTED */

		/* update block count */
		totwrt -= nwrt;
		blkno += nwrt;
		va += sectorsize * nwrt;
	}

	sddoingadump = 0;

	return (0);
}

/*
 * Copy up to len chars from src to dst, ignoring non-printables.
 * Must be room for len+1 chars in dst so we can write the NUL.
 * Does not assume src is NUL-terminated.
 */
void
viscpy(u_char *dst, u_char *src, int len)
{
	while (len > 0 && *src != '\0') {
		if (*src < 0x20 || *src >= 0x80) {
			src++;
			continue;
		}
		*dst++ = *src++;
		len--;
	}
	*dst = '\0';
}

int
sd_read_cap_10(struct sd_softc *sc, int flags)
{
	struct scsi_read_capacity cdb;
	struct scsi_read_cap_data *rdcap;
	struct scsi_xfer *xs;
	int rv = ENOMEM;

	CLR(flags, SCSI_IGNORE_ILLEGAL_REQUEST);

	rdcap = dma_alloc(sizeof(*rdcap), (ISSET(flags, SCSI_NOSLEEP) ?
	    PR_NOWAIT : PR_WAITOK) | PR_ZERO);
	if (rdcap == NULL)
		return (ENOMEM);

	if (sc->flags & SDF_DYING) {
		rv = ENXIO;
		goto done;
	}
	xs = scsi_xs_get(sc->sc_link, flags | SCSI_DATA_IN | SCSI_SILENT);
	if (xs == NULL)
		goto done;

	bzero(&cdb, sizeof(cdb));
	cdb.opcode = READ_CAPACITY;

	memcpy(xs->cmd, &cdb, sizeof(cdb));
	xs->cmdlen = sizeof(cdb);
	xs->data = (void *)rdcap;
	xs->datalen = sizeof(*rdcap);
	xs->timeout = 20000;

	rv = scsi_xs_sync(xs);
	scsi_xs_put(xs);

	if (rv == 0) {
		sc->params.disksize = _4btol(rdcap->addr) + 1ll;
		sc->params.secsize = _4btol(rdcap->length);
		CLR(sc->flags, SDF_THIN);
	}

done:
	dma_free(rdcap, sizeof(*rdcap));
	return (rv);
}

int
sd_read_cap_16(struct sd_softc *sc, int flags)
{
	struct scsi_read_capacity_16 cdb;
	struct scsi_read_cap_data_16 *rdcap;
	struct scsi_xfer *xs;
	int rv = ENOMEM;

	CLR(flags, SCSI_IGNORE_ILLEGAL_REQUEST);

	rdcap = dma_alloc(sizeof(*rdcap), (ISSET(flags, SCSI_NOSLEEP) ?
	    PR_NOWAIT : PR_WAITOK) | PR_ZERO);
	if (rdcap == NULL)
		return (ENOMEM);

	if (sc->flags & SDF_DYING) {
		rv = ENXIO;
		goto done;
	}
	xs = scsi_xs_get(sc->sc_link, flags | SCSI_DATA_IN | SCSI_SILENT);
	if (xs == NULL)
		goto done;

	bzero(&cdb, sizeof(cdb));
	cdb.opcode = READ_CAPACITY_16;
	cdb.byte2 = SRC16_SERVICE_ACTION;
	_lto4b(sizeof(*rdcap), cdb.length);

	memcpy(xs->cmd, &cdb, sizeof(cdb));
	xs->cmdlen = sizeof(cdb);
	xs->data = (void *)rdcap;
	xs->datalen = sizeof(*rdcap);
	xs->timeout = 20000;

	rv = scsi_xs_sync(xs);
	scsi_xs_put(xs);

	if (rv == 0) {
		if (_8btol(rdcap->addr) == 0) {
			rv = EIO;
			goto done;
		}

		sc->params.disksize = _8btol(rdcap->addr) + 1;
		sc->params.secsize = _4btol(rdcap->length);
		if (ISSET(_2btol(rdcap->lowest_aligned), READ_CAP_16_TPE))
			SET(sc->flags, SDF_THIN);
		else
			CLR(sc->flags, SDF_THIN);
	}

done:
	dma_free(rdcap, sizeof(*rdcap));
	return (rv);
}

int
sd_size(struct sd_softc *sc, int flags)
{
	int rv;

	if (sc->flags & SDF_DYING)
		return (ENXIO);
	if (SCSI3(sc->sc_link->inqdata.version)) {
		rv = sd_read_cap_16(sc, flags);
		if (rv != 0)
			rv = sd_read_cap_10(sc, flags);
	} else {
		rv = sd_read_cap_10(sc, flags);
		if (rv == 0 && sc->params.disksize == 0x100000000ll)
			rv = sd_read_cap_16(sc, flags);
	}

	return (rv);
}

int
sd_thin_pages(struct sd_softc *sc, int flags)
{
	struct scsi_vpd_hdr *pg;
	size_t len = 0;
	u_int8_t *pages;
	int i, score = 0;
	int rv;

	pg = dma_alloc(sizeof(*pg), (ISSET(flags, SCSI_NOSLEEP) ?
	    PR_NOWAIT : PR_WAITOK) | PR_ZERO);
	if (pg == NULL)
		return (ENOMEM);

	if (sc->flags & SDF_DYING) {
		rv = ENXIO;
		goto done;
	}
	rv = scsi_inquire_vpd(sc->sc_link, pg, sizeof(*pg),
	    SI_PG_SUPPORTED, flags);
	if (rv != 0)
		goto done;

	len = _2btol(pg->page_length);

	dma_free(pg, sizeof(*pg));
	pg = dma_alloc(sizeof(*pg) + len, (ISSET(flags, SCSI_NOSLEEP) ?
	    PR_NOWAIT : PR_WAITOK) | PR_ZERO);
	if (pg == NULL)
		return (ENOMEM);

	if (sc->flags & SDF_DYING) {
		rv = ENXIO;
		goto done;
	}
	rv = scsi_inquire_vpd(sc->sc_link, pg, sizeof(*pg) + len,
	    SI_PG_SUPPORTED, flags);
	if (rv != 0)
		goto done;

	pages = (u_int8_t *)(pg + 1);
	if (pages[0] != SI_PG_SUPPORTED) {
		rv = EIO;
		goto done;
	}

	for (i = 1; i < len; i++) {
		switch (pages[i]) {
		case SI_PG_DISK_LIMITS:
		case SI_PG_DISK_THIN:
			score++;
			break;
		}
	}

	if (score < 2)
		rv = EOPNOTSUPP;

done:
	dma_free(pg, sizeof(*pg) + len);
	return (rv);
}

int
sd_vpd_block_limits(struct sd_softc *sc, int flags)
{
	struct scsi_vpd_disk_limits *pg;
	int rv;

	pg = dma_alloc(sizeof(*pg), (ISSET(flags, SCSI_NOSLEEP) ?
	    PR_NOWAIT : PR_WAITOK) | PR_ZERO);
	if (pg == NULL)
		return (ENOMEM);

	if (sc->flags & SDF_DYING) {
		rv = ENXIO;
		goto done;
	}
	rv = scsi_inquire_vpd(sc->sc_link, pg, sizeof(*pg),
	    SI_PG_DISK_LIMITS, flags);
	if (rv != 0)
		goto done;

	if (_2btol(pg->hdr.page_length) == SI_PG_DISK_LIMITS_LEN_THIN) {
		sc->params.unmap_sectors = _4btol(pg->max_unmap_lba_count);
		sc->params.unmap_descs = _4btol(pg->max_unmap_desc_count);
	} else
		rv = EOPNOTSUPP;

done:
	dma_free(pg, sizeof(*pg));
	return (rv);
}

int
sd_vpd_thin(struct sd_softc *sc, int flags)
{
	struct scsi_vpd_disk_thin *pg;
	int rv;

	pg = dma_alloc(sizeof(*pg), (ISSET(flags, SCSI_NOSLEEP) ?
	    PR_NOWAIT : PR_WAITOK) | PR_ZERO);
	if (pg == NULL)
		return (ENOMEM);

	if (sc->flags & SDF_DYING) {
		rv = ENXIO;
		goto done;
	}
	rv = scsi_inquire_vpd(sc->sc_link, pg, sizeof(*pg),
	    SI_PG_DISK_THIN, flags);
	if (rv != 0)
		goto done;

#ifdef notyet
	if (ISSET(pg->flags, VPD_DISK_THIN_TPU))
		sc->sc_delete = sd_unmap;
	else if (ISSET(pg->flags, VPD_DISK_THIN_TPWS)) {
		sc->sc_delete = sd_write_same_16;
		sc->params.unmap_descs = 1; /* WRITE SAME 16 only does one */
	} else
		rv = EOPNOTSUPP;
#endif

done:
	dma_free(pg, sizeof(*pg));
	return (rv);
}

int
sd_thin_params(struct sd_softc *sc, int flags)
{
	int rv;

	rv = sd_thin_pages(sc, flags);
	if (rv != 0)
		return (rv);

	rv = sd_vpd_block_limits(sc, flags);
	if (rv != 0)
		return (rv);

	rv = sd_vpd_thin(sc, flags);
	if (rv != 0)
		return (rv);

	return (0);
}

/*
 * Fill out the disk parameter structure. Return SDGP_RESULT_OK if the
 * structure is correctly filled in, SDGP_RESULT_OFFLINE otherwise. The caller
 * is responsible for clearing the SDEV_MEDIA_LOADED flag if the structure
 * cannot be completed.
 */
int
sd_get_parms(struct sd_softc *sc, struct disk_parms *dp, int flags)
{
	struct scsi_link *link;
	union scsi_mode_sense_buf *buf = NULL;
	struct page_rigid_geometry *rigid = NULL;
	struct page_flex_geometry *flex = NULL;
	struct page_reduced_geometry *reduced = NULL;
	u_char *page0 = NULL;
	u_int32_t heads = 0, sectors = 0, cyls = 0, secsize = 0;
	int err = 0, big;

	if (sd_size(sc, flags) != 0)
		return (SDGP_RESULT_OFFLINE);

	if (ISSET(sc->flags, SDF_THIN) && sd_thin_params(sc, flags) != 0) {
		/* we dont know the unmap limits, so we cant use thin shizz */
		CLR(sc->flags, SDF_THIN);
	}

	buf = dma_alloc(sizeof(*buf), PR_NOWAIT);
	if (buf == NULL)
		goto validate;

	if (sc->flags & SDF_DYING)
		goto die;
	link = sc->sc_link;

	/*
	 * Ask for page 0 (vendor specific) mode sense data to find
	 * READONLY info. The only thing USB devices will ask for.
	 */
	err = scsi_do_mode_sense(link, 0, buf, (void **)&page0,
	    NULL, NULL, NULL, 1, flags | SCSI_SILENT, &big);
	if (sc->flags & SDF_DYING)
		goto die;
	if (err == 0) {
		if (big && buf->hdr_big.dev_spec & SMH_DSP_WRITE_PROT)
			SET(link->flags, SDEV_READONLY);
		else if (!big && buf->hdr.dev_spec & SMH_DSP_WRITE_PROT)
			SET(link->flags, SDEV_READONLY);
		else
			CLR(link->flags, SDEV_READONLY);
	}

	/*
	 * Many UMASS devices choke when asked about their geometry. Most
	 * don't have a meaningful geometry anyway, so just fake it if
	 * scsi_size() worked.
	 */
	if ((link->flags & SDEV_UMASS) && (dp->disksize > 0))
		goto validate;

	switch (link->inqdata.device & SID_TYPE) {
	case T_OPTICAL:
		/* No more information needed or available. */
		break;

	case T_RDIRECT:
		/* T_RDIRECT supports only PAGE_REDUCED_GEOMETRY (6). */
		err = scsi_do_mode_sense(link, PAGE_REDUCED_GEOMETRY,
		    buf, (void **)&reduced, NULL, NULL, &secsize,
		    sizeof(*reduced), flags | SCSI_SILENT, NULL);
		if (!err && reduced &&
		    DISK_PGCODE(reduced, PAGE_REDUCED_GEOMETRY)) {
			if (dp->disksize == 0)
				dp->disksize = _5btol(reduced->sectors);
			if (secsize == 0)
				secsize = _2btol(reduced->bytes_s);
		}
		break;

	default:
		/*
		 * NOTE: Some devices leave off the last four bytes of
		 * PAGE_RIGID_GEOMETRY and PAGE_FLEX_GEOMETRY mode sense pages.
		 * The only information in those four bytes is RPM information
		 * so accept the page. The extra bytes will be zero and RPM will
		 * end up with the default value of 3600.
		 */
		if (((link->flags & SDEV_ATAPI) == 0) ||
		    ((link->flags & SDEV_REMOVABLE) == 0))
			err = scsi_do_mode_sense(link,
			    PAGE_RIGID_GEOMETRY, buf, (void **)&rigid, NULL,
			    NULL, &secsize, sizeof(*rigid) - 4,
			    flags | SCSI_SILENT, NULL);
		if (!err && rigid && DISK_PGCODE(rigid, PAGE_RIGID_GEOMETRY)) {
			heads = rigid->nheads;
			cyls = _3btol(rigid->ncyl);
			if (heads * cyls > 0)
				sectors = dp->disksize / (heads * cyls);
		} else {
			if (sc->flags & SDF_DYING)
				goto die;
			err = scsi_do_mode_sense(link,
			    PAGE_FLEX_GEOMETRY, buf, (void **)&flex, NULL, NULL,
			    &secsize, sizeof(*flex) - 4,
			    flags | SCSI_SILENT, NULL);
			if (!err && flex &&
			    DISK_PGCODE(flex, PAGE_FLEX_GEOMETRY)) {
				sectors = flex->ph_sec_tr;
				heads = flex->nheads;
				cyls = _2btol(flex->ncyl);
				if (secsize == 0)
					secsize = _2btol(flex->bytes_s);
				if (dp->disksize == 0)
					dp->disksize = heads * cyls * sectors;
			}
		}
		break;
	}

validate:
	if (buf)
		dma_free(buf, sizeof(*buf));

	if (dp->disksize == 0)
		return (SDGP_RESULT_OFFLINE);

	if (dp->secsize == 0)
		dp->secsize = (secsize == 0) ? 512 : secsize;

	/*
	 * Restrict secsize values to powers of two between 512 and 64k.
	 */
	switch (dp->secsize) {
	case 0x200:	/* == 512, == DEV_BSIZE on all architectures. */
	case 0x400:
	case 0x800:
	case 0x1000:
	case 0x2000:
	case 0x4000:
	case 0x8000:
	case 0x10000:
		break;
	default:
		SC_DEBUG(sc->sc_link, SDEV_DB1,
		    ("sd_get_parms: bad secsize: %#lx\n", dp->secsize));
		return (SDGP_RESULT_OFFLINE);
	}

	/*
	 * XXX THINK ABOUT THIS!!  Using values such that sectors * heads *
	 * cyls is <= disk_size can lead to wasted space. We need a more
	 * careful calculation/validation to make everything work out
	 * optimally.
	 */
	if (dp->disksize > 0xffffffff && (dp->heads * dp->sectors) < 0xffff) {
		dp->heads = 511;
		dp->sectors = 255;
		cyls = 0;
	} else {
		/*
		 * Use standard geometry values for anything we still don't
		 * know.
		 */
		dp->heads = (heads == 0) ? 255 : heads;
		dp->sectors = (sectors == 0) ? 63 : sectors;
	}

	dp->cyls = (cyls == 0) ? dp->disksize / (dp->heads * dp->sectors) :
	    cyls;

	if (dp->cyls == 0) {
		dp->heads = dp->cyls = 1;
		dp->sectors = dp->disksize;
	}

	return (SDGP_RESULT_OK);

die:
	dma_free(buf, sizeof(*buf));
	return (SDGP_RESULT_OFFLINE);
}

int
sd_flush(struct sd_softc *sc, int flags)
{
	struct scsi_link *link;
	struct scsi_xfer *xs;
	struct scsi_synchronize_cache *cmd;
	int error;

	if (sc->flags & SDF_DYING)
		return (ENXIO);
	link = sc->sc_link;

	if (link->quirks & SDEV_NOSYNCCACHE)
		return (0);

	/*
	 * Issue a SYNCHRONIZE CACHE. Address 0, length 0 means "all remaining
	 * blocks starting at address 0". Ignore ILLEGAL REQUEST in the event
	 * that the command is not supported by the device.
	 */

	xs = scsi_xs_get(link, flags);
	if (xs == NULL) {
		SC_DEBUG(link, SDEV_DB1, ("cache sync failed to get xs\n"));
		return (EIO);
	}

	cmd = (struct scsi_synchronize_cache *)xs->cmd;
	cmd->opcode = SYNCHRONIZE_CACHE;

	xs->cmdlen = sizeof(*cmd);
	xs->timeout = 100000;
	xs->flags |= SCSI_IGNORE_ILLEGAL_REQUEST;

	error = scsi_xs_sync(xs);

	scsi_xs_put(xs);

	if (error)
		SC_DEBUG(link, SDEV_DB1, ("cache sync failed\n"));
	else
		sc->flags &= ~SDF_DIRTY;

	return (error);
}
