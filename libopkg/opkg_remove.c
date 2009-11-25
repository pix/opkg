/* opkg_remove.c - the opkg package management system

   Carl D. Worth

   Copyright (C) 2001 University of Southern California

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2, or (at
   your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.
*/

#include "includes.h"
#include "opkg_message.h"

#include <glob.h>

#include "opkg_remove.h"
#include "opkg_error.h"
#include "opkg_cmd.h"

#include "file_util.h"
#include "sprintf_alloc.h"
#include "str_util.h"
#include "libbb/libbb.h"

/*
 * Returns number of the number of packages depending on the packages provided by this package.
 * Every package implicitly provides itself.
 */
int
pkg_has_installed_dependents(opkg_conf_t *conf, abstract_pkg_t *parent_apkg, pkg_t *pkg, abstract_pkg_t *** pdependents)
{
     int nprovides = pkg->provides_count;
     abstract_pkg_t **provides = pkg->provides;
     int n_installed_dependents = 0;
     int i;
     for (i = 0; i < nprovides; i++) {
	  abstract_pkg_t *providee = provides[i];
	  abstract_pkg_t **dependers = providee->depended_upon_by;
	  abstract_pkg_t *dep_ab_pkg;
	  if (dependers == NULL)
	       continue;
	  while ((dep_ab_pkg = *dependers++) != NULL) {
	       if (dep_ab_pkg->state_status == SS_INSTALLED){
		    n_installed_dependents++;
               }
	  }

     }
     /* if caller requested the set of installed dependents */
     if (pdependents) {
	  int p = 0;
	  abstract_pkg_t **dependents = xcalloc((n_installed_dependents+1), sizeof(abstract_pkg_t *));

	  *pdependents = dependents;
	  for (i = 0; i < nprovides; i++) {
	       abstract_pkg_t *providee = provides[i];
	       abstract_pkg_t **dependers = providee->depended_upon_by;
	       abstract_pkg_t *dep_ab_pkg;
	       if (dependers == NULL)
		    continue;
	       while ((dep_ab_pkg = *dependers++) != NULL) {
		    if (dep_ab_pkg->state_status == SS_INSTALLED && !(dep_ab_pkg->state_flag & SF_MARKED)) {
			 dependents[p++] = dep_ab_pkg;
			 dep_ab_pkg->state_flag |= SF_MARKED;
		    }
	       }
	  }
	  dependents[p] = NULL;
	  /* now clear the marks */
	  for (i = 0; i < p; i++) {
	       abstract_pkg_t *dep_ab_pkg = dependents[i];
	       dep_ab_pkg->state_flag &= ~SF_MARKED;
	  }
     }
     return n_installed_dependents;
}

static int
opkg_remove_dependent_pkgs (opkg_conf_t *conf, pkg_t *pkg, abstract_pkg_t **dependents)
{
    int i;
    int a;
    int count;
    pkg_vec_t *dependent_pkgs;
    abstract_pkg_t * ab_pkg;

    if((ab_pkg = pkg->parent) == NULL){
	fprintf(stderr, "%s: unable to get dependent pkgs. pkg %s isn't in hash table\n",
		__FUNCTION__, pkg->name);
	return 0;
    }
    
    if (dependents == NULL)
	    return 0;

    // here i am using the dependencies_checked
    if (ab_pkg->dependencies_checked == 2) // variable to make out whether this package
	return 0;			   // has already been encountered in the process
	                                   // of marking packages for removal - Karthik
    ab_pkg->dependencies_checked = 2;

    i = 0;
    count = 1;
    dependent_pkgs = pkg_vec_alloc();

    while (dependents [i] != NULL) {
        abstract_pkg_t *dep_ab_pkg = dependents[i];
	
	if (dep_ab_pkg->dependencies_checked == 2){
	    i++;
	    continue;	
        }
        if (dep_ab_pkg->state_status == SS_INSTALLED) {
            for (a = 0; a < dep_ab_pkg->pkgs->len; a++) {
                pkg_t *dep_pkg = dep_ab_pkg->pkgs->pkgs[a];
                if (dep_pkg->state_status == SS_INSTALLED) {
                    pkg_vec_insert(dependent_pkgs, dep_pkg);
                    count++;
                }
            }
        }
	i++;
	/* 1 - to keep track of visited ab_pkgs when checking for possiblility of a broken removal of pkgs.
	 * 2 - to keep track of pkgs whose deps have been checked alrdy  - Karthik */	
    }
    
    if (count == 1) {
        pkg_vec_free(dependent_pkgs);  
	return 0;
    }
    
    
    int err=0;
    for (i = 0; i < dependent_pkgs->len; i++) {
        err = opkg_remove_pkg(conf, dependent_pkgs->pkgs[i],0);
        if (err) {
            pkg_vec_free(dependent_pkgs);
            break;
	}
    }
    pkg_vec_free(dependent_pkgs);
    return err;
}

