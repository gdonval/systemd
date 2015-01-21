/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2014 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <getopt.h>

#include "sd-event.h"
#include "event-util.h"
#include "verbs.h"
#include "build.h"
#include "machine-image.h"
#include "import-tar.h"
#include "import-raw.h"
#include "import-dkr.h"
#include "import-util.h"

static bool arg_force = false;
static const char *arg_image_root = "/var/lib/machines";
static ImportVerify arg_verify = IMPORT_VERIFY_SIGNATURE;
static const char* arg_dkr_index_url = DEFAULT_DKR_INDEX_URL;

static void on_tar_finished(TarImport *import, int error, void *userdata) {
        sd_event *event = userdata;
        assert(import);

        if (error == 0)
                log_info("Operation completed successfully.");

        sd_event_exit(event, EXIT_FAILURE);
}

static int strip_tar_suffixes(const char *name, char **ret) {
        const char *e;
        char *s;

        e = endswith(name, ".tar");
        if (!e)
                e = endswith(name, ".tar.xz");
        if (!e)
                e = endswith(name, ".tar.gz");
        if (!e)
                e = endswith(name, ".tar.bz2");
        if (!e)
                e = endswith(name, ".tgz");
        if (!e)
                e = strchr(name, 0);

        if (e <= name)
                return -EINVAL;

        s = strndup(name, e - name);
        if (!s)
                return -ENOMEM;

        *ret = s;
        return 0;
}

static int pull_tar(int argc, char *argv[], void *userdata) {
        _cleanup_(tar_import_unrefp) TarImport *import = NULL;
        _cleanup_event_unref_ sd_event *event = NULL;
        const char *url, *local;
        _cleanup_free_ char *l = NULL, *ll = NULL;
        int r;

        url = argv[1];
        if (!http_url_is_valid(url)) {
                log_error("URL '%s' is not valid.", url);
                return -EINVAL;
        }

        if (argc >= 3)
                local = argv[2];
        else {
                r = import_url_last_component(url, &l);
                if (r < 0)
                        return log_error_errno(r, "Failed get final component of URL: %m");

                local = l;
        }

        if (isempty(local) || streq(local, "-"))
                local = NULL;

        if (local) {
                r = strip_tar_suffixes(local, &ll);
                if (r < 0)
                        return log_oom();

                local = ll;

                if (!machine_name_is_valid(local)) {
                        log_error("Local image name '%s' is not valid.", local);
                        return -EINVAL;
                }

                if (!arg_force) {
                        r = image_find(local, NULL);
                        if (r < 0)
                                return log_error_errno(r, "Failed to check whether image '%s' exists: %m", local);
                        else if (r > 0) {
                                log_error_errno(EEXIST, "Image '%s' already exists.", local);
                                return -EEXIST;
                        }
                }

                log_info("Pulling '%s', saving as '%s'.", url, local);
        } else
                log_info("Pulling '%s'.", url);

        r = sd_event_default(&event);
        if (r < 0)
                return log_error_errno(r, "Failed to allocate event loop: %m");

        assert_se(sigprocmask_many(SIG_BLOCK, SIGTERM, SIGINT, -1) == 0);
        sd_event_add_signal(event, NULL, SIGTERM, NULL,  NULL);
        sd_event_add_signal(event, NULL, SIGINT, NULL, NULL);

        r = tar_import_new(&import, event, arg_image_root, on_tar_finished, event);
        if (r < 0)
                return log_error_errno(r, "Failed to allocate importer: %m");

        r = tar_import_pull(import, url, local, arg_force, arg_verify);
        if (r < 0)
                return log_error_errno(r, "Failed to pull image: %m");

        r = sd_event_loop(event);
        if (r < 0)
                return log_error_errno(r, "Failed to run event loop: %m");

        log_info("Exiting.");

        return r;
}

static void on_raw_finished(RawImport *import, int error, void *userdata) {
        sd_event *event = userdata;
        assert(import);

        if (error == 0)
                log_info("Operation completed successfully.");

        sd_event_exit(event, EXIT_FAILURE);
}

static int strip_raw_suffixes(const char *p, char **ret) {
        static const char suffixes[] =
                ".xz\0"
                ".gz\0"
                ".bz2\0"
                ".raw\0"
                ".qcow2\0"
                ".img\0"
                ".bin\0";

        _cleanup_free_ char *q = NULL;

        q = strdup(p);
        if (!q)
                return -ENOMEM;

        for (;;) {
                const char *sfx;
                bool changed = false;

                NULSTR_FOREACH(sfx, suffixes) {
                        char *e;

                        e = endswith(q, sfx);
                        if (e) {
                                *e = 0;
                                changed = true;
                        }
                }

                if (!changed)
                        break;
        }

        *ret = q;
        q = NULL;

        return 0;
}

