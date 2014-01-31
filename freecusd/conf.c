/*
 * Copyright 2014 Ian Pilcher <arequipeno@gmail.com>
 *
 * This program is free software.  You can redistribute it or modify it under
 * the terms of version 2 of the GNU General Public License (GPL), as published
 * by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY -- without even the implied warranty of MERCHANTIBILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the text of the GPL for more details.
 *
 * Version 2 of the GNU General Public License is available at:
 *
 *   http://www.gnu.org/licenses/old-licenses/gpl-2.0.html
 */

#include "freecusd.h"

#include <string.h>
#include <errno.h>

/*
 * Configuration file name
 */
const char *fcd_conf_file_name = NULL;

/*
 * Number and names of RAID disks
 */
unsigned fcd_conf_disk_count = 0;
char fcd_conf_disk_names[FCD_MAX_DISK_COUNT][FCD_DISK_NAME_SIZE];

/*
 * Default RAID disks
 */
static char *fcd_conf_disk_names_values[] = {
	"/dev/sdb", "/dev/sdc", "/dev/sdd", "/dev/sde", "/dev/sdf"
};

static const cip_str_list fcd_conf_disk_names_def = {
	.values	= fcd_conf_disk_names_values,
	.count	= FCD_ARRAY_SIZE(fcd_conf_disk_names_values),
};

/*
 * [freecusd] section schema skeleton
 */
static int fcd_conf_raiddisks_cb();

static const cip_opt_info fcd_conf_freecusd_opts[] = {
	{
		.name		= "raid_disks",
		.type		= CIP_OPT_TYPE_STR_LIST,
		.post_parse_fn	= fcd_conf_raiddisks_cb,
		.default_value	= &fcd_conf_disk_names_def,
		.flags		= CIP_OPT_DEFAULT,
	},
	{ 0 }
};

static const cip_sect_info fcd_conf_freecusd_sect = {
	.name		= "freecusd",
	.options	= fcd_conf_freecusd_opts,
	.flags		= CIP_SECT_CREATE,
};

/*
 * raid_disks post-parse callback
 */
static int fcd_conf_raiddisks_cb(cip_err_ctx *ctx, const cip_ini_value *value,
				 const cip_ini_sect *sect
						__attribute__((unused)),
				 const cip_ini_file *file
						__attribute__((unused)),
				 void *post_parse_data __attribute__((unused)))
{
	const cip_str_list *list;
	const char *disk;
	unsigned i, j;

	list = (const cip_str_list *)(value->value);
	if (list->count < 1 || list->count > FCD_MAX_DISK_COUNT) {
		cip_err(ctx,
			"Number of disks (%u) outside valid range (1 - %d)",
			list->count, FCD_MAX_DISK_COUNT);
		return -1;
	}

	for (i = 0; i < list->count; ++i) {

		disk = list->values[i];

		if (strlen(disk) != FCD_DISK_NAME_SIZE - 1 	||
			strncmp(disk, "/dev/sd",
				FCD_DISK_NAME_SIZE - 2) != 0 	||
			disk[FCD_DISK_NAME_SIZE - 2] < 'a' 	||
			disk[FCD_DISK_NAME_SIZE - 2] > 'z'	) {

			cip_err(ctx, "Invalid disk: %s", list->values[i]);
			return -1;
		}

		for (j = 0; j < i; ++j) {

			if (disk[FCD_DISK_NAME_SIZE - 2] ==
				list->values[j][FCD_DISK_NAME_SIZE - 2]) {

				cip_err(ctx, "Duplicate disk: %s", disk);
				return -1;
			}
		}
	}

	for (i = 0; i < list->count; ++i) {
		memcpy(fcd_conf_disk_names[i], list->values[i],
		       FCD_DISK_NAME_SIZE);
	}

	fcd_conf_disk_count = list->count;

	return 0;
}

/*
 * Post-parse callback for monitor enable/disable booleans
 */
int fcd_conf_mon_enable_cb(cip_err_ctx *ctx __attribute__((unused)),
			   const cip_ini_value *value,
			   const cip_ini_sect *sect __attribute__((unused)),
			   const cip_ini_file *file __attribute__((unused)),
			   void *post_parse_data)
{
	struct fcd_monitor *mon;
	const bool *b;

	mon = (struct fcd_monitor *)post_parse_data;
	b = (const bool *)(value->value);

	mon->enabled = *b;
	if (!mon->enabled) {
		FCD_INFO("%s monitor disabled by configuration setting\n",
			 mon->name);
	}

	return 0;
}

/*
 * Parse the configuration file
 */
static int fcd_conf_warn(const char *msg)
{
	FCD_WARN("%s\n", msg);
	return 0;
}

void fcd_conf_parse(void)
{
	cip_sect_schema *freecusd_schema, *raiddisk_schema;
	cip_file_schema *file_schema;
	const char *cfg_file_name;
	struct fcd_monitor **mon;
	cip_ini_file *file;
	cip_err_ctx ctx;
	FILE *stream;
	int ret;

	cip_err_ctx_init(&ctx);

	file_schema = cip_file_schema_new1(&ctx);
	if (file_schema == NULL)
		FCD_FATAL("%s\n", cip_last_err(&ctx));

	freecusd_schema = cip_sect_schema_new2(&ctx, file_schema,
					       &fcd_conf_freecusd_sect);
	if (freecusd_schema == NULL)
		FCD_FATAL("%s\n", cip_last_err(&ctx));

	raiddisk_schema = cip_sect_schema_new1(&ctx, file_schema, "raid_disk",
					       CIP_SECT_MULTIPLE);
	if (raiddisk_schema == NULL)
		FCD_FATAL("%s\n", cip_last_err(&ctx));

	for (mon = fcd_monitors; *mon != NULL; ++mon) {

		if ((*mon)->enabled_opt_name != NULL) {
			ret = cip_opt_schema_new1(&ctx, freecusd_schema,
						  (*mon)->enabled_opt_name,
						  CIP_OPT_TYPE_BOOL,
						  fcd_conf_mon_enable_cb,
						  *mon, 0, NULL);
			if (ret == -1)
				FCD_FATAL("%s\n", cip_last_err(&ctx));
		}

		if ((*mon)->freecusd_opts != NULL) {
			ret = cip_opt_schema_new3(&ctx, freecusd_schema,
						  (*mon)->freecusd_opts);
			if (ret == -1)
				FCD_FATAL("%s\n", cip_last_err(&ctx));
		}

		if ((*mon)->raiddisk_opts != NULL) {
			ret = cip_opt_schema_new3(&ctx, raiddisk_schema,
						  (*mon)->raiddisk_opts);
			if (ret == -1)
				FCD_FATAL("%s\n", cip_last_err(&ctx));
		}
	}

	cfg_file_name = (fcd_conf_file_name != NULL) ? fcd_conf_file_name :
						"/etc/freecusd.conf";
	stream = fopen(cfg_file_name, "re");
	if (stream == NULL) {
		if (fcd_conf_file_name == NULL && errno == ENOENT)
			cfg_file_name = "(none)";
		else
			FCD_FATAL("Failed to open configuration file: %s: %m\n",
				  cfg_file_name);
	}

	file = cip_parse_stream(&ctx, stream, cfg_file_name, file_schema,
				fcd_conf_warn);
	if (file == NULL)
		FCD_FATAL("%s\n", cip_last_err(&ctx));

	if (stream != NULL && fclose(stream) == EOF)
		FCD_PERROR(cfg_file_name);

	cip_ini_file_free(file);
	cip_file_schema_free(file_schema);
	cip_err_ctx_fini(&ctx);
}

