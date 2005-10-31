/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License, Version 1.0 only
 * (the "License").  You may not use this file except in compliance
 * with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2005 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#ifndef _SYS_VDEV_IMPL_H
#define	_SYS_VDEV_IMPL_H

#pragma ident	"%Z%%M%	%I%	%E% SMI"

#include <sys/avl.h>
#include <sys/dmu.h>
#include <sys/metaslab.h>
#include <sys/nvpair.h>
#include <sys/space_map.h>
#include <sys/vdev.h>
#include <sys/dkio.h>
#include <sys/uberblock_impl.h>

#ifdef	__cplusplus
extern "C" {
#endif

/*
 * Virtual device descriptors.
 *
 * All storage pool operations go through the virtual device framework,
 * which provides data replication and I/O scheduling.
 */

/*
 * Forward declarations that lots of things need.
 */
typedef struct vdev_queue vdev_queue_t;
typedef struct vdev_cache vdev_cache_t;
typedef struct vdev_cache_entry vdev_cache_entry_t;

/*
 * Virtual device operations
 */
typedef int	vdev_open_func_t(vdev_t *vd, uint64_t *size, uint64_t *ashift);
typedef void	vdev_close_func_t(vdev_t *vd);
typedef uint64_t vdev_asize_func_t(vdev_t *vd, uint64_t psize);
typedef void	vdev_io_start_func_t(zio_t *zio);
typedef void	vdev_io_done_func_t(zio_t *zio);
typedef void	vdev_state_change_func_t(vdev_t *vd, int, int);

typedef struct vdev_ops {
	vdev_open_func_t		*vdev_op_open;
	vdev_close_func_t		*vdev_op_close;
	vdev_asize_func_t		*vdev_op_asize;
	vdev_io_start_func_t		*vdev_op_io_start;
	vdev_io_done_func_t		*vdev_op_io_done;
	vdev_state_change_func_t	*vdev_op_state_change;
	char				vdev_op_type[16];
	boolean_t			vdev_op_leaf;
} vdev_ops_t;

/*
 * Virtual device properties
 */
struct vdev_cache_entry {
	char		*ve_data;
	uint64_t	ve_offset;
	uint64_t	ve_lastused;
	avl_node_t	ve_offset_node;
	avl_node_t	ve_lastused_node;
	uint32_t	ve_hits;
	uint16_t	ve_missed_update;
	zio_t		*ve_fill_io;
};

struct vdev_cache {
	uint64_t	vc_size;
	uint64_t	vc_bshift;
	uint64_t	vc_blocksize;
	uint64_t	vc_max;
	avl_tree_t	vc_offset_tree;
	avl_tree_t	vc_lastused_tree;
	kmutex_t	vc_lock;
};

struct vdev_queue {
	uint64_t	vq_min_pending;
	uint64_t	vq_max_pending;
	uint64_t	vq_agg_limit;
	uint64_t	vq_time_shift;
	uint64_t	vq_ramp_rate;
	avl_tree_t	vq_deadline_tree;
	avl_tree_t	vq_read_tree;
	avl_tree_t	vq_write_tree;
	avl_tree_t	vq_pending_tree;
	kmutex_t	vq_lock;
};

/*
 * Virtual device descriptor
 */
struct vdev {
	/*
	 * Common to all vdev types.
	 */
	uint64_t	vdev_id;	/* child number in vdev parent	*/
	uint64_t	vdev_guid;	/* unique ID for this vdev	*/
	uint64_t	vdev_guid_sum;	/* self guid + all child guids	*/
	uint64_t	vdev_asize;	/* allocatable device capacity	*/
	uint64_t	vdev_ashift;	/* block alignment shift	*/
	uint64_t	vdev_state;	/* see VDEV_STATE_* #defines	*/
	vdev_ops_t	*vdev_ops;	/* vdev operations		*/
	spa_t		*vdev_spa;	/* spa for this vdev		*/
	void		*vdev_tsd;	/* type-specific data		*/
	vdev_t		*vdev_top;	/* top-level vdev		*/
	vdev_t		*vdev_parent;	/* parent vdev			*/
	vdev_t		**vdev_child;	/* array of children		*/
	uint64_t	vdev_children;	/* number of children		*/
	space_map_t	vdev_dtl_map;	/* dirty time log in-core state	*/
	space_map_t	vdev_dtl_scrub;	/* DTL for scrub repair writes	*/
	vdev_stat_t	vdev_stat;	/* virtual device statistics	*/

	/*
	 * Top-level vdev state.
	 */
	uint64_t	vdev_ms_array;	/* metaslab array object	*/
	uint64_t	vdev_ms_shift;	/* metaslab size shift		*/
	uint64_t	vdev_ms_count;	/* number of metaslabs		*/
	metaslab_group_t *vdev_mg;	/* metaslab group		*/
	metaslab_t	**vdev_ms;	/* metaslab array		*/
	space_map_obj_t	*vdev_smo;	/* metaslab space map array	*/
	txg_list_t	vdev_ms_list;	/* per-txg dirty metaslab lists	*/
	txg_list_t	vdev_dtl_list;	/* per-txg dirty DTL lists	*/
	txg_node_t	vdev_txg_node;	/* per-txg dirty vdev linkage	*/
	uint8_t		vdev_dirty[TXG_SIZE]; /* per-txg dirty flags	*/
	int		vdev_is_dirty;	/* on config dirty list?	*/
	list_node_t	vdev_dirty_node; /* config dirty list		*/
	zio_t		*vdev_io_retry;	/* I/O retry list		*/
	list_t		vdev_io_pending; /* I/O pending list		*/

	/*
	 * Leaf vdev state.
	 */
	uint64_t	vdev_psize;	/* physical device capacity	*/
	space_map_obj_t	vdev_dtl;	/* dirty time log on-disk state	*/
	txg_node_t	vdev_dtl_node;	/* per-txg dirty DTL linkage	*/
	char		*vdev_path;	/* vdev path (if any)		*/
	char		*vdev_devid;	/* vdev devid (if any)		*/
	uint64_t	vdev_fault_arg; /* fault injection paramater	*/
	int		vdev_fault_mask; /* zio types to fault		*/
	uint8_t		vdev_fault_mode; /* fault injection mode	*/
	uint8_t		vdev_cache_active; /* vdev_cache and vdev_queue	*/
	uint8_t		vdev_offline;	/* device taken offline?	*/
	uint8_t		vdev_detached;	/* device detached?		*/
	vdev_queue_t	vdev_queue;	/* I/O deadline schedule queue	*/
	vdev_cache_t	vdev_cache;	/* physical block cache		*/

	/*
	 * For DTrace to work in userland (libzpool) context, these fields must
	 * remain at the end of the structure.  DTrace will use the kernel's
	 * CTF definition for 'struct vdev', and since the size of a kmutex_t is
	 * larger in userland, the offsets for the rest fields would be
	 * incorrect.
	 */
	kmutex_t	vdev_dtl_lock;	/* vdev_dtl_{map,resilver}	*/
	kmutex_t	vdev_dirty_lock; /* vdev_dirty[]		*/
	kmutex_t	vdev_io_lock;	/* vdev_io_pending list		*/
	kcondvar_t	vdev_io_cv;	/* vdev_io_pending list empty?	*/
	kmutex_t	vdev_stat_lock;	/* vdev_stat			*/
};

#define	VDEV_SKIP_SIZE		(8 << 10)
#define	VDEV_BOOT_HEADER_SIZE	(8 << 10)
#define	VDEV_PHYS_SIZE		(112 << 10)
#define	VDEV_UBERBLOCKS		((128 << 10) >> UBERBLOCK_SHIFT)

#define	VDEV_BOOT_MAGIC		0x2f5b007b10c	/* ZFS boot block	*/
#define	VDEV_BOOT_VERSION	1		/* version number	*/

typedef struct vdev_boot_header {
	uint64_t	vb_magic;		/* VDEV_BOOT_MAGIC	*/
	uint64_t	vb_version;		/* VDEV_BOOT_VERSION	*/
	uint64_t	vb_offset;		/* start offset	(bytes) */
	uint64_t	vb_size;		/* size (bytes)		*/
	char		vb_pad[VDEV_BOOT_HEADER_SIZE - 4 * sizeof (uint64_t)];
} vdev_boot_header_t;

typedef struct vdev_phys {
	char		vp_nvlist[VDEV_PHYS_SIZE - sizeof (zio_block_tail_t)];
	zio_block_tail_t vp_zbt;
} vdev_phys_t;

typedef struct vdev_label {
	char			vl_pad[VDEV_SKIP_SIZE];		/*   8K	*/
	vdev_boot_header_t	vl_boot_header;			/*   8K	*/
	vdev_phys_t		vl_vdev_phys;			/* 120K	*/
	uberblock_phys_t	vl_uberblock[VDEV_UBERBLOCKS];	/* 128K	*/
} vdev_label_t;							/* 256K total */

/*
 * Size and offset of embedded boot loader region on each label.
 * The total size of the first two labels plus the boot area is 4MB.
 */
#define	VDEV_BOOT_OFFSET	(2 * sizeof (vdev_label_t))
#define	VDEV_BOOT_SIZE		(7ULL << 19)			/* 3.5M	*/

/*
 * vdev_dirty[] flags
 */
#define	VDD_ALLOC	0x01	/* allocated from in this txg		*/
#define	VDD_FREE	0x02	/* freed to in this txg			*/
#define	VDD_ADD		0x04	/* added to the pool in this txg	*/
#define	VDD_DTL		0x08	/* dirty time log entry in this txg	*/

/*
 * Size of label regions at the start and end of each leaf device.
 */
#define	VDEV_LABEL_START_SIZE	(2 * sizeof (vdev_label_t) + VDEV_BOOT_SIZE)
#define	VDEV_LABEL_END_SIZE	(2 * sizeof (vdev_label_t))
#define	VDEV_LABELS		4

#define	VDEV_ALLOC_LOAD		0
#define	VDEV_ALLOC_ADD		1

/*
 * Allocate or free a vdev
 */
extern vdev_t *vdev_alloc(spa_t *spa, nvlist_t *config, vdev_t *parent,
    uint_t id, int alloctype);
extern void vdev_free(vdev_t *vd);

/*
 * Add or remove children and parents
 */
extern void vdev_add_child(vdev_t *pvd, vdev_t *cvd);
extern void vdev_remove_child(vdev_t *pvd, vdev_t *cvd);
extern void vdev_compact_children(vdev_t *pvd);
extern vdev_t *vdev_add_parent(vdev_t *cvd, vdev_ops_t *ops);
extern void vdev_remove_parent(vdev_t *cvd);

/*
 * vdev sync load and sync
 */
extern int vdev_load(vdev_t *vd, int import);
extern void vdev_sync(vdev_t *vd, uint64_t txg);
extern void vdev_sync_done(vdev_t *vd, uint64_t txg);
extern void vdev_dirty(vdev_t *vd, uint8_t flags, uint64_t txg);

/*
 * Available vdev types.
 */
extern vdev_ops_t vdev_root_ops;
extern vdev_ops_t vdev_mirror_ops;
extern vdev_ops_t vdev_replacing_ops;
extern vdev_ops_t vdev_raidz_ops;
extern vdev_ops_t vdev_disk_ops;
extern vdev_ops_t vdev_file_ops;
extern vdev_ops_t vdev_missing_ops;

/*
 * Common asize function
 */
extern uint64_t vdev_default_asize(vdev_t *vd, uint64_t psize);

#ifdef	__cplusplus
}
#endif

#endif	/* _SYS_VDEV_IMPL_H */