static int pull_raw(int argc, char *argv[], void *userdata) {
        _cleanup_(raw_import_unrefp) RawImport *import = NULL;
        _cleanup_event_unref_ sd_event *event = NULL;
        const char *url, *local;
        _cleanup_free_ char *l = NULL, *ll = NULL;
        int r;

        url = argv[1];
        if (!http_url_is_valid(url)) {
                log_error("URL '%s' is not valid.", url);
                return -EINVAL;
        }

        if (argc >= 3)
                local = argv[2];
        else {
                r = import_url_last_component(url, &l);
                if (r < 0)
                        return log_error_errno(r, "Failed get final component of URL: %m");

                local = l;
        }

        if (isempty(local) || streq(local, "-"))
                local = NULL;

        if (local) {
                r = strip_raw_suffixes(local, &ll);
                if (r < 0)
                        return log_oom();

                local = ll;

                if (!machine_name_is_valid(local)) {
                        log_error("Local image name '%s' is not valid.", local);
                        return -EINVAL;
                }

                if (!arg_force) {
                        r = image_find(local, NULL);
                        if (r < 0)
                                return log_error_errno(r, "Failed to check whether image '%s' exists: %m", local);
                        else if (r > 0) {
                                log_error_errno(EEXIST, "Image '%s' already exists.", local);
                                return -EEXIST;
                        }
                }

                log_info("Pulling '%s', saving as '%s'.", url, local);
        } else
                log_info("Pulling '%s'.", url);

        r = sd_event_default(&event);
        if (r < 0)
                return log_error_errno(r, "Failed to allocate event loop: %m");

        assert_se(sigprocmask_many(SIG_BLOCK, SIGTERM, SIGINT, -1) == 0);
        sd_event_add_signal(event, NULL, SIGTERM, NULL,  NULL);
        sd_event_add_signal(event, NULL, SIGINT, NULL, NULL);

        r = raw_import_new(&import, event, arg_image_root, on_raw_finished, event);
        if (r < 0)
                return log_error_errno(r, "Failed to allocate importer: %m");

        r = raw_import_pull(import, url, local, arg_force, arg_verify);
        if (r < 0)
                return log_error_errno(r, "Failed to pull image: %m");

        r = sd_event_loop(event);
        if (r < 0)
                return log_error_errno(r, "Failed to run event loop: %m");

        log_info("Exiting.");

        return r;
}

static void on_dkr_finished(DkrImport *import, int error, void *userdata) {
        sd_event *event = userdata;
        assert(import);

        if (error == 0)
                log_info("Operation completed successfully.");

        sd_event_exit(event, EXIT_FAILURE);
}

static int pull_dkr(int argc, char *argv[], void *userdata) {
        _cleanup_(dkr_import_unrefp) DkrImport *import = NULL;
        _cleanup_event_unref_ sd_event *event = NULL;
        const char *name, *tag, *local;
        int r;

        if (!arg_dkr_index_url) {
                log_error("Please specify an index URL with --dkr-index-url=");
                return -EINVAL;
        }

        if (arg_verify != IMPORT_VERIFY_NO) {
                log_error("Imports from dkr do not support image verification, please pass --verify=no.");
                return -EINVAL;
        }

        tag = strchr(argv[1], ':');
        if (tag) {
                name = strndupa(argv[1], tag - argv[1]);
                tag++;
        } else {
                name = argv[1];
                tag = "latest";
        }

        if (!dkr_name_is_valid(name)) {
                log_error("Remote name '%s' is not valid.", name);
                return -EINVAL;
        }

        if (!dkr_tag_is_valid(tag)) {
                log_error("Tag name '%s' is not valid.", tag);
                return -EINVAL;
        }

        if (argc >= 3)
                local = argv[2];
        else {
                local = strchr(name, '/');
                if (local)
                        local++;
                else
                        local = name;
        }

        if (isempty(local) || streq(local, "-"))
                local = NULL;

        if (local) {
                if (!machine_name_is_valid(local)) {
                        log_error("Local image name '%s' is not valid.", local);
                        return -EINVAL;
                }

                if (!arg_force) {
                        r = image_find(local, NULL);
                        if (r < 0)
                                return log_error_errno(r, "Failed to check whether image '%s' exists: %m", local);
                        else if (r > 0) {
                                log_error_errno(EEXIST, "Image '%s' already exists.", local);
                                return -EEXIST;
                        }
                }

                log_info("Pulling '%s' with tag '%s', saving as '%s'.", name, tag, local);
        } else
                log_info("Pulling '%s' with tag '%s'.", name, tag);

        r = sd_event_default(&event);
        if (r < 0)
                return log_error_errno(r, "Failed to allocate event loop: %m");

        assert_se(sigprocmask_many(SIG_BLOCK, SIGTERM, SIGINT, -1) == 0);
        sd_event_add_signal(event, NULL, SIGTERM, NULL,  NULL);
        sd_event_add_signal(event, NULL, SIGINT, NULL, NULL);

        r = dkr_import_new(&import, event, arg_dkr_index_url, arg_image_root, on_dkr_finished, event);
        if (r < 0)
                return log_error_errno(r, "Failed to allocate importer: %m");

        r = dkr_import_pull(import, name, tag, local, arg_force);
        if (r < 0)
                return log_error_errno(r, "Failed to pull image: %m");

        r = sd_event_loop(event);
        if (r < 0)
                return log_error_errno(r, "Failed to run event loop: %m");

        log_info("Exiting.");

        return 0;
}

