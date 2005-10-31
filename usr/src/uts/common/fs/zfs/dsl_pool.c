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

#pragma ident	"%Z%%M%	%I%	%E% SMI"

#include <sys/dsl_pool.h>
#include <sys/dsl_dataset.h>
#include <sys/dsl_dir.h>
#include <sys/dmu_tx.h>
#include <sys/dmu_objset.h>
#include <sys/arc.h>
#include <sys/zap.h>
#include <sys/zfs_context.h>
#include <sys/fs/zfs.h>

/* internal reserved dir name */
#define	MOS_DIR_NAME "$MOS"

static dsl_dir_t *
dsl_pool_open_mos_dir(dsl_pool_t *dp)
{
	uint64_t obj;
	int err;

	err = zap_lookup(dp->dp_meta_objset,
	    dp->dp_root_dir->dd_phys->dd_child_dir_zapobj,
	    MOS_DIR_NAME, sizeof (obj), 1, &obj);
	ASSERT3U(err, ==, 0);

	return (dsl_dir_open_obj(dp, obj, MOS_DIR_NAME, dp));
}

static dsl_pool_t *
dsl_pool_open_impl(spa_t *spa, uint64_t txg)
{
	dsl_pool_t *dp;
	blkptr_t *bp = spa_get_rootblkptr(spa);

	dp = kmem_zalloc(sizeof (dsl_pool_t), KM_SLEEP);
	dp->dp_spa = spa;
	dp->dp_meta_rootbp = *bp;
	txg_init(dp, txg);

	txg_list_create(&dp->dp_dirty_datasets,
	    offsetof(dsl_dataset_t, ds_dirty_link));
	txg_list_create(&dp->dp_dirty_dirs,
	    offsetof(dsl_dir_t, dd_dirty_link));
	list_create(&dp->dp_synced_objsets, sizeof (dsl_dataset_t),
	    offsetof(dsl_dataset_t, ds_synced_link));

	return (dp);
}

dsl_pool_t *
dsl_pool_open(spa_t *spa, uint64_t txg)
{
	int err;
	dsl_pool_t *dp = dsl_pool_open_impl(spa, txg);

	dp->dp_meta_objset =
	    &dmu_objset_open_impl(spa, NULL, &dp->dp_meta_rootbp)->os;

	rw_enter(&dp->dp_config_rwlock, RW_READER);
	err = zap_lookup(dp->dp_meta_objset, DMU_POOL_DIRECTORY_OBJECT,
	    DMU_POOL_ROOT_DATASET, sizeof (uint64_t), 1,
	    &dp->dp_root_dir_obj);
	ASSERT3U(err, ==, 0);

	dp->dp_root_dir = dsl_dir_open_obj(dp, dp->dp_root_dir_obj,
	    NULL, dp);
	dp->dp_mos_dir = dsl_pool_open_mos_dir(dp);
	rw_exit(&dp->dp_config_rwlock);

	return (dp);
}

void
dsl_pool_close(dsl_pool_t *dp)
{
	/* drop our reference from dsl_pool_open() */
	dsl_dir_close(dp->dp_mos_dir, dp);
	dsl_dir_close(dp->dp_root_dir, dp);

	/* undo the dmu_objset_open_impl(mos) from dsl_pool_open() */
	dmu_objset_evict(NULL, dp->dp_meta_objset->os);

	txg_list_destroy(&dp->dp_dirty_datasets);
	txg_list_destroy(&dp->dp_dirty_dirs);
	list_destroy(&dp->dp_synced_objsets);

	arc_flush();
	txg_fini(dp);
	kmem_free(dp, sizeof (dsl_pool_t));
}

