/*
 * Copyright Gavin Shan, Alibaba Inc 2018.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef _EXT4_EXTEND
#define _EXT4_EXTEND

#include <linux/writeback.h>

#ifdef CONFIG_EXT4_FS_EXTEND

#define EXT4_EXT_DEFAULT_DELAY_UPDATE_TIME	300000	/* 5 minutes */

struct ext4_ext_sb_info {
	unsigned int    s_opt;
#define EXT4_EXT_OPT_VALID		(1 << 0)
#define EXT4_EXT_OPT_DELAY_UPDATE_TIME	(1 << 1)
	struct mutex    s_mutex;
	struct kobject  s_kobj;
	unsigned long	s_delay_update_time;
};

int ext4_handle_ext_mount_opt(struct super_block *sb, char *opt, int token,
			      substring_t *param, unsigned long *journal_devnum,
			      unsigned int *journal_ioprio, int is_remount);
int ext4_register_ext_sysfs(struct super_block *sb);
void ext4_unregister_ext_sysfs(struct super_block *sb);
int ext4_ext_update_time(struct inode *inode, struct timespec *tm, int flags);

#else /* !CONFIG_EXT4_FS_EXTEND */

static inline int ext4_handle_ext_mount_opt(struct super_block *sb, char *opt,
					    int token, substring_t *param,
					    unsigned long *journal_devnum,
					    unsigned int *journal_ioprio,
					    int is_remount)
{
	return 0;
}

static inline int ext4_register_ext_sysfs(struct super_block *sb)
{
	return 0;
}

static inline void ext4_unregister_ext_sysfs(struct super_block *sb)
{
}

#endif /* CONFIG_EXT4_FS_EXTEND */
#endif /* _EXT4_EXTEND */
