/*
*   Software Updater - client side
*
*      Copyright (c) 2012-2016 Intel Corporation.
*
*   This program is free software: you can redistribute it and/or modify
*   it under the terms of the GNU General Public License as published by
*   the Free Software Foundation, version 2 or later of the License.
*
*   This program is distributed in the hope that it will be useful,
*   but WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*   GNU General Public License for more details.
*
*   You should have received a copy of the GNU General Public License
*   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*   Authors:
*         Jaime A. Garcia <jaime.garcia.naranjo@linux.intel.com>
*         Tim Pepper <timothy.c.pepper@linux.intel.com>
*
*/

#define _GNU_SOURCE
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "config.h"
#include "swupd.h"

#define MODE_RW_O (S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)

/*
* list_installable_bundles()
* Parse the full manifest for the current version of the OS and print
*   all available bundles.
*/
int list_installable_bundles()
{
	struct list *list;
	struct file *file;
	struct manifest *MoM = NULL;
	int current_version;
	int lock_fd;
	int ret;

	ret = swupd_init(&lock_fd);
	if (ret != 0) {
		printf("Error: Failed updater initialization. Exiting now\n");
		return ret;
	}

	if (!check_network()) {
		printf("Error: Network issue, unable to download manifest\n");
		v_lockfile(lock_fd);
		return EXIT_FAILURE;
	}

	current_version = get_current_version(path_prefix);
	if (current_version < 0) {
		printf("Error: Unable to determine current OS version\n");
		v_lockfile(lock_fd);
		return ECURRENT_VERSION;
	}

	swupd_curl_set_current_version(current_version);

	MoM = load_mom(current_version);
	if (!MoM) {
		v_lockfile(lock_fd);
		return ret;
	}

	list = MoM->manifests;
	while (list) {
		file = list->data;
		list = list->next;
		printf("%s\n", file->filename);
	}

	free_manifest(MoM);
	v_lockfile(lock_fd);
	return 0;
}

/* bundle_name: this is the name for the bundle we want to be loaded
* version: this is the MoM version from which we pull last changed for bundle manifest
* submanifest: where bundle manifest is going to be loaded
*
* Basically we read MoM version then get the submanifest only for our bundle (component)
* put it into submanifest pointer, then dispose MoM data.
*/
static int load_bundle_manifest(const char *bundle_name, int version, struct manifest **submanifest)
{
	struct list *sub_list = NULL;
	struct manifest *mom = NULL;
	int ret = 0;

	*submanifest = NULL;

	swupd_curl_set_current_version(version);
	mom = load_mom(version);
	if (!mom) {
		return EMOM_NOTFOUND;
	}

	sub_list = recurse_manifest(mom, bundle_name);
	if (!sub_list) {
		ret = ERECURSE_MANIFEST;
		goto free_out;
	}

	*submanifest = sub_list->data;
	sub_list->data = NULL;
	ret = 0;

free_out:
	list_free_list(sub_list);
	free_manifest(mom);
	return ret;
}

/* Finds out whether bundle_name is tracked bundle on
*  current system.
*/
bool is_tracked_bundle(const char *bundle_name)
{
	struct stat statb;
	char *filename = NULL;
	bool ret = true;

	string_or_die(&filename, "%s/%s/%s", path_prefix, BUNDLES_DIR, bundle_name);

	if (stat(filename, &statb) == -1) {
		ret = false;
	}

	free(filename);
	return ret;
}

/* When loading all tracked bundles into memory, they happen
 * to be hold in subs global var, for some reasons it is
 * needed to just pop out one or more of this loaded tracked
 * bundles, this function search for bundle_name into subs
 * struct and if it found then free it from the list.
 */
static int unload_tracked_bundle(const char *bundle_name)
{
	struct list *bundles;
	struct list *cur_item;
	struct sub *bundle;

	bundles = list_head(subs);
	while (bundles) {
		bundle = bundles->data;
		cur_item = bundles;
		bundles = bundles->next;
		if (strcmp(bundle->component, bundle_name) == 0) {
			/* unlink (aka untrack) matching bundle name from tracked ones */
			subs = free_bundle(cur_item);
			return EXIT_SUCCESS;
		}
	}

	return EBUNDLE_NOT_TRACKED;
}

/* Check if bundle is included by any subscribed bundles */
static bool is_included(const char *bundle_name, struct manifest *mom)
{
	bool ret = false;
	struct list *b, *i;

	b = list_head(mom->submanifests);
	while (b) {
		struct manifest *bundle = b->data;
		b = b->next;

		i = list_head(bundle->includes);
		while (i) {
			char *name = i->data;
			i = i->next;

			if (strcmp(name, bundle_name) == 0) {
				ret = true;
				goto out;
			}
		}
	}

out:
	return ret;
}