dsl_pool_t *
dsl_pool_create(spa_t *spa, uint64_t txg)
{
	int err;
	dsl_pool_t *dp = dsl_pool_open_impl(spa, txg);
	dmu_tx_t *tx = dmu_tx_create_assigned(dp, txg);
	dp->dp_meta_objset = &dmu_objset_create_impl(spa,
	    NULL, DMU_OST_META, tx)->os;

	/* create the pool directory */
	err = zap_create_claim(dp->dp_meta_objset, DMU_POOL_DIRECTORY_OBJECT,
	    DMU_OT_OBJECT_DIRECTORY, DMU_OT_NONE, 0, tx);
	ASSERT3U(err, ==, 0);

	/* create and open the root dir */
	dsl_dataset_create_root(dp, &dp->dp_root_dir_obj, tx);
	dp->dp_root_dir = dsl_dir_open_obj(dp, dp->dp_root_dir_obj,
	    NULL, dp);

	/* create and open the meta-objset dir */
	err = dsl_dir_create_sync(dp->dp_root_dir, MOS_DIR_NAME,
	    tx);
	ASSERT3U(err, ==, 0);
	dp->dp_mos_dir = dsl_pool_open_mos_dir(dp);

	dmu_tx_commit(tx);

	return (dp);
}

void
dsl_pool_sync(dsl_pool_t *dp, uint64_t txg)
{
	dmu_tx_t *tx;
	objset_impl_t *mosi = dp->dp_meta_objset->os;

	tx = dmu_tx_create_assigned(dp, txg);

	do {
		dsl_dir_t *dd;
		dsl_dataset_t *ds;

		while (ds = txg_list_remove(&dp->dp_dirty_datasets, txg)) {
			if (!list_link_active(&ds->ds_synced_link))
				list_insert_tail(&dp->dp_synced_objsets, ds);
			dsl_dataset_sync(ds, tx);
		}
		while (dd = txg_list_remove(&dp->dp_dirty_dirs, txg))
			dsl_dir_sync(dd, tx);
		/*
		 * We need to loop since dsl_dir_sync() could create a
		 * new (dirty) objset.
		 * XXX - isn't this taken care of by the spa's sync to
		 * convergence loop?
		 */
	} while (!txg_list_empty(&dp->dp_dirty_datasets, txg));

	if (list_head(&mosi->os_dirty_dnodes[txg & TXG_MASK]) != NULL ||
	    list_head(&mosi->os_free_dnodes[txg & TXG_MASK]) != NULL) {
		dmu_objset_sync(mosi, tx);
		dprintf_bp(&dp->dp_meta_rootbp, "meta objset rootbp is %s", "");
		spa_set_rootblkptr(dp->dp_spa, &dp->dp_meta_rootbp);
	}

	dmu_tx_commit(tx);
}

void
dsl_pool_zil_clean(dsl_pool_t *dp)
{
	dsl_dataset_t *ds;

	while (ds = list_head(&dp->dp_synced_objsets)) {
		list_remove(&dp->dp_synced_objsets, ds);
		ASSERT(ds->ds_user_ptr != NULL);
		zil_clean(((objset_impl_t *)ds->ds_user_ptr)->os_zil);
	}
}

int
dsl_pool_sync_context(dsl_pool_t *dp)
{
	/*
	 * Yeah, this is cheesy.  But the SPA needs some way to let
	 * the sync threads invoke spa_open() and spa_close() while
	 * it holds the namespace lock.  I'm certainly open to better
	 * ideas for how to determine whether the current thread is
	 * operating on behalf of spa_sync().  This works for now.
	 */
	return (curthread == dp->dp_tx.tx_sync_thread ||
	    BP_IS_HOLE(&dp->dp_meta_rootbp));
}

uint64_t
dsl_pool_adjustedsize(dsl_pool_t *dp, boolean_t netfree)
{
	uint64_t space, resv;

	/*
	 * Reserve about 1% (1/128), or at least 16MB, for allocation
	 * efficiency.
	 * XXX The intent log is not accounted for, so it must fit
	 * within this slop.
	 *
	 * If we're trying to assess whether it's OK to do a free,
	 * cut the reservation in half to allow forward progress
	 * (e.g. make it possible to rm(1) files from a full pool).
	 */
	space = spa_get_space(dp->dp_spa);
	resv = MAX(space >> 7, SPA_MINDEVSIZE >> 2);
	if (netfree)
		resv >>= 1;

	return (space - resv);
}