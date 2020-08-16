// SPDX-License-Identifier: GPL-2.0
/*
 *  linux/fs/ntfs3/record.c
 *
 * Copyright (C) 2019-2020 Paragon Software GmbH, All rights reserved.
 *
 */

#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/nls.h>
#include <linux/sched/signal.h>

#include "debug.h"
#include "ntfs.h"
#include "ntfs_fs.h"

static inline int compare_attr(const ATTRIB *left, ATTR_TYPE type,
			       const __le16 *name, u8 name_len,
			       const u16 *upcase)
{
	/* First, compare the type codes: */
	int diff = le32_to_cpu(left->type) - le32_to_cpu(type);

	if (diff)
		return diff;

	/*
	 * They have the same type code, so we have to compare the names.
	 * First compare case insensitive
	 */
	diff = ntfs_cmp_names(attr_name(left), left->name_len, name, name_len,
			      upcase);
	if (diff)
		return diff;

	/* Second compare case sensitive */
	return ntfs_cmp_names(attr_name(left), left->name_len, name, name_len,
			      NULL);
}

/*
 * mi_new_attt_id
 *
 * returns unused attribute id that is less than mrec->next_attr_id
 */
static __le16 mi_new_attt_id(mft_inode *mi)
{
	u16 free_id, max_id, t16;
	MFT_REC *rec = mi->mrec;
	ATTRIB *attr;
	__le16 id;

	id = rec->next_attr_id;
	free_id = le16_to_cpu(id);
	if (free_id < 0x7FFF) {
		rec->next_attr_id = cpu_to_le16(free_id + 1);
		return id;
	}

	/* One record can store up to 1024/24 ~= 42 attributes */
	free_id = 0;
	max_id = 0;

	attr = NULL;

next_attr:
	attr = mi_enum_attr(mi, attr);
	if (!attr) {
		rec->next_attr_id = cpu_to_le16(max_id + 1);
		mi->dirty = true;
		return cpu_to_le16(free_id);
	}

	t16 = le16_to_cpu(attr->id);
	if (t16 == free_id) {
		free_id += 1;
		attr = NULL;
		goto next_attr;
	}

	if (max_id < t16)
		max_id = t16;
	goto next_attr;
}

int mi_get(ntfs_sb_info *sbi, CLST rno, mft_inode **mi)
{
	int err;
	mft_inode *m = ntfs_alloc(sizeof(mft_inode), 1);

	if (!m)
		return -ENOMEM;

	err = mi_init(m, sbi, rno);
	if (!err)
		err = mi_read(m, false);

	if (err) {
		mi_put(m);
		return err;
	}

	*mi = m;
	return 0;
}

void mi_put(mft_inode *mi)
{
	mi_clear(mi);
	ntfs_free(mi);
}

int mi_init(mft_inode *mi, ntfs_sb_info *sbi, CLST rno)
{
	mi->sbi = sbi;
	mi->rno = rno;
	mi->mrec = ntfs_alloc(sbi->record_size, 0);
	if (!mi->mrec)
		return -ENOMEM;

	return 0;
}

/*
 * mi_read
 *
 * reads MFT data
 */