/*  This function is a fresh new implementation for a bundle
 *  remove without being tied to verify loop, this means
 *  improved speed and space as well as more roubustness and
 *  flexibility. What it does is basically:
 *
 *  1) Read MoM and load all submanifests except the one to be
 *  	removed and then consolidate them.
 *  2) Load the removed bundle submanifest.
 *  3) Order the file list by filename
 *  4) Deduplicate removed submanifest file list that happens
 *  	to be on the MoM (minus bundle to be removed).
 *  5) iterate over to be removed bundle submanifest file list
 *  	performing a unlink(2) for each filename.
 *  6) Done.
 */
int remove_bundle(const char *bundle_name)
{
	int lock_fd;
	int ret = 0;
	int current_version = CURRENT_OS_VERSION;
	struct manifest *current_mom, *bundle_manifest = NULL;

	ret = swupd_init(&lock_fd);
	if (ret != 0) {
		printf("Failed updater initialization, exiting now.\n");
		return ret;
	}

	/* os-core bundle not allowed to be removed...
	 * although this is going to be caught later because of all files
	 * being marked as 'duplicated' and note removing anything
	 * anyways, better catch here and return success, no extra work to be done.
	 */
	if (strcmp(bundle_name, "os-core") == 0) {
		ret = EBUNDLE_NOT_TRACKED;
		goto out_free_curl;
	}

	if (!is_tracked_bundle(bundle_name)) {
		ret = EBUNDLE_NOT_TRACKED;
		printf("Warning: Bundle \"%s\" does not seem to be installed\n", bundle_name);
		goto out_free_curl;
	}

	current_version = get_current_version(path_prefix);
	if (current_version < 0) {
		printf("Error: Unable to determine current OS version\n");
		ret = ECURRENT_VERSION;
		goto out_free_curl;
	}

	swupd_curl_set_current_version(current_version);

	current_mom = load_mom(current_version);
	if (!current_mom) {
		printf("Unable to download/verify %d Manifest.MoM\n", current_version);
		ret = EMOM_NOTFOUND;
		goto out_free_curl;
	}

	if (!search_bundle_in_manifest(current_mom, bundle_name)) {
		printf("Bundle name is invalid, aborting removal\n");
		ret = EBUNDLE_REMOVE;
		goto out_free_mom;
	}

	/* load all tracked bundles into memory */
	read_subscriptions_alt();
	/* now popout the one to be removed */
	ret = unload_tracked_bundle(bundle_name);
	if (ret != 0) {
		goto out_free_mom;
	}

	subscription_versions_from_MoM(current_mom, 0);

	/* load all submanifest minus the one to be removed */
	current_mom->submanifests = recurse_manifest(current_mom, NULL);
	if (!current_mom->submanifests) {
		printf("Error: Cannot load MoM sub-manifests\n");
		ret = ERECURSE_MANIFEST;
		goto out_free_mom;
	}

	if (is_included(bundle_name, current_mom)) {
		printf("Error: bundle requested to be removed is required by other installed bundles\n");
		ret = EBUNDLE_REMOVE;
		goto out_free_mom;
	}

	current_mom->files = files_from_bundles(current_mom->submanifests);

	current_mom->files = consolidate_files(current_mom->files);

	/* Now that we have the consolidated list of all files, load bundle to be removed submanifest*/
	ret = load_bundle_manifest(bundle_name, current_version, &bundle_manifest);
	if (ret != 0 || !bundle_manifest) {
		printf("Error: Cannot load %s sub-manifest (ret = %d)\n", bundle_name, ret);
		goto out_free_mom;
	}

	/* deduplication needs file list sorted by filename, do so */
	bundle_manifest->files = list_sort(bundle_manifest->files, file_sort_filename);
	deduplicate_files_from_manifest(&bundle_manifest, current_mom);

	printf("Deleting bundle files...\n");
	remove_files_in_manifest_from_fs(bundle_manifest);

	printf("Untracking bundle from system...\n");
	rm_bundle_file(bundle_name);

	printf("Success: Bundle removed\n");

	free_manifest(bundle_manifest);
out_free_mom:
	free_manifest(current_mom);
out_free_curl:

	if (ret) {
		printf("Error: Bundle remove failed\n");
	}

	swupd_deinit(lock_fd);

	return ret;
}

