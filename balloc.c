/*
 * PMFS emulated persistence. This file contains code to 
 * handle data blocks of various sizes efficiently.
 *
 * Persistent Memory File System
 * Copyright (c) 2012-2013, Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 */

/* 
 * Amendment:
 * 1.build index on free blocks;
 * 2.build three linear lists for three-size-type continuous free blocks;
 * 3.modify relative algorithm.
 */

#include <linux/fs.h>
#include <linux/bitops.h>
#include "pmfs.h"

void pmfs_init_blockmap(struct super_block *sb, unsigned long init_used_size)
{
	struct pmfs_sb_info *sbi = PMFS_SB(sb);
	unsigned long num_used_block;
	struct pmfs_blocknode *blknode;
	struct pmfs_blockp *blkp;
	unsigned long length;

	/* init_used_size contains: super blocks, pmfs-log, etc */
	num_used_block = (init_used_size + sb->s_blocksize - 1) >>
		sb->s_blocksize_bits;

	blknode = pmfs_alloc_blocknode(sb);
	blkp = pmfs_alloc_blockp();
	if (blknode == NULL || blkp == NULL)
		PMFS_ASSERT(0);
	blkp->blocknode = blknode;  
	blknode->blockp=blkp;
	/*  M1
	blknode->block_low = sbi->block_start;
	blknode->block_high = sbi->block_start + num_used_block - 1;
	*/
	sbi->num_free_blocks -= num_used_block;
	
	blknode->block_low = sbi->block_start + num_used_block ;
	blknode->block_high = sbi->block_end ;
	
	length = blknode->block_high - blknode->block_low + 1;
	/* M2  list_add(&blknode->link, &sbi->block_inuse_head); */
	list_add(&blknode->link, &sbi->block_free_head);   // build index on continuous freeblocks 
	/* build three linear list */
	if(length<512)  
		list_add(&blkp->link, &sbi->freeblocks_4K_head);
	else if(length>=512 && length<0x40000)
		list_add(&blkp->link, &sbi->freeblocks_2M_head);
	else
		list_add(&blkp->link, &sbi->freeblocks_1G_head);
	
}

/* This function not used
static struct pmfs_blocknode *pmfs_next_blocknode(struct pmfs_blocknode *i,
						  struct list_head *head)
{
	if (list_is_last(&i->link, head))
		return NULL;
	return list_first_entry(&i->link, typeof(*i), link);
}
*/
unsigned int is_freeblks_type_change(struct super_block *sb,struct list_head *head, struct pmfs_blocknode *node){
	struct pmfs_sb_info *sbi=PMFS_SB(sb);
	unsigned int length= node->block_high - node->block_low +1;
	if(head == &(sbi->freeblocks_4K_head))
		return (length<512) ? 0 : 1;
	else if(head == &(sbi->freeblocks_2M_head))
		return (length>=512 && length< 0x40000) ? 0 : 1;
	else 
		return (length>= 0x40000) ? 0: 1;
	
}	

struct list_head *pmfs_get_type_head(struct super_block *sb, unsigned long size, unsigned int flag)
{
	/* flag means two different call method */
	struct pmfs_sb_info *sbi=PMFS_SB(sb);
	struct list_head *head = NULL;
	if(size < 512){
		head = &(sbi->freeblocks_4K_head);
		if(flag==0 && head->next == head){  // if the 4K_list is empty, find freeblocks on 2M_list
			size=512;
		}
	}
	if(size>=512 && size<0x40000 ){
		head = &(sbi->freeblocks_2M_head);
		if(flag==0 && head -> next == head)   // if the 2M_list is empty ,find freeblocks on 1G_list
			size=0x40000;
	}
	if(size>=0x40000){
		head = &(sbi->freeblocks_1G_head);
		if(flag==0 && head ->next == head){ // if the 1G_list is empty, no continuous freeblocks can fit needs, return error code
			head=NULL;
		}
	}
	return head;
}

/* Caller must hold the super_block lock.  If start_hint is provided, it is
 * only valid until the caller releases the super_block lock. */