int mi_read(mft_inode *mi, bool is_mft)
{
	int err;
	MFT_REC *rec = mi->mrec;
	ntfs_sb_info *sbi = mi->sbi;
	u32 bpr = sbi->record_size;
	u64 vbo = (u64)mi->rno << sbi->record_bits;
	ntfs_inode *mft_ni = sbi->mft.ni;
	struct runs_tree *run = mft_ni ? &mft_ni->file.run : NULL;
	struct rw_semaphore *rw_lock = NULL;

	if (is_mounted(sbi)) {
		if (!is_mft) {
			rw_lock = &mft_ni->file.run_lock;
			down_read(rw_lock);
		}
	}

	err = ntfs_read_bh_ex(sbi, run, vbo, &rec->rhdr, bpr, &mi->nb);
	if (rw_lock)
		up_read(rw_lock);
	if (!err)
		goto ok;

	if (err == 1) {
		mi->dirty = true;
		goto ok;
	}

	if (err != -ENOENT)
		goto out;

	if (rw_lock) {
		ni_lock(mft_ni);
		down_write(rw_lock);
	}
	err = attr_load_runs_vcn(mft_ni, ATTR_DATA, NULL, 0, &mft_ni->file.run,
				 vbo >> sbi->cluster_bits);
	if (rw_lock) {
		up_write(rw_lock);
		ni_unlock(mft_ni);
	}
	if (err)
		goto out;

	if (rw_lock)
		down_read(rw_lock);
	err = ntfs_read_bh_ex(sbi, run, vbo, &rec->rhdr, bpr, &mi->nb);
	if (rw_lock)
		up_read(rw_lock);

	if (err == 1) {
		mi->dirty = true;
		goto ok;
	}
	if (err)
		goto out;

ok:
	/* check field 'total' only here */
	if (le32_to_cpu(rec->total) != bpr) {
		err = -EINVAL;
		goto out;
	}

	return 0;

out:
	return err;
}

ATTRIB *mi_enum_attr(mft_inode *mi, ATTRIB *attr)
{
	const MFT_REC *rec = mi->mrec;
	u32 used = le32_to_cpu(rec->used);
	u32 t32, off, asize;
	u16 t16;

	if (!attr) {
		u32 total = le32_to_cpu(rec->total);

		off = le16_to_cpu(rec->attr_off);

		if (used > total)
			goto out;

		if (off >= used || off < MFTRECORD_FIXUP_OFFSET_1 ||
		    !IsDwordAligned(off)) {
			goto out;
		}

		/* Skip non-resident records */
		if (!is_rec_inuse(rec))
			goto out;

		attr = Add2Ptr(rec, off);
	} else {
		/* Check if input attr inside record */
		off = PtrOffset(rec, attr);
		if (off >= used)
			goto out;

		asize = le32_to_cpu(attr->size);
		if (asize < SIZEOF_RESIDENT)
			goto out;

		attr = Add2Ptr(attr, asize);
		off += asize;
	}

	asize = le32_to_cpu(attr->size);

	/* Can we use the first field (attr->type) */
	if (off + 8 > used) {
		static_assert(QuadAlign(sizeof(ATTR_TYPE)) == 8);
		goto out;
	}

	if (attr->type == ATTR_END) {
		if (used != off + 8)
			goto out;
		return NULL;
	}

	t32 = le32_to_cpu(attr->type);
	if ((t32 & 0xf) || (t32 > 0x100))
		goto out;

	/* Check boundary */
	if (off + asize > used)
		goto out;

	/* Check size of attribute */
	if (!attr->non_res) {
		if (asize < SIZEOF_RESIDENT)
			goto out;

		t16 = le16_to_cpu(attr->res.data_off);

		if (t16 > asize)
			goto out;

		t32 = le32_to_cpu(attr->res.data_size);
		if (t16 + t32 > asize)
			goto out;

		return attr;
	}

	/* Check some nonresident fields */
	if (attr->name_len &&
	    le16_to_cpu(attr->name_off) + sizeof(short) * attr->name_len >
		    le16_to_cpu(attr->nres.run_off)) {
		goto out;
	}

	if (attr->nres.svcn || !is_attr_ext(attr)) {
		if (asize + 8 < SIZEOF_NONRESIDENT)
			goto out;

		if (attr->nres.c_unit)
			goto out;
	} else if (asize + 8 < SIZEOF_NONRESIDENT_EX)
		goto out;

	return attr;

out:
	return NULL;
}

/*
 * mi_find_attr
 *
 * finds the attribute by type and name and id
 */
ATTRIB *mi_find_attr(mft_inode *mi, ATTRIB *attr, ATTR_TYPE type,
		     const __le16 *name, size_t name_len, const __le16 *id)
{
	u32 type_in = le32_to_cpu(type);
	u32 atype;

next_attr:
	attr = mi_enum_attr(mi, attr);
	if (!attr)
		return NULL;

	atype = le32_to_cpu(attr->type);
	if (atype > type_in)
		return NULL;

	if (atype < type_in)
		goto next_attr;

	if (attr->name_len != name_len)
		goto next_attr;

	if (name_len && memcmp(attr_name(attr), name, name_len * sizeof(short)))
		goto next_attr;

	if (id && *id != attr->id)
		goto next_attr;

	return attr;
}