/* tristate return, -1 for errors, 1 for no errors but no new subscriptions, 0 for no errors and new subscriptions */
int add_subscriptions(struct list *bundles, int current_version, struct manifest *mom)
{
	bool new_bundles = false;
	char *bundle;
	int ret;
	int retries = 0;
	int timeout = 10;
	struct file *file;
	struct list *iter;
	struct manifest *manifest;

	srand(time(NULL));

	iter = list_head(bundles);
	while (iter) {
		bundle = iter->data;
		iter = iter->next;

		file = search_bundle_in_manifest(mom, bundle);
		if (!file) {
			printf("%s bundle name is invalid, skipping it...\n", bundle);
			continue;
		}

	retry_manifest_download:
		manifest = load_manifest(current_version, file->last_change, file, mom);
		if (!manifest) {
			if (retries < MAX_TRIES) {
				increment_retries(&retries, &timeout);
				goto retry_manifest_download;
			}
			printf("Unable to download manifest %s version %d, exiting now\n", bundle, file->last_change);
			ret = -1;
			goto out;
		}

		if (manifest->includes) {
			ret = add_subscriptions(manifest->includes, current_version, mom);
			if (ret == -1) {
				free_manifest(manifest);
				goto out;
			} else if (ret == 0) {
				new_bundles = true;
			}
		}
		free_manifest(manifest);

		if (is_tracked_bundle(bundle)) {
			continue;
		}

		if (component_subscribed(bundle)) {
			continue;
		}
		create_and_append_subscription(bundle);
		new_bundles = true;
	}
	if (new_bundles) {
		ret = 0;
	} else {
		ret = 1;
	}

out:
	return ret;
}

static int install_bundles(struct list *bundles, int current_version, struct manifest *mom)
{
	int ret;
	struct file *file;
	struct list *iter;
	struct list *to_install_bundles = NULL;
	struct list *to_install_files = NULL;

	/* step 1: check bundle args are valid if so populate subs struct */
	ret = add_subscriptions(bundles, current_version, mom);

	if (ret) {
		if (ret == 1) {
			printf("bundle(s) already installed, exiting now\n");
		}
		ret = EBUNDLE_INSTALL;
		goto out;
	}

	subscription_versions_from_MoM(mom, 0);

	to_install_bundles = recurse_manifest(mom, NULL);
	if (!to_install_bundles) {
		printf("Error: Cannot load to install bundles\n");
		ret = ERECURSE_MANIFEST;
		goto out;
	}

	to_install_files = files_from_bundles(to_install_bundles);
	to_install_files = consolidate_files(to_install_files);

	/* step 2: download neccessary packs */

	(void)rm_staging_dir_contents("download");

	printf("Downloading packs...\n");
	(void)download_subscribed_packs(true);

	/* step 3: Add tracked bundles */
	read_subscriptions_alt();
	subscription_versions_from_MoM(mom, 0);
	mom->submanifests = recurse_manifest(mom, NULL);
	if (!mom->submanifests) {
		printf("Error: Cannot load installed bundles\n");
		ret = ERECURSE_MANIFEST;
		goto out;
	}

	mom->files = files_from_bundles(mom->submanifests);
	mom->files = consolidate_files(mom->files);

	/* step 4: Install all bundle(s) files into the fs */

	printf("Installing bundle(s) files...\n");
	iter = list_head(to_install_files);
	while (iter) {
		file = iter->data;
		iter = iter->next;

		if (file->is_deleted || file->do_not_update || ignore(file)) {
			continue;
		}

		ret = do_staging(file, mom);
		if (ret) {
			ret = verify_fix_path(file->filename, mom);
		}
		if (ret) {
			ret = EBUNDLE_INSTALL;
			goto out;
		}
	}

	iter = list_head(to_install_files);
	while (iter) {
		file = iter->data;
		iter = iter->next;

		if (file->is_deleted || file->do_not_update || ignore(file)) {
			continue;
		}

		/* This was staged by verify_fix_path */
		if (!file->staging) {
			file = search_file_in_manifest(mom, file->filename);
		}

		rename_staged_file_to_final(file);
	}

	sync();

	/* step 5: Run any scripts that are needed to complete update */
	run_scripts();

	ret = 0;
	printf("Bundle(s) installation done.\n");

out:
	if (to_install_files) {
		list_free_list(to_install_files);
	}
	if (to_install_bundles) {
		list_free_list_and_data(to_install_bundles, free_manifest_data);
	}
	free_subscriptions();
	return ret;
}

/* Bundle install one ore more bundles passed in bundles
 * param as a null terminated array of strings
 */
int install_bundles_frontend(char **bundles)
{
	int lock_fd;
	int ret = 0;
	int current_version;
	struct list *bundles_list = NULL;
	struct manifest *mom;

	/* initialize swupd and get current version from OS */
	ret = swupd_init(&lock_fd);
	if (ret != 0) {
		printf("Failed updater initialization, exiting now.\n");
		return ret;
	}

	current_version = get_current_version(path_prefix);
	if (current_version < 0) {
		printf("Error: Unable to determine current OS version\n");
		ret = ECURRENT_VERSION;
		goto clean_and_exit;
	}

	swupd_curl_set_current_version(current_version);

	mom = load_mom(current_version);
	if (!mom) {
		printf("Cannot load official manifest MoM for version %i\n", current_version);
		ret = EMOM_NOTFOUND;
		goto clean_and_exit;
	}

	for (; *bundles; ++bundles) {
		bundles_list = list_prepend_data(bundles_list, *bundles);
	}

	ret = install_bundles(bundles_list, current_version, mom);
	list_free_list(bundles_list);

	free_manifest(mom);
clean_and_exit:
	swupd_deinit(lock_fd);

	return ret;
}