static int help(int argc, char *argv[], void *userdata) {

        printf("%s [OPTIONS...] {COMMAND} ...\n\n"
               "Import container or virtual machine image.\n\n"
               "  -h --help                   Show this help\n"
               "     --version                Show package version\n"
               "     --force                  Force creation of image\n"
               "     --verify=                Verify downloaded image, one of: 'no', 'sum'\n"
               "                              'signature'.\n"
               "     --image-root=            Image root directory\n"
               "     --dkr-index-url=URL      Specify index URL to use for downloads\n\n"
               "Commands:\n"
               "  pull-tar URL [NAME]         Download a TAR image\n"
               "  pull-raw URL [NAME]         Download a RAW image\n"
               "  pull-dkr REMOTE [NAME]      Download a DKR image\n",
               program_invocation_short_name);

        return 0;
}

static int parse_argv(int argc, char *argv[]) {

        enum {
                ARG_VERSION = 0x100,
                ARG_FORCE,
                ARG_DKR_INDEX_URL,
                ARG_IMAGE_ROOT,
                ARG_VERIFY,
        };

        static const struct option options[] = {
                { "help",            no_argument,       NULL, 'h'                 },
                { "version",         no_argument,       NULL, ARG_VERSION         },
                { "force",           no_argument,       NULL, ARG_FORCE           },
                { "dkr-index-url",   required_argument, NULL, ARG_DKR_INDEX_URL   },
                { "image-root",      required_argument, NULL, ARG_IMAGE_ROOT      },
                { "verify",          required_argument, NULL, ARG_VERIFY          },
                {}
        };

        int c;

        assert(argc >= 0);
        assert(argv);

        while ((c = getopt_long(argc, argv, "h", options, NULL)) >= 0)

                switch (c) {

                case 'h':
                        return help(0, NULL, NULL);

                case ARG_VERSION:
                        puts(PACKAGE_STRING);
                        puts(SYSTEMD_FEATURES);
                        return 0;

                case ARG_FORCE:
                        arg_force = true;
                        break;

                case ARG_DKR_INDEX_URL:
                        if (!http_url_is_valid(optarg)) {
                                log_error("Index URL is not valid: %s", optarg);
                                return -EINVAL;
                        }

                        arg_dkr_index_url = optarg;
                        break;

                case ARG_IMAGE_ROOT:
                        arg_image_root = optarg;
                        break;

                case ARG_VERIFY:
                        arg_verify = import_verify_from_string(optarg);
                        if (arg_verify < 0) {
                                log_error("Invalid verification setting '%s'", optarg);
                                return -EINVAL;
                        }

                        break;

                case '?':
                        return -EINVAL;

                default:
                        assert_not_reached("Unhandled option");
                }

        return 1;
}

static int import_main(int argc, char *argv[]) {

        static const Verb verbs[] = {
                { "help",     VERB_ANY, VERB_ANY, 0, help     },
                { "pull-tar", 2,        3,        0, pull_tar },
                { "pull-raw", 2,        3,        0, pull_raw },
                { "pull-dkr", 2,        3,        0, pull_dkr },
                {}
        };

        return dispatch_verb(argc, argv, verbs, NULL);
}

int main(int argc, char *argv[]) {
        int r;

        setlocale(LC_ALL, "");
        log_parse_environment();
        log_open();

        r = parse_argv(argc, argv);
        if (r <= 0)
                goto finish;

        r = import_main(argc, argv);

finish:
        return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