int mi_write(mft_inode *mi, int wait)
{
	MFT_REC *rec;
	int err;
	ntfs_sb_info *sbi;

	if (!mi->dirty)
		return 0;

	sbi = mi->sbi;
	rec = mi->mrec;

	err = ntfs_write_bh_ex(sbi, &rec->rhdr, &mi->nb, wait);
	if (err)
		return err;

	mi->dirty = false;

	return 0;
}

int mi_format_new(mft_inode *mi, ntfs_sb_info *sbi, CLST rno, __le16 flags,
		  bool is_mft)
{
	int err;
	u16 seq = 1;
	MFT_REC *rec;
	u64 vbo = (u64)rno << sbi->record_bits;

	err = mi_init(mi, sbi, rno);
	if (err)
		return err;

	rec = mi->mrec;

	if (rno == MFT_REC_MFT)
		;
	else if (rno < MFT_REC_FREE)
		seq = rno;
	else if (rno >= sbi->mft.used)
		;
	else if (mi_read(mi, is_mft))
		;
	else if (rec->rhdr.sign == NTFS_FILE_SIGNATURE) {
		/* Record is reused. Update its sequence number */
		seq = le16_to_cpu(rec->seq) + 1;
		if (!seq)
			seq = 1;
	}

	memcpy(rec, sbi->new_rec, sbi->record_size);

	rec->seq = cpu_to_le16(seq);
	rec->flags = RECORD_FLAG_IN_USE | flags;

	mi->dirty = true;

	if (!mi->nb.nbufs) {
		ntfs_inode *ni = sbi->mft.ni;
		bool lock = false;

		if (is_mounted(sbi) && !is_mft) {
			down_read(&ni->file.run_lock);
			lock = true;
		}

		err = ntfs_get_bh(sbi, &ni->file.run, vbo, sbi->record_size,
				  &mi->nb);
		if (lock)
			up_read(&ni->file.run_lock);
	}

	return err;
}

/*
 * mi_mark_free
 *
 * marks record as unused and marks it as free in bitmap
 */
void mi_mark_free(mft_inode *mi)
{
	CLST rno = mi->rno;
	ntfs_sb_info *sbi = mi->sbi;

	if (rno >= MFT_REC_RESERVED && rno < MFT_REC_FREE) {
		ntfs_clear_mft_tail(sbi, rno, rno + 1);
		mi->dirty = false;
		return;
	}

	if (mi->mrec) {
		clear_rec_inuse(mi->mrec);
		mi->dirty = true;
		mi_write(mi, 0);
	}
	ntfs_mark_rec_free(sbi, rno);
}

/*
 * mi_insert_attr
 *
 * reserves space for new attribute
 * returns not full constructed attribute or NULL if not possible to create
 */
ATTRIB *mi_insert_attr(mft_inode *mi, ATTR_TYPE type, const __le16 *name,
		       u8 name_len, u32 asize, u16 name_off)
{
	size_t tail;
	ATTRIB *attr;
	__le16 id;
	MFT_REC *rec = mi->mrec;
	ntfs_sb_info *sbi = mi->sbi;
	u32 used = le32_to_cpu(rec->used);
	const u16 *upcase = sbi->upcase;
	int diff;

	/* Can we insert mi attribute? */
	if (used + asize > mi->sbi->record_size)
		return NULL;

	/*
	 * Scan through the list of attributes to find the point
	 * at which we should insert it.
	 */
	attr = NULL;
	while ((attr = mi_enum_attr(mi, attr))) {
		diff = compare_attr(attr, type, name, name_len, upcase);
		if (diff > 0)
			break;
		if (diff < 0)
			continue;

		if (!is_attr_indexed(attr))
			return NULL;
		break;
	}

	if (!attr) {
		tail = 8; /* not used, just to suppress warning */
		attr = Add2Ptr(rec, used - 8);
	} else {
		tail = used - PtrOffset(rec, attr);
	}

	id = mi_new_attt_id(mi);

	memmove(Add2Ptr(attr, asize), attr, tail);
	memset(attr, 0, asize);

	attr->type = type;
	attr->size = cpu_to_le32(asize);
	attr->name_len = name_len;
	attr->name_off = cpu_to_le16(name_off);
	attr->id = id;

	memmove(Add2Ptr(attr, name_off), name, name_len * sizeof(short));
	rec->used = cpu_to_le32(used + asize);

	mi->dirty = true;

	return attr;
}