void __pmfs_free_block(struct super_block *sb, unsigned long blocknr,
		      unsigned short btype, struct pmfs_blocknode **start_hint)
{
	struct pmfs_sb_info *sbi = PMFS_SB(sb);
	//struct list_head *head = &(sbi->block_inuse_head);
	struct list_head *free_head = &(sbi->block_free_head);
	struct list_head *free_type_head = NULL;
	unsigned long prev_block_high;
	unsigned long new_block_low;
	unsigned long new_block_high;
	unsigned long num_blocks = 0;
	unsigned long init_used_size, num_used_block, journal_data_start;
	unsigned long size, new_size ;
	struct pmfs_blocknode *i, *prev_i;
	struct pmfs_blocknode *free_blocknode = NULL;
	struct pmfs_blockp *free_blockp = NULL;
	struct pmfs_blockp *free_blockp2 = NULL;
	struct pmfs_blocknode *curr_node;
	struct pmfs_blockp  *bp, *bp_prev, *newbp;

	num_blocks = pmfs_get_numblocks(btype);
	new_block_low = blocknr;
	new_block_high = blocknr + num_blocks - 1;

	journal_data_start = PMFS_SB_SIZE * 2;
	journal_data_start = (journal_data_start +sb->s_blocksize - 1) &
		~(sb->s_blocksize - 1);
	init_used_size = journal_data_start + sbi->jsize;
	num_used_block = (init_used_size + sb->s_blocksize - 1) >>
		sb->s_blocksize_bits;

	BUG_ON(list_empty(free_head));

	if (start_hint && *start_hint &&
	    new_block_low >= (*start_hint)->block_low)
		i = *start_hint;
	else{
		i = list_first_entry(free_head, typeof(*i), link);
		//pmfs_info("pmfs dbg info:__pmfs_free_block(), firstnode->low=%lu, firstnode_high=%lu\n",i->block_low,i->block_high);
	}
	//pmfs_info("pmfs dbg info: __pmfs_free_block(), new_block_low=%lu, new_block_high=%lu\n",new_block_low, new_block_high);
	list_for_each_entry_from(i, free_head, link) {
		
		if(i->link.prev == free_head) {
			prev_i = NULL;
			prev_block_high = sbi->block_start + num_used_block - 1;
		} else {
			prev_i = list_entry(i->link.prev, typeof(*i), link);
			prev_block_high = prev_i->block_high;
		}
		//pmfs_info("pmfs dbg info: __pmfs_free_block(), i->block_low=%lu, i->block_high=%lu, 
		//			prev_block_high=%lu\n",i->block_low, i->block_high,prev_block_high);
		
		if (new_block_low > i->block_high && i->link.next != free_head) {
			/* skip to next blocknode */ 
			//pmfs_info("pmfs dbg info: __pmfs_free_block(): skip to next\n");
			continue;
		}

		if(new_block_low > i->block_high && i->link.next == free_head){
			if(new_block_low == i->block_high + 1){
				//aligns left
				size = i->block_high - i->block_low +1;
				free_type_head = pmfs_get_type_head(sb,size,1);
				i->block_high = new_block_high;
				bp = i->blockp;
				if(is_freeblks_type_change(sb,free_type_head, i)){
					list_del(&bp->link);
					new_size = i->block_high - i->block_low +1;
					free_type_head = pmfs_get_type_head(sb,new_size,1);
					list_add(&bp->link, free_type_head);
				}
				sbi->num_free_blocks += num_blocks;
				goto block_found;
			}

			if(new_block_low > i->block_high +1){
				curr_node = pmfs_alloc_blocknode( sb);
				newbp = pmfs_alloc_blockp();
				if(curr_node == NULL || newbp == NULL)
					PMFS_ASSERT(0);
				curr_node->blockp = newbp;
				curr_node->block_low = new_block_low;
				curr_node->block_high = new_block_high;
				newbp->blocknode = curr_node;
				sbi->num_blocknode_allocated ++;
				list_add(&curr_node->link, &i->link);
				free_type_head = pmfs_get_type_head(sb,num_blocks,1);
				list_add(&newbp->link, free_type_head);
				sbi->num_free_blocks += num_blocks;
				goto block_found;
			}
		}

		if((new_block_low ==  (prev_block_high + 1)) && 
			(new_block_high == (i->block_low - 1)))
		{
			/* Fill the gap completely */
			pmfs_info("pmfs dbg info: __pmfs_free_block(): fill the gap\n");
			size= i->block_high - i->block_low +1;
			free_type_head = pmfs_get_type_head(sb,size,1);
			bp=i->blockp;
			if(prev_i){
				i->block_low = prev_i->block_low; // combine continus freeblocks
				new_size=i->block_high - i->block_low +1;
				list_del(&prev_i->link);  //delete previous freeblock node
				free_blocknode=prev_i;
				bp_prev=prev_i->blockp;
				list_del(&bp_prev->link);
				free_blockp=bp_prev;
				sbi->num_blocknode_allocated--;
			}
			else {
				i->block_low = new_block_low;
			}
			/* maintail three list structs */
			if(is_freeblks_type_change(sb,free_type_head,i)){
				list_del(&bp->link);
				new_size = i->block_high - i->block_low +1;
				free_type_head=pmfs_get_type_head(sb,new_size,1);
				list_add(&bp->link, free_type_head);
			}

			sbi->num_free_blocks += num_blocks;
			goto block_found;
		}

		if(new_block_low == (prev_block_high + 1) &&
			new_block_high < (i->block_low-1) )
		{
			/* Aligns left */
			//pmfs_info("pmfs dbg info: __pmfs_free_block(): aligns left\n");
			size = prev_i->block_high - prev_i->block_low +1;
			free_type_head = pmfs_get_type_head(sb,size,1);
			bp_prev = prev_i ->blockp;
			if(prev_i){
				prev_i->block_high = new_block_high;
				if(is_freeblks_type_change(sb,free_type_head,prev_i)){
					list_del(&bp_prev->link);
					new_size=prev_i->block_high - prev_i->block_low + 1;
					free_type_head = pmfs_get_type_head(sb,new_size,1);
					list_add(&bp_prev->link, free_type_head);
				}
			}
			else {
				curr_node = pmfs_alloc_blocknode(sb);
				newbp = pmfs_alloc_blockp();
				if(curr_node==NULL || newbp==NULL )
					PMFS_ASSERT(0);
				curr_node->blockp=newbp;
				curr_node->block_low=new_block_low;
				curr_node->block_high=new_block_high;
				newbp->blocknode=curr_node;
				list_add(&curr_node->link, free_head);
				free_type_head = pmfs_get_type_head(sb,num_blocks,1);
				list_add(&newbp->link,free_type_head);
				sbi->num_blocknode_allocated ++;
			}
			sbi->num_free_blocks += num_blocks;
			goto block_found;
		}

		if( (new_block_low > (prev_block_high+1)) && 
			(new_block_high == (i->block_low-1) ) )
		{
			/* Aligns to right */
			//pmfs_info("pmfs dbg info: __pmfs_free_block(): aligns right\n");
			size = i->block_high - i->block_low + 1;
			free_type_head = pmfs_get_type_head(sb,size,1);
			bp=i->blockp;
			i->block_low=new_block_low;
			if(is_freeblks_type_change(sb,free_type_head,i)){
				list_del(&bp->link);
				new_size = i->block_high - i->block_low + 1;
				free_type_head = pmfs_get_type_head(sb,new_size,1);
				list_add(&bp->link, free_type_head);
			}
			sbi->num_free_blocks += num_blocks;
			goto block_found;
		}

		if((new_block_low > (prev_block_high + 1)) &&
		    (new_block_high < (i->block_low - 1)))
		{
			//pmfs_info("pmfs dbg info: __pmfs_free_block(): aligns middle \n");
			curr_node = pmfs_alloc_blocknode(sb);
			newbp = pmfs_alloc_blockp();
			if(curr_node == NULL || newbp == NULL)
				PMFS_ASSERT(0);
			//pmfs_info("pmfs dbg info: __pmfs_free_block(): aligns middle : assert pass \n");
			curr_node->blockp = newbp;
			curr_node->block_low = new_block_low;
			curr_node->block_high = new_block_high;
			newbp->blocknode = curr_node;
			sbi->num_blocknode_allocated++;
			if(prev_i){
				list_add(&curr_node->link, &prev_i->link);
			}
			else{
				list_add(&curr_node->link, free_head);
			}
			free_type_head = pmfs_get_type_head(sb,num_blocks,1);
			//pmfs_info("pmfs dbg info: __pmfs_free_block(): aligns middle: get head pass \n");
			list_add(&newbp->link, free_type_head);
			sbi->num_free_blocks += num_blocks;
				
			goto block_found;
		}
		
		/*
		if ((new_block_low == i->block_low) &&
			(new_block_high == i->block_high)) {
			 //fits entire datablock 
			if (start_hint)
				*start_hint = pmfs_next_blocknode(i, head);
			list_del(&i->link);
			free_blocknode = i;
			sbi->num_blocknode_allocated--;
			sbi->num_free_blocks += num_blocks;
			goto block_found;
		}
		if ((new_block_low == i->block_low) &&
			(new_block_high < i->block_high)) {
			 //Aligns left 
			i->block_low = new_block_high + 1;
			sbi->num_free_blocks += num_blocks;
			if (start_hint)
				*start_hint = i;
			goto block_found;
		}
		if ((new_block_low > i->block_low) && 
			(new_block_high == i->block_high)) {
			//Aligns right 
			i->block_high = new_block_low - 1;
			sbi->num_free_blocks += num_blocks;
			if (start_hint)
				*start_hint = pmfs_next_blocknode(i, head);
			goto block_found;
		}
		if ((new_block_low > i->block_low) &&
			(new_block_high < i->block_high)) {
			//Aligns somewhere in the middle 
			curr_node = pmfs_alloc_blocknode(sb);
			PMFS_ASSERT(curr_node);
			if (curr_node == NULL) {
				// returning without freeing the block
				goto block_found;
			}
			curr_node->block_low = new_block_high + 1;
			curr_node->block_high = i->block_high;
			i->block_high = new_block_low - 1;
			list_add(&curr_node->link, &i->link);
			sbi->num_free_blocks += num_blocks;
			if (start_hint)
				*start_hint = curr_node;
			goto block_found;
		}
		*/
	}

	pmfs_error_mng(sb, "Unable to free block %ld\n", blocknr);

block_found:
	//pmfs_info("pmfs dbg info:__pmfs_free_block(): block found\n");
	if (free_blocknode)
		__pmfs_free_blocknode(free_blocknode);
	if(free_blockp)
		__pmfs_free_blockp(free_blockp);
	if(free_blockp2)
		__pmfs_free_blockp(free_blockp2);
}