static void
print_dependents_warning(opkg_conf_t *conf, abstract_pkg_t *abpkg, pkg_t *pkg, abstract_pkg_t **dependents)
{
    abstract_pkg_t *dep_ab_pkg;
    opkg_message(conf, OPKG_ERROR, "Package %s is depended upon by packages:\n", pkg->name);
    while ((dep_ab_pkg = *dependents++) != NULL) {
	 if (dep_ab_pkg->state_status == SS_INSTALLED)
	      opkg_message(conf, OPKG_ERROR, "\t%s\n", dep_ab_pkg->name);
    }
    opkg_message(conf, OPKG_ERROR, "These might cease to work if package %s is removed.\n\n", pkg->name);
    opkg_message(conf, OPKG_ERROR, "");
    opkg_message(conf, OPKG_ERROR, "You can force removal of this package with --force-depends.\n");
    opkg_message(conf, OPKG_ERROR, "You can force removal of this package and its dependents\n");
    opkg_message(conf, OPKG_ERROR, "with --force-removal-of-dependent-packages\n");
}

/*
 * Find and remove packages that were autoinstalled and are orphaned
 * by the removal of pkg.
 */
static int
remove_autoinstalled(opkg_conf_t *conf, pkg_t *pkg)
{
	int i, j;
	int n_deps;
	pkg_t *p;
	struct compound_depend *cdep;
	abstract_pkg_t **dependents;

	int count = pkg->pre_depends_count +
				pkg->depends_count +
				pkg->recommends_count +
				pkg->suggests_count;

	for (i=0; i<count; i++) {
		cdep = &pkg->depends[i];
		if (cdep->type != DEPEND)
			continue;
		for (j=0; j<cdep->possibility_count; j++) {
			p = pkg_hash_fetch_installed_by_name (&conf->pkg_hash,
					cdep->possibilities[j]->pkg->name);

			/* If the package is not installed, this could have
			 * been a circular dependency and the package has
			 * already been removed.
			 */
			if (!p)
				return -1;

			if (!p->auto_installed)
				continue;

			n_deps = pkg_has_installed_dependents(conf, NULL, p,
					&dependents);
			if (n_deps == 0) {
				 opkg_message(conf, OPKG_NOTICE,
				               "%s was autoinstalled and is "
					       "now orphaned, removing\n",
					       p->name);
			         opkg_remove_pkg(conf, p, 0);
			} else
				opkg_message(conf, OPKG_INFO,
						"%s was autoinstalled and is "
						"still required by %d "
						"installed packages.\n",
						p->name, n_deps);

			if (dependents)
				free(dependents);
		}
	}

	return 0;
}

int
opkg_remove_pkg(opkg_conf_t *conf, pkg_t *pkg, int from_upgrade)
{
     int err;
     abstract_pkg_t *parent_pkg = NULL;

/*
 * If called from an upgrade and not from a normal remove,
 * ignore the essential flag.
 */
     if (pkg->essential && !from_upgrade) {
	  if (conf->force_removal_of_essential_packages) {
	       fprintf(stderr, "WARNING: Removing essential package %s under your coercion.\n"
		       "\tIf your system breaks, you get to keep both pieces\n",
		       pkg->name);
	  } else {
	       fprintf(stderr, "ERROR: Refusing to remove essential package %s.\n"
		       "\tRemoving an essential package may lead to an unusable system, but if\n"
		       "\tyou enjoy that kind of pain, you can force opkg to proceed against\n"
		       "\tits will with the option: --force-removal-of-essential-packages\n",
		       pkg->name);
	       return OPKG_PKG_IS_ESSENTIAL;
	  }
     }

     if ((parent_pkg = pkg->parent) == NULL)
	  return 0;

     /* only attempt to remove dependent installed packages if
      * force_depends is not specified or the package is being
      * replaced.
      */
     if (!conf->force_depends
	 && !(pkg->state_flag & SF_REPLACE)) {
	  abstract_pkg_t **dependents;
	  int has_installed_dependents = 
	       pkg_has_installed_dependents(conf, parent_pkg, pkg, &dependents);

	  if (has_installed_dependents) {
	       /*
		* if this package is depended upon by others, then either we should
		* not remove it or we should remove it and all of its dependents 
		*/

	       if (!conf->force_removal_of_dependent_packages) {
		    print_dependents_warning(conf, parent_pkg, pkg, dependents);
		    free(dependents);
		    return OPKG_PKG_HAS_DEPENDENTS;
	       }

	       /* remove packages depending on this package - Karthik */
	       err = opkg_remove_dependent_pkgs (conf, pkg, dependents);
	       if (err) {
	         free(dependents);
                 return err;
               }
	  }
          if (dependents)
              free(dependents);
     }

     if (from_upgrade == 0) {
         opkg_message (conf, OPKG_NOTICE,
	               "Removing package %s from %s...\n", pkg->name, pkg->dest->name);
     }
     pkg->state_flag |= SF_FILELIST_CHANGED;

     pkg->state_want = SW_DEINSTALL;
     opkg_state_changed++;

     pkg_run_script(conf, pkg, "prerm", "remove");

     /* DPKG_INCOMPATIBILITY: dpkg is slightly different here. It
	maintains an empty filelist rather than deleting it. That seems
	like a big pain, and I don't see that that should make a big
	difference, but for anyone who wants tighter compatibility,
	feel free to fix this. */
     remove_data_files_and_list(conf, pkg);

     pkg_run_script(conf, pkg, "postrm", "remove");

     remove_maintainer_scripts(conf, pkg);
     pkg->state_status = SS_NOT_INSTALLED;

     if (parent_pkg) 
	  parent_pkg->state_status = SS_NOT_INSTALLED;

     /* remove autoinstalled packages that are orphaned by the removal of this one */
     if (conf->autoremove)
       remove_autoinstalled (conf, pkg);

     return 0;
}