/*
 * mi_remove_attr
 *
 * removes the attribute from record
 * NOTE: The source attr will point to next attribute
 */
bool mi_remove_attr(mft_inode *mi, ATTRIB *attr)
{
	MFT_REC *rec = mi->mrec;
	u32 aoff = PtrOffset(rec, attr);
	u32 used = le32_to_cpu(rec->used);
	u32 asize = le32_to_cpu(attr->size);

	if (aoff + asize > used)
		return false;

	used -= asize;
	memmove(attr, Add2Ptr(attr, asize), used - aoff);
	rec->used = cpu_to_le32(used);
	mi->dirty = true;

	return true;
}

bool mi_resize_attr(mft_inode *mi, ATTRIB *attr, int bytes)
{
	MFT_REC *rec = mi->mrec;
	u32 aoff = PtrOffset(rec, attr);
	u32 total, used = le32_to_cpu(rec->used);
	u32 nsize, asize = le32_to_cpu(attr->size);
	u32 rsize = le32_to_cpu(attr->res.data_size);
	int tail = (int)(used - aoff - asize);
	int dsize;
	char *next;

	if (tail < 0 || aoff >= used)
		return false;

	if (!bytes)
		return true;

	total = le32_to_cpu(rec->total);
	next = Add2Ptr(attr, asize);

	if (bytes > 0) {
		dsize = QuadAlign(bytes);
		if (used + dsize > total)
			return false;
		nsize = asize + dsize;
		// move tail
		memmove(next + dsize, next, tail);
		memset(next, 0, dsize);
		used += dsize;
		rsize += dsize;
	} else {
		dsize = QuadAlign(-bytes);
		if (dsize > asize)
			return false;
		nsize = asize - dsize;
		memmove(next - dsize, next, tail);
		used -= dsize;
		rsize -= dsize;
	}

	rec->used = cpu_to_le32(used);
	attr->size = cpu_to_le32(nsize);
	if (!attr->non_res)
		attr->res.data_size = cpu_to_le32(rsize);
	mi->dirty = true;

	return true;
}

int mi_pack_runs(mft_inode *mi, ATTRIB *attr, struct runs_tree *run, CLST len)
{
	int err = 0;
	ntfs_sb_info *sbi = mi->sbi;
	u32 new_run_size;
	CLST plen;
	MFT_REC *rec = mi->mrec;
	CLST svcn = le64_to_cpu(attr->nres.svcn);
	u32 used = le32_to_cpu(rec->used);
	u32 aoff = PtrOffset(rec, attr);
	u32 asize = le32_to_cpu(attr->size);
	char *next = Add2Ptr(attr, asize);
	u16 run_off = le16_to_cpu(attr->nres.run_off);
	u32 run_size = asize - run_off;
	u32 tail = used - aoff - asize;
	u32 dsize = sbi->record_size - used;

	/* Make a maximum gap in current record */
	memmove(next + dsize, next, tail);

	/* Pack as much as possible */
	err = run_pack(run, svcn, len, Add2Ptr(attr, run_off), run_size + dsize,
		       &plen);
	if (err < 0) {
		memmove(next, next + dsize, tail);
		return err;
	}

	new_run_size = QuadAlign(err);

	memmove(next + new_run_size - run_size, next + dsize, tail);

	attr->size = cpu_to_le32(asize + new_run_size - run_size);
	attr->nres.evcn = cpu_to_le64(svcn + plen - 1);
	rec->used = cpu_to_le32(used + new_run_size - run_size);
	mi->dirty = true;

	return 0;
}