void pmfs_free_block(struct super_block *sb, unsigned long blocknr,
		      unsigned short btype)
{
	struct pmfs_sb_info *sbi = PMFS_SB(sb);
	mutex_lock(&sbi->s_lock);
	__pmfs_free_block(sb, blocknr, btype, NULL);
	mutex_unlock(&sbi->s_lock);
}

int pmfs_new_block(struct super_block *sb, unsigned long *blocknr,
	unsigned short btype, int zero)
{
	struct pmfs_sb_info *sbi = PMFS_SB(sb);
	//struct list_head *head = &(sbi->block_inuse_head);
     	//struct list_head *free_head = &(sbi->block_free_head);
	struct list_head *free_type_head = NULL;
	struct pmfs_blocknode *i;
	struct pmfs_blockp *pi, *newpi;
	struct pmfs_blocknode *free_blocknode= NULL;
	struct pmfs_blockp *free_blockp = NULL;
	void *bp;
	unsigned long num_blocks = 0;
	//struct pmfs_blocknode *curr_node;
	int errval = 0;
	bool found = 0;
	unsigned long new_size;
	//unsigned long next_block_low;
	unsigned long new_block_low;
	unsigned long new_block_high;

	num_blocks = pmfs_get_numblocks(btype);

	mutex_lock(&sbi->s_lock);

	free_type_head=pmfs_get_type_head(sb, btype,0);
	if(!free_type_head)
		goto out;

	//pmfs_info("pmfs dbg info: pmfs_new_block(): get head pass \n");
	pi = list_first_entry(free_type_head, typeof(*pi), link); 
	if(pi==NULL){
		pmfs_info("pmfs dbg info: pmfs_new_block(): pi get first entry failed\n");
		goto out;
	}
	i=pi->blocknode;
	if(i==NULL){
		pmfs_info("pmfs dbg info:pmfs_new_block(): i is null\n");
		goto out;
	}
	
	new_block_low=i->block_low;  // aligns  left
	new_block_high=new_block_low + num_blocks - 1;
	
	//pmfs_info("pmfs dbg info: pmfs_new_block(): new_block_low=%lu,new_block_high=%lu, i->block_high=%lu \n",
	//				new_block_low,new_block_high,i->block_high);
	if(new_block_high == i->block_high ){ 
		/* Fits entire freeblocks */
		//pmfs_info("pmfs dbg info: pmfs_new_block(): fit gap \n");
		list_del(&i->link);
		list_del(&pi->link);
		free_blocknode = i;
		free_blockp = pi;
		sbi->num_blocknode_allocated--;
		sbi->num_free_blocks -= num_blocks;
		found= 1;
	}
	else if(new_block_high < i->block_high){
		/* Aligns left */
		//pmfs_info("pmfs dbg info: pmfs_new_block(): aligns left \n");
		i->block_low = new_block_high +1;
		sbi->num_free_blocks -= num_blocks;
		found = 1;
		/* maintain three list struct */
		if(is_freeblks_type_change(sb, free_type_head, i)){ //move pi to lower linklist
			list_del(&pi->link);
			new_size=i->block_high - i->block_low +1;
			free_type_head=pmfs_get_type_head(sb,new_size , 1);
			list_add(&pi->link, free_type_head);
		}
			
	}

/*
	list_for_each_entry(i, head, link) {
		if (i->link.next == head) {
			next_i = NULL;
			next_block_low = sbi->block_end;
		} else {
			next_i = list_entry(i->link.next, typeof(*i), link);
			next_block_low = next_i->block_low;
		}

		new_block_low = (i->block_high + num_blocks) & ~(num_blocks - 1);
		new_block_high = new_block_low + num_blocks - 1;

		if (new_block_high >= next_block_low) {
			// Does not fit - skip to next blocknode 
			continue;
		}

		if ((new_block_low == (i->block_high + 1)) &&
			(new_block_high == (next_block_low - 1)))
		{
			// Fill the gap completely 
			if (next_i) {
				i->block_high = next_i->block_high;
				list_del(&next_i->link);
				free_blocknode = next_i;
				sbi->num_blocknode_allocated--;
			} else {
				i->block_high = new_block_high;
			}
			found = 1;
			break;
		}

		if ((new_block_low == (i->block_high + 1)) &&
			(new_block_high < (next_block_low - 1))) {
			// Aligns to left 
			i->block_high = new_block_high;
			found = 1;
			break;
		}

		if ((new_block_low > (i->block_high + 1)) &&
			(new_block_high == (next_block_low - 1))) {
			// Aligns to right 
			if (next_i) {
				//right node exist 
				next_i->block_low = new_block_low;
			} else {
				// right node does NOT exist 
				curr_node = pmfs_alloc_blocknode(sb);
				PMFS_ASSERT(curr_node);
				if (curr_node == NULL) {
					errval = -ENOSPC;
					break;
				}
				curr_node->block_low = new_block_low;
				curr_node->block_high = new_block_high;
				list_add(&curr_node->link, &i->link);
			}
			found = 1;
			break;
		}

		if ((new_block_low > (i->block_high + 1)) &&
			(new_block_high < (next_block_low - 1))) {
			// Aligns somewhere in the middle 
			curr_node = pmfs_alloc_blocknode(sb);
			PMFS_ASSERT(curr_node);
			if (curr_node == NULL) {
				errval = -ENOSPC;
				break;
			}
			curr_node->block_low = new_block_low;
			curr_node->block_high = new_block_high;
			list_add(&curr_node->link, &i->link);
			found = 1;
			break;
		}
	} 
*/

	
	if (found == 1) {
		sbi->num_free_blocks -= num_blocks;
		//pmfs_info("pmfs dbg info:pmfs_new_block(): block found\n")
	}	

	mutex_unlock(&sbi->s_lock);

	if (free_blocknode)
		__pmfs_free_blocknode(free_blocknode);
	if(free_blockp)
		__pmfs_free_blockp(free_blockp);
out:
	if (found == 0) {
		//pmfs_error_mng(sb, "alloc new block failed\n");
		return -ENOSPC;
	}

	if (zero) {
		size_t size;
		bp = pmfs_get_block(sb, pmfs_get_block_off(sb, new_block_low, btype));
		pmfs_memunlock_block(sb, bp); //TBDTBD: Need to fix this
		if (btype == PMFS_BLOCK_TYPE_4K)
			size = 0x1 << 12;
		else if (btype == PMFS_BLOCK_TYPE_2M)
			size = 0x1 << 21;
		else
			size = 0x1 << 30;
		memset_nt(bp, 0, size);
		pmfs_memlock_block(sb, bp);
	}
	*blocknr = new_block_low;

	return errval;
}

unsigned long pmfs_count_free_blocks(struct super_block *sb)
{
	struct pmfs_sb_info *sbi = PMFS_SB(sb);
	return sbi->num_free_blocks; 
}