void
remove_data_files_and_list(opkg_conf_t *conf, pkg_t *pkg)
{
     str_list_t installed_dirs;
     str_list_t *installed_files;
     str_list_elt_t *iter;
     char *file_name;
     conffile_t *conffile;
     int removed_a_dir;
     pkg_t *owner;
     int rootdirlen = 0;

     str_list_init(&installed_dirs);
     installed_files = pkg_get_installed_files(conf, pkg);

     /* don't include trailing slash */
     if (conf->offline_root)
          rootdirlen = strlen(conf->offline_root);

     for (iter = str_list_first(installed_files); iter; iter = str_list_next(installed_files, iter)) {
	  file_name = (char *)iter->data;

	  if (file_is_dir(file_name)) {
	       str_list_append(&installed_dirs, file_name);
	       continue;
	  }

	  conffile = pkg_get_conffile(pkg, file_name+rootdirlen);
	  if (conffile) {
	       if (conffile_has_been_modified(conf, conffile)) {
		    opkg_message (conf, OPKG_NOTICE,
		                  "  not deleting modified conffile %s\n", file_name);
		    continue;
	       }
	  }

	  opkg_message(conf, OPKG_INFO, "  deleting %s (noaction=%d)\n", file_name, conf->noaction);
	  if (!conf->noaction)
	       unlink(file_name);
     }

     /* Remove empty directories */
     if (!conf->noaction) {
	  do {
	       removed_a_dir = 0;
	       for (iter = str_list_first(&installed_dirs); iter; iter = str_list_next(&installed_dirs, iter)) {
		    file_name = (char *)iter->data;
	    
		    if (rmdir(file_name) == 0) {
			 opkg_message(conf, OPKG_INFO, "  deleting %s\n", file_name);
			 removed_a_dir = 1;
			 str_list_remove(&installed_dirs, &iter);
		    }
	       }
	  } while (removed_a_dir);
     }

     pkg_free_installed_files(pkg);
     pkg_remove_installed_files_list(conf, pkg);

     /* Don't print warning for dirs that are provided by other packages */
     for (iter = str_list_first(&installed_dirs); iter; iter = str_list_next(&installed_dirs, iter)) {
	  file_name = (char *)iter->data;

	  owner = file_hash_get_file_owner(conf, file_name);
	  if (owner) {
	       free(iter->data);
	       iter->data = NULL;
	       str_list_remove(&installed_dirs, &iter);
	  }
     }

     /* cleanup */
     while (!void_list_empty(&installed_dirs)) {
        iter = str_list_pop(&installed_dirs);
        free(iter->data);
        free(iter);
     }
     str_list_deinit(&installed_dirs);
}

void
remove_maintainer_scripts(opkg_conf_t *conf, pkg_t *pkg)
{
	int i, err;
	char *globpattern;
	glob_t globbuf;

	if (conf->noaction)
		return;

	sprintf_alloc(&globpattern, "%s/%s.*",
			pkg->dest->info_dir, pkg->name);

	err = glob(globpattern, 0, NULL, &globbuf);
	free(globpattern);
	if (err)
		return;

	for (i = 0; i < globbuf.gl_pathc; i++) {
		opkg_message(conf, OPKG_INFO, "deleting %s\n",
				globbuf.gl_pathv[i]);
		unlink(globbuf.gl_pathv[i]);
	}
	globfree(&globbuf);
}
