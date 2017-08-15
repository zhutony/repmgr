/*
 * repmgr-client.c - Command interpreter for the repmgr package
 *
 * Copyright (c) 2ndQuadrant, 2010-2017
 *
 * This module is a command-line utility to easily setup a cluster of
 * hot standby servers for an HA environment
 *
 * Commands implemented are:
 *
 * [ PRIMARY | MASTER ] REGISTER
 * [ PRIMARY | MASTER ] UNREGISTER
 *
 * STANDBY CLONE
 * STANDBY REGISTER
 * STANDBY UNREGISTER
 * STANDBY PROMOTE
 * STANDBY FOLLOW
 * STANDBY SWITCHOVER
 *
 * BDR REGISTER
 * BDR UNREGISTER
 *
 * CLUSTER SHOW
 * CLUSTER EVENT
 * CLUSTER CROSSCHECK
 * CLUSTER MATRIX
 *
 * NODE STATUS
 * NODE CHECK
 *
 * For internal use:
 * NODE REJOIN
 * NODE SERVICE
 */

#include <unistd.h>
#include <sys/stat.h>

#include "repmgr.h"
#include "compat.h"
#include "repmgr-client.h"
#include "repmgr-client-global.h"
#include "repmgr-action-primary.h"
#include "repmgr-action-standby.h"
#include "repmgr-action-bdr.h"
#include "repmgr-action-node.h"

#include "repmgr-action-cluster.h"

#include <storage/fd.h>         /* for PG_TEMP_FILE_PREFIX */


/* globally available variables *
 * ============================ */

t_runtime_options runtime_options = T_RUNTIME_OPTIONS_INITIALIZER;
t_configuration_options config_file_options = T_CONFIGURATION_OPTIONS_INITIALIZER;

/* conninfo params for the node we're operating on */
t_conninfo_param_list source_conninfo;

bool	 config_file_required = true;
char	 pg_bindir[MAXLEN] = "";


char  path_buf[MAXLEN] = "";

/*
 * if --node-id/--node-name provided, place that node's record here
 * for later use
 */
t_node_info target_node_info = T_NODE_INFO_INITIALIZER;


/* Collate command line errors and warnings here for friendlier reporting */
static ItemList	cli_errors = { NULL, NULL };
static ItemList	cli_warnings = { NULL, NULL };


int
main(int argc, char **argv)
{
	int			optindex;
	int			c;

	char	   *repmgr_node_type = NULL;
	char	   *repmgr_action = NULL;
	bool		valid_repmgr_node_type_found = true;
	int			action = NO_ACTION;
	char	   *dummy_action = "";

	set_progname(argv[0]);

	/*
	 * Tell the logger we're a command-line program - this will
	 * ensure any output logged before the logger is initialized
	 * will be formatted correctly. Can be overriden with "--log-to-file".
	 */
	logger_output_mode = OM_COMMAND_LINE;

	/*
	 * Initialize and pre-populate conninfo parameters; these will be
	 * overwritten if matching command line parameters are provided.
	 *
	 * Only some actions will need these, but we need to do this before
	 * the command line is parsed.
	 *
	 * Note: PQconndefaults() does not provide a default value for
	 * "dbname", but if none is provided will default to "username"
	 * when the connection is made.
	 */

	initialize_conninfo_params(&source_conninfo, false);

	for (c = 0; c < source_conninfo.size && source_conninfo.keywords[c]; c++)
	{
		if (strcmp(source_conninfo.keywords[c], "host") == 0 &&
			(source_conninfo.values[c] != NULL))
		{
			strncpy(runtime_options.host, source_conninfo.values[c], MAXLEN);
		}
		else if (strcmp(source_conninfo.keywords[c], "hostaddr") == 0 &&
				(source_conninfo.values[c] != NULL))
		{
			strncpy(runtime_options.host, source_conninfo.values[c], MAXLEN);
		}
		else if (strcmp(source_conninfo.keywords[c], "port") == 0 &&
				 (source_conninfo.values[c] != NULL))
		{
			strncpy(runtime_options.port, source_conninfo.values[c], MAXLEN);
		}
		else if (strcmp(source_conninfo.keywords[c], "dbname") == 0 &&
				 (source_conninfo.values[c] != NULL))
		{
			strncpy(runtime_options.dbname, source_conninfo.values[c], MAXLEN);
		}
		else if (strcmp(source_conninfo.keywords[c], "user") == 0 &&
				 (source_conninfo.values[c] != NULL))
		{
			strncpy(runtime_options.username, source_conninfo.values[c], MAXLEN);
		}
	}


	/* set default user for -R/--remote-user */
	{
		struct passwd *pw = NULL;

		pw = getpwuid(geteuid());
		if (pw == NULL)
		{
			fprintf(stderr, _("could not get current user name: %s\n"), strerror(errno));
			exit(ERR_BAD_CONFIG);
		}

		strncpy(runtime_options.username, pw->pw_name, MAXLEN);
	}

	while ((c = getopt_long(argc, argv, "?Vb:f:Fd:h:p:U:R:S:L:vtD:crC:", long_options,
							&optindex)) != -1)
	{
		/*
		 * NOTE: some integer parameters (e.g. -p/--port) are stored internally
		 * as strings. We use repmgr_atoi() to check these but discard the
		 * returned integer; repmgr_atoi() will append the error message to the
		 * provided list.
		 */
		switch (c)
		{
			/*
			 * Options which cause repmgr to exit in this block;
			 * these are the only ones which can be executed as root user
			 */
			case OPT_HELP: /* --help */
				do_help();
				exit(SUCCESS);
			case '?':
				/* Actual help option given */
				if (strcmp(argv[optind - 1], "-?") == 0)
				{
					do_help();
					exit(SUCCESS);
				}
				break;
			case 'V':
				/*
				 * in contrast to repmgr3 and earlier, we only display the repmgr version
				 * as it's not specific to a particular PostgreSQL version
				 */
				printf("%s %s\n", progname(), REPMGR_VERSION);
				exit(SUCCESS);

			/* general configuration options
			 * ----------------------------- */

			/* -b/--pg_bindir */
			case 'b':
				strncpy(runtime_options.pg_bindir, optarg, MAXLEN);
				break;

			/* -f/--config-file */
			case 'f':
				strncpy(runtime_options.config_file, optarg, MAXLEN);
				break;

			/* --dry-run */
			case OPT_DRY_RUN:
				runtime_options.dry_run = true;
				break;

			/* -F/--force */
			case 'F':
				runtime_options.force = true;
				break;

			/* --replication-user (primary/standby register only) */
			case OPT_REPLICATION_USER:
				strncpy(runtime_options.replication_user, optarg, MAXLEN);
				break;

			/* -W/--wait */
			case 'W':
				runtime_options.wait = true;
				break;

			/* database connection options */
			/* --------------------------- */

			/*
			 * These are the standard database connection options; with the
			 * exception of -d/--dbname (which could be a conninfo string)
			 * we'll also set these values in "source_conninfo" (overwriting
			 * preset values from environment variables).
			 * XXX check this is same as psql
			 */
			/* -d/--dbname */
			case 'd':
				strncpy(runtime_options.dbname, optarg, MAXLEN);
				/* dbname will be set in source_conninfo later after checking if it's a conninfo string */
				runtime_options.connection_param_provided = true;
				break;

			/* -h/--host */
			case 'h':
				strncpy(runtime_options.host, optarg, MAXLEN);
				param_set(&source_conninfo, "host", optarg);
				runtime_options.connection_param_provided = true;
				runtime_options.host_param_provided = true;
				break;

			case 'p':
				(void) repmgr_atoi(optarg, "-p/--port", &cli_errors, false);
				param_set(&source_conninfo, "port", optarg);
				strncpy(runtime_options.port,
						optarg,
						MAXLEN);
				runtime_options.connection_param_provided = true;
				break;

			/* -U/--user */
			case 'U':
				strncpy(runtime_options.username, optarg, MAXLEN);
				param_set(&source_conninfo, "user", optarg);
				runtime_options.connection_param_provided = true;
				break;

			/* other connection options */
			/* ------------------------ */

			/* -R/--remote_user */
			case 'R':
				strncpy(runtime_options.remote_user, optarg, MAXLEN);
				break;

			/* -S/--superuser */
			case 'S':
				strncpy(runtime_options.superuser, optarg, MAXLEN);
				break;

			/* node options *
			 * ------------ */

			/* -D/--pgdata/--data-dir */
			case 'D':
				strncpy(runtime_options.data_dir, optarg, MAXPGPATH);
				break;

			/* --node-id */
			case OPT_NODE_ID:
				runtime_options.node_id = repmgr_atoi(optarg, "--node-id", &cli_errors, false);
				break;

			/* --node-name */
			case OPT_NODE_NAME:
				strncpy(runtime_options.node_name, optarg, MAXLEN);
				break;

			/* standby options *
			 * --------------- */

			/* --upstream-node-id */
			case OPT_UPSTREAM_NODE_ID:
				runtime_options.upstream_node_id = repmgr_atoi(optarg, "--upstream-node-id", &cli_errors, false);
				break;

			/* "standby clone" options *
			 * ----------------------- */

			/* -c/--fast-checkpoint */
			case 'c':
				runtime_options.fast_checkpoint = true;
				break;

			/* --copy-external-config-files(=[samepath|pgdata]) */
			case OPT_COPY_EXTERNAL_CONFIG_FILES:
				runtime_options.copy_external_config_files = true;
				if (optarg != NULL)
				{
					if (strcmp(optarg, "samepath") == 0)
					{
						runtime_options.copy_external_config_files_destination = CONFIG_FILE_SAMEPATH;
					}
					/* allow "data_directory" as synonym for "pgdata" */
					else if (strcmp(optarg, "pgdata") == 0 || strcmp(optarg, "data_directory") == 0)
					{
						runtime_options.copy_external_config_files_destination = CONFIG_FILE_PGDATA;
					}
					else
					{
						item_list_append(&cli_errors,
										 _("value provided for \"--copy-external-config-files\" must be \"samepath\" or \"pgdata\""));
					}
				}
				break;

			/* -w/--wal-keep-segments */
			case 'w':
				(void) repmgr_atoi(optarg, "-w/--wal-keep-segments", &cli_errors, false);
				strncpy(runtime_options.wal_keep_segments,
						optarg,
						MAXLEN);
				runtime_options.wal_keep_segments_used = true;
				break;

			/* --no-upstream-connection */
			case OPT_NO_UPSTREAM_CONNECTION:
				runtime_options.no_upstream_connection = true;
				break;

			/* --recovery-min-apply-delay */
			case OPT_RECOVERY_MIN_APPLY_DELAY:
			{
				char 	   *ptr = NULL;
				int targ = strtol(optarg, &ptr, 10);

				if (targ < 1)
				{
					item_list_append(&cli_errors, _("invalid value provided for '--recovery-min-apply-delay'"));
					break;
				}
				if (ptr && *ptr)
				{
					if (strcmp(ptr, "ms") != 0 && strcmp(ptr, "s") != 0 &&
					   strcmp(ptr, "min") != 0 && strcmp(ptr, "h") != 0 &&
					   strcmp(ptr, "d") != 0)
					{
						item_list_append(&cli_errors,
										 _("value provided for '--recovery-min-apply-delay' must be one of ms/s/min/h/d"));
						break;
					}
				}

				strncpy(runtime_options.recovery_min_apply_delay, optarg, MAXLEN);
				break;
			}

			case OPT_UPSTREAM_CONNINFO:
				strncpy(runtime_options.upstream_conninfo, optarg, MAXLEN);
				break;

			case OPT_USE_RECOVERY_CONNINFO_PASSWORD:
				runtime_options.use_recovery_conninfo_password = true;
				break;

			case OPT_WITHOUT_BARMAN:
				runtime_options.without_barman = true;
				break;

			/* "standby register" options *
			 * -------------------------- */

			case OPT_REGISTER_WAIT:
				runtime_options.wait_register_sync = true;
				if (optarg != NULL)
				{
					runtime_options.wait_register_sync_seconds = repmgr_atoi(optarg, "--wait-sync", &cli_errors, false);
				}
				break;

			/* "standby switchover" options *
			 * ---------------------------- */

			case OPT_ALWAYS_PROMOTE:
				runtime_options.always_promote = true;
				break;

			case OPT_FORCE_REWIND:
				runtime_options.force_rewind = true;
				break;

			case OPT_SIBLINGS_FOLLOW:
				runtime_options.siblings_follow = true;
				break;

			/* "node status" options *
			 * --------------------- */

			case OPT_IS_SHUTDOWN:
				runtime_options.is_shutdown = true;
				break;

			/* "node check" options *
			 * --------------------- */
			case OPT_ARCHIVER:
				runtime_options.archiver = true;
				break;

			case OPT_DOWNSTREAM:
				runtime_options.downstream = true;
				break;

			case OPT_REPLICATION_LAG:
				runtime_options.replication_lag = true;
				break;

			case OPT_ROLE:
				runtime_options.role = true;
				break;

			/* "node join" options *
			 * ------------------- */
			case OPT_CONFIG_FILES:
				strncpy(runtime_options.config_files, optarg, MAXLEN);
				break;

			/* "node service" options *
			 * ---------------------- */

			/* --action (repmgr node service --action) */
			case OPT_ACTION:
				strncpy(runtime_options.action, optarg, MAXLEN);
				break;

			case OPT_LIST_ACTIONS:
				runtime_options.list_actions = true;
				break;

			case OPT_CHECK:
				runtime_options.check = true;
				break;

			case OPT_CHECKPOINT:
				runtime_options.checkpoint = true;
				break;

			/* "cluster event" options *
			 * ----------------------- */

			case OPT_EVENT:
				strncpy(runtime_options.event, optarg, MAXLEN);
				break;

			case OPT_LIMIT:
				runtime_options.limit = repmgr_atoi(optarg, "--limit", &cli_errors, false);
				runtime_options.limit_provided = true;
				break;

			case OPT_ALL:
				runtime_options.all = true;
				break;



			/* logging options *
			 * --------------- */

			/* -L/--log-level */
			case 'L':
			{
				int detected_log_level = detect_log_level(optarg);
				if (detected_log_level != -1)
				{
					strncpy(runtime_options.log_level, optarg, MAXLEN);
				}
				else
				{
					PQExpBufferData invalid_log_level;
					initPQExpBuffer(&invalid_log_level);
					appendPQExpBuffer(&invalid_log_level, _("invalid log level \"%s\" provided"), optarg);
					item_list_append(&cli_errors, invalid_log_level.data);
					termPQExpBuffer(&invalid_log_level);
				}
				break;
			}

			/* --log-to-file */
			case OPT_LOG_TO_FILE:
				runtime_options.log_to_file = true;
				logger_output_mode = OM_DAEMON;
				break;

			/* --terse */
			case 't':
				runtime_options.terse = true;
				break;

			/* --verbose */
			case 'v':
				runtime_options.verbose = true;
				break;


			/* output options */
			/* -------------- */
			case OPT_CSV:
				runtime_options.csv = true;
				break;

			case OPT_NAGIOS:
				runtime_options.nagios = true;
				break;

			case OPT_OPTFORMAT:
				runtime_options.optformat = true;
				break;

			/* internal options */
			case OPT_CONFIG_ARCHIVE_DIR:
				/* TODO: check this is an absolute path */
				strncpy(runtime_options.config_archive_dir, optarg, MAXPGPATH);
				break;

			/* options deprecated since 3.3 *
			 * ---------------------------- */
			case OPT_DATA_DIR:
				item_list_append(&cli_warnings,
								 _("--data-dir is deprecated; use -D/--pgdata instead"));
				break;
			case OPT_NO_CONNINFO_PASSWORD:
				item_list_append(&cli_warnings,
								 _("--no-conninfo-password is deprecated; pasuse --use-recovery-conninfo-password to explicitly set a password"));
				break;
			/* -C/--remote-config-file */
			case 'C':
				item_list_append(&cli_warnings,
								 _("--remote-config-file is no longer required"));
				break;


		}
	}

	/*
	 * If -d/--dbname appears to be a conninfo string, validate by attempting
	 * to parse it (and if successful, store the parsed parameters)
	 */
	if (runtime_options.dbname)
	{
		if (strncmp(runtime_options.dbname, "postgresql://", 13) == 0 ||
		   strncmp(runtime_options.dbname, "postgres://", 11) == 0 ||
		   strchr(runtime_options.dbname, '=') != NULL)
		{
			char	   *errmsg = NULL;
			PQconninfoOption *opts;

			runtime_options.conninfo_provided = true;

			opts = PQconninfoParse(runtime_options.dbname, &errmsg);

			if (opts == NULL)
			{
				PQExpBufferData conninfo_error;
				initPQExpBuffer(&conninfo_error);
				appendPQExpBuffer(&conninfo_error, _("error parsing conninfo:\n%s"), errmsg);
				item_list_append(&cli_errors, conninfo_error.data);

				termPQExpBuffer(&conninfo_error);
				pfree(errmsg);
			}
			else
			{
				/*
				 * Store any parameters provided in the conninfo string in our
				 * internal array; also overwrite any options set in
				 * runtime_options.(host|port|username), as the conninfo
				 * settings take priority
				 */
				PQconninfoOption *opt;
				for (opt = opts; opt->keyword != NULL; opt++)
				{
					if (opt->val != NULL && opt->val[0] != '\0')
					{
						param_set(&source_conninfo, opt->keyword, opt->val);
					}

					if (strcmp(opt->keyword, "host") == 0 &&
						(opt->val != NULL && opt->val[0] != '\0'))
					{
						strncpy(runtime_options.host, opt->val, MAXLEN);
						runtime_options.host_param_provided = true;
					}
					if (strcmp(opt->keyword, "hostaddr") == 0 &&
						(opt->val != NULL && opt->val[0] != '\0'))
					{
						strncpy(runtime_options.host, opt->val, MAXLEN);
						runtime_options.host_param_provided = true;
					}
					else if (strcmp(opt->keyword, "port") == 0 &&
							 (opt->val != NULL && opt->val[0] != '\0'))
					{
						strncpy(runtime_options.port, opt->val, MAXLEN);
					}
					else if (strcmp(opt->keyword, "user") == 0 &&
							 (opt->val != NULL && opt->val[0] != '\0'))
					{
						strncpy(runtime_options.username, opt->val, MAXLEN);
					}
				}

				PQconninfoFree(opts);
			}
		}
		else
		{
   			param_set(&source_conninfo, "dbname", runtime_options.dbname);
		}
	}

	/*
	 * Disallow further running as root to prevent directory ownership problems.
	 * We check this here to give the root user a chance to execute --help/--version
	 * options.
	 */
	if (geteuid() == 0)
	{
		fprintf(stderr,
				_("%s: cannot be run as root\n"
				  "Please log in (using, e.g., \"su\") as the "
				  "(unprivileged) user that owns "
				  "the data directory.\n"
				),
				progname());
		free_conninfo_params(&source_conninfo);
		exit(ERR_BAD_CONFIG);
	}

	/* Exit here already if errors in command line options found */
	if (cli_errors.head != NULL)
	{
		free_conninfo_params(&source_conninfo);
		exit_with_cli_errors(&cli_errors);
	}

	/*
	 * Determine the node type and action; following are valid:
	 *
	 *	 { PRIMARY | MASTER } REGISTER |
	 *	 STANDBY {REGISTER | UNREGISTER | CLONE [node] | PROMOTE | FOLLOW [node] | SWITCHOVER | REWIND} |
	 *	 BDR { REGISTER | UNREGISTER } |
	 *   NODE { STATUS | CHECK | REJOIN | ARCHIVE-CONFIG | RESTORE-CONFIG | SERVICE } |
	 *	 CLUSTER { CROSSCHECK | MATRIX | SHOW | CLEANUP | EVENT }
	 *
	 * [node] is an optional hostname, provided instead of the -h/--host option
	 */
	if (optind < argc)
	{
		repmgr_node_type = argv[optind++];
	}

	if (optind < argc)
	{
		repmgr_action = argv[optind++];
	}
	else
	{
		repmgr_action = dummy_action;
	}

	if (repmgr_node_type != NULL)
	{
#ifndef BDR_ONLY
		if (strcasecmp(repmgr_node_type, "PRIMARY") == 0 || strcasecmp(repmgr_node_type, "MASTER") == 0 )
		{
			if (strcasecmp(repmgr_action, "REGISTER") == 0)
				action = PRIMARY_REGISTER;
			else if (strcasecmp(repmgr_action, "UNREGISTER") == 0)
				action = PRIMARY_UNREGISTER;
			else if (strcasecmp(repmgr_action, "CHECK") == 0)
				action = NODE_CHECK;
			else if (strcasecmp(repmgr_action, "STATUS") == 0)
				action = NODE_STATUS;
		}

		else if (strcasecmp(repmgr_node_type, "STANDBY") == 0)
		{
			if (strcasecmp(repmgr_action, "CLONE") == 0)
				action = STANDBY_CLONE;
			else if (strcasecmp(repmgr_action, "REGISTER") == 0)
				action = STANDBY_REGISTER;
			else if (strcasecmp(repmgr_action, "UNREGISTER") == 0)
				action = STANDBY_UNREGISTER;
			else if (strcasecmp(repmgr_action, "PROMOTE") == 0)
				action = STANDBY_PROMOTE;
			else if (strcasecmp(repmgr_action, "FOLLOW") == 0)
				action = STANDBY_FOLLOW;
			else if (strcasecmp(repmgr_action, "SWITCHOVER") == 0)
				action = STANDBY_SWITCHOVER;
			else if (strcasecmp(repmgr_action, "CHECK") == 0)
				action = NODE_CHECK;
			else if (strcasecmp(repmgr_action, "STATUS") == 0)
				action = NODE_STATUS;
		}
		else if (strcasecmp(repmgr_node_type, "BDR") == 0)
#else
		if (strcasecmp(repmgr_node_type, "BDR") == 0)
#endif
		{
			if (strcasecmp(repmgr_action, "REGISTER") == 0)
				action = BDR_REGISTER;
			else if (strcasecmp(repmgr_action, "UNREGISTER") == 0)
				action = BDR_UNREGISTER;
			else if (strcasecmp(repmgr_action, "CHECK") == 0)
				action = NODE_CHECK;
			else if (strcasecmp(repmgr_action, "STATUS") == 0)
				action = NODE_STATUS;
		}

		else if (strcasecmp(repmgr_node_type, "NODE") == 0)
		{
		    if (strcasecmp(repmgr_action, "CHECK") == 0)
				action = NODE_CHECK;
			else if (strcasecmp(repmgr_action, "STATUS") == 0)
				action = NODE_STATUS;
			else if (strcasecmp(repmgr_action, "REJOIN") == 0)
				action = NODE_REJOIN;
			else if (strcasecmp(repmgr_action, "SERVICE") == 0)
				action = NODE_SERVICE;
		}

		else if (strcasecmp(repmgr_node_type, "CLUSTER") == 0)
		{

			if (strcasecmp(repmgr_action, "SHOW") == 0)
				action = CLUSTER_SHOW;
			else if (strcasecmp(repmgr_action, "EVENT") == 0)
				action = CLUSTER_EVENT;
			/* allow "CLUSTER EVENTS" as synonym for "CLUSTER EVENT" */
			else if (strcasecmp(repmgr_action, "EVENTS") == 0)
				action = CLUSTER_EVENT;
			else if (strcasecmp(repmgr_action, "CROSSCHECK") == 0)
				action = CLUSTER_CROSSCHECK;
			else if (strcasecmp(repmgr_action, "MATRIX") == 0)
				action = CLUSTER_MATRIX;
		}
		else
		{
			valid_repmgr_node_type_found = false;
		}
	}

	if (action == NO_ACTION)
	{
		PQExpBufferData command_error;
		initPQExpBuffer(&command_error);

		if (repmgr_node_type == NULL)
		{
			appendPQExpBuffer(&command_error,
							  _("no repmgr command provided"));
		}
		else if (valid_repmgr_node_type_found == false && repmgr_action[0] == '\0')
		{
			appendPQExpBuffer(&command_error,
							  _("unknown repmgr node type '%s'"),
							  repmgr_node_type);
		}
		else if (repmgr_action[0] == '\0')
		{
		   appendPQExpBuffer(&command_error,
							 _("no action provided for node type '%s'"),
							 repmgr_node_type);
		}
		else
		{
			appendPQExpBuffer(&command_error,
							  _("unknown repmgr action '%s %s'"),
							  repmgr_node_type,
							  repmgr_action);
		}

		item_list_append(&cli_errors, command_error.data);
	}

	/* STANDBY CLONE historically accepts the upstream hostname as an additional argument */
	if (action == STANDBY_CLONE)
	{
		if (optind < argc)
		{
			if (runtime_options.host_param_provided == true)
			{
				PQExpBufferData additional_host_arg;
				initPQExpBuffer(&additional_host_arg);
				appendPQExpBuffer(&additional_host_arg,
								  _("host name provided both with %s and as an extra parameter"),
								  runtime_options.conninfo_provided == true ? "host=" : "-h/--host");
				item_list_append(&cli_errors, additional_host_arg.data);
			}
			else
			{
				strncpy(runtime_options.host, argv[optind++], MAXLEN);
				param_set(&source_conninfo, "host", runtime_options.host);
				runtime_options.host_param_provided = true;
			}
		}
	}

	if (optind < argc)
	{
		PQExpBufferData too_many_args;
		initPQExpBuffer(&too_many_args);
		appendPQExpBuffer(&too_many_args, _("too many command-line arguments (first extra is \"%s\")"), argv[optind]);
		item_list_append(&cli_errors, too_many_args.data);
	}

	check_cli_parameters(action);

	/*
	 * Sanity checks for command line parameters completed by now;
	 * any further errors will be runtime ones
	 */
	if (cli_errors.head != NULL)
	{
		free_conninfo_params(&source_conninfo);
		exit_with_cli_errors(&cli_errors);
	}

	/*
	 * Print any warnings about inappropriate command line options,
	 * unless -t/--terse set
	 */
	if (cli_warnings.head != NULL && runtime_options.terse == false)
	{
		log_warning(_("following problems with command line parameters detected:"));
		print_item_list(&cli_warnings);
	}

	/* post-processing following command line parameter checks
	 * ======================================================= */

	if (runtime_options.csv == true)
	{
		runtime_options.output_mode = OM_CSV;
	}
	else if (runtime_options.nagios == true)
	{
		runtime_options.output_mode = OM_NAGIOS;
	}
	else if (runtime_options.optformat == true)
	{
		runtime_options.output_mode = OM_OPTFORMAT;
	}

	/*
	 * The configuration file is not required for some actions (e.g. 'standby clone'),
	 * however if available we'll parse it anyway for options like 'log_level',
	 * 'use_replication_slots' etc.
	 */
	load_config(runtime_options.config_file,
				runtime_options.verbose,
				runtime_options.terse,
				&config_file_options,
				argv[0]);


	/* Some configuration file items can be overriden by command line options */
	/* Command-line parameter -L/--log-level overrides any setting in config file*/
	if (*runtime_options.log_level != '\0')
	{
		strncpy(config_file_options.log_level, runtime_options.log_level, MAXLEN);
	}

	/*
	 * Initialise pg_bindir - command line parameter will override
	 * any setting in the configuration file
	 */
	if (!strlen(runtime_options.pg_bindir))
	{
		strncpy(runtime_options.pg_bindir, config_file_options.pg_bindir, MAXLEN);
	}

	/* Add trailing slash */
	if (strlen(runtime_options.pg_bindir))
	{
		int len = strlen(runtime_options.pg_bindir);
		if (runtime_options.pg_bindir[len - 1] != '/')
		{
			maxlen_snprintf(pg_bindir, "%s/", runtime_options.pg_bindir);
		}
		else
		{
			strncpy(pg_bindir, runtime_options.pg_bindir, MAXLEN);
		}
	}

	/*
	 * Initialize the logger. We've previously requested STDERR logging only
	 * to ensure the repmgr command doesn't have its output diverted to a logging
	 * facility (which usually doesn't make sense for a command line program).
	 *
	 * If required (e.g. when calling repmgr from repmgrd), this behaviour can be
	 * overridden with "--log-to-file".
	 */

	logger_init(&config_file_options, progname());

	if (runtime_options.verbose)
		logger_set_verbose();

	if (runtime_options.terse)
		logger_set_terse();

	/*
	 * Node configuration information is not needed for all actions, with
	 * STANDBY CLONE being the main exception.
	 */
	if (config_file_required)
	{
		/*
		 * if a configuration file was provided, the configuration file parser
		 * will already have errored out if no valid node_id found
		 */
		if (config_file_options.node_id == NODE_NOT_FOUND)
		{
			free_conninfo_params(&source_conninfo);

			log_error(_("no node information was found - please supply a configuration file"));
			exit(ERR_BAD_CONFIG);
		}
	}

	/*
	 * If a node was specified (by --node-id or --node-name), check it exists
	 * (and pre-populate a record for later use).
	 *
	 * At this point check_cli_parameters() will already have determined
	 * if provision of these is valid for the action, otherwise it unsets them.
	 *
	 * We need to check this much later than other command line parameters
	 * as we need to wait until the configuration file is parsed and we can
	 * obtain the conninfo string.
	 */

	if (runtime_options.node_id != UNKNOWN_NODE_ID || runtime_options.node_name[0] != '\0')
	{
		PGconn *conn = NULL;
		RecordStatus record_status = RECORD_NOT_FOUND;

		log_verbose(LOG_DEBUG, "connecting to local node to retrieve record for node specified with --node-id or --node-name");

		if (strlen(config_file_options.conninfo))
			conn = establish_db_connection(config_file_options.conninfo, true);
		else
			conn = establish_db_connection_by_params(&source_conninfo, true);

		if (runtime_options.node_id != UNKNOWN_NODE_ID)
		{
			record_status = get_node_record(conn, runtime_options.node_id, &target_node_info);

			if (record_status != RECORD_FOUND)
			{
				log_error(_("node %i (specified with --node-id) not found"),
						  runtime_options.node_id);
				PQfinish(conn);
				free_conninfo_params(&source_conninfo);

				exit(ERR_BAD_CONFIG);
			}
		}
		else if (runtime_options.node_name[0] != '\0')
		{
			char *escaped = escape_string(conn, runtime_options.node_name);
			if (escaped == NULL)
			{
				log_error(_("unable to escape value provided for --node-name"));
				PQfinish(conn);
				free_conninfo_params(&source_conninfo);

				exit(ERR_BAD_CONFIG);
			}

			record_status = get_node_record_by_name(conn, escaped, &target_node_info);

			pfree(escaped);
			if (record_status != RECORD_FOUND)
			{
				log_error(_("node %s (specified with --node-name) not found"),
						  runtime_options.node_name);
				PQfinish(conn);
				free_conninfo_params(&source_conninfo);

				exit(ERR_BAD_CONFIG);
			}
		}

		PQfinish(conn);
	}


	switch (action)
	{
#ifndef BDR_ONLY
		/* PRIMARY */
		case PRIMARY_REGISTER:
			do_primary_register();
			break;
		case PRIMARY_UNREGISTER:
			do_primary_unregister();
			break;

		/* STANDBY */
		case STANDBY_CLONE:
			do_standby_clone();
			break;
		case STANDBY_REGISTER:
			do_standby_register();
			break;
		case STANDBY_UNREGISTER:
			do_standby_unregister();
			break;
		case STANDBY_PROMOTE:
			do_standby_promote();
			break;
		case STANDBY_FOLLOW:
			do_standby_follow();
			break;
		case STANDBY_SWITCHOVER:
			do_standby_switchover();
			break;

			break;
#else
		/* we won't ever reach here, but stop the compiler complaining */
		case PRIMARY_REGISTER:
		case PRIMARY_UNREGISTER:
		case STANDBY_CLONE:
		case STANDBY_REGISTER:
		case STANDBY_UNREGISTER:
		case STANDBY_PROMOTE:
		case STANDBY_FOLLOW:
		case STANDBY_SWITCHOVER:
			break;

#endif
		/* BDR */
		case BDR_REGISTER:
			do_bdr_register();
			break;
		case BDR_UNREGISTER:
			do_bdr_unregister();
			break;

		/* NODE */
		case NODE_STATUS:
			do_node_status();
			break;
		case NODE_CHECK:
			do_node_check();
			break;
		case NODE_REJOIN:
			do_node_rejoin();
			break;
		case NODE_SERVICE:
			do_node_service();
			break;

		/* CLUSTER */
		case CLUSTER_SHOW:
			do_cluster_show();
			break;
		case CLUSTER_EVENT:
			do_cluster_event();
			break;
		case CLUSTER_CROSSCHECK:
			do_cluster_crosscheck();
			break;
		case CLUSTER_MATRIX:
			do_cluster_matrix();
			break;

		default:
			/* An action will have been determined by this point  */
			break;
	}

	free_conninfo_params(&source_conninfo);

	return SUCCESS;
}



/*
 * Check for useless or conflicting parameters, and also whether a
 * configuration file is required.
 *
 * Messages will be added to the command line warning and error lists
 * as appropriate.
 *
 * XXX for each individual actions, check only required actions
 * for non-required actions check warn if provided
 */

static void
check_cli_parameters(const int action)
{
	/* ========================================================================
	 * check all parameters required for an action are provided, and warn
	 * about ineffective actions
	 * ========================================================================
	 */
	switch (action)
	{
		case PRIMARY_REGISTER:
			/* no required parameters */
			break;
		case STANDBY_CLONE:
		{
			standy_clone_mode mode = get_standby_clone_mode();

			config_file_required = false;

			if (mode == barman)
			{
				if (runtime_options.copy_external_config_files)
				{
					item_list_append(&cli_warnings,
									 _("--copy-external-config-files ineffective in Barman mode"));
				}

				if (runtime_options.fast_checkpoint)
				{
					item_list_append(&cli_warnings,
									 _("-c/--fast-checkpoint has no effect in Barman mode"));
				}


			}
			else
			{
				if (!runtime_options.host_param_provided)
				{
					item_list_append_format(&cli_errors,
											_("host name for the source node must be provided when executing %s"),
											action_name(action));
				}

				if (!runtime_options.connection_param_provided)
				{
					item_list_append_format(&cli_errors,
											_("database connection parameters for the source node must be provided when executing %s"),
											action_name(action));
				}

				// XXX if -D/--pgdata provided, and also config_file_options.pgdaga, warn -D/--pgdata will be ignored

				if (*runtime_options.upstream_conninfo)
				{
					if (runtime_options.use_recovery_conninfo_password == true)
					{
						item_list_append(&cli_warnings,
										 _("--use-recovery-conninfo-password ineffective when specifying --upstream-conninfo"));
					}

					if (*runtime_options.replication_user)
					{
						item_list_append(&cli_warnings,
										 _("--replication-user ineffective when specifying --upstream-conninfo"));
					}
				}

				if (runtime_options.no_upstream_connection == true)
				{
					item_list_append(&cli_warnings,
									 _("--no-upstream-connection only effective in Barman mode"));
				}
			}
		}
		break;

		case STANDBY_FOLLOW:
		{
			/*
			 * if `repmgr standby follow` executed with host params, ensure data
			 * directory was provided
			 */
		}
		break;

		case NODE_REJOIN:
			if (runtime_options.connection_param_provided == false)
			{
				item_list_append(
					&cli_errors,
					"database connection parameters for an available node must be provided when executing NODE REJOIN");
			}
			break;
		case CLUSTER_SHOW:
		case CLUSTER_MATRIX:
		case CLUSTER_CROSSCHECK:
			if (runtime_options.connection_param_provided)
				config_file_required = false;
			break;
		case CLUSTER_EVENT:
			/* no required parameters */
			break;

	}

	/* ========================================================================
	 * warn if parameters provided for an action where they're not relevant
	 * ========================================================================
	 */

	/* --host etc.*/
	if (runtime_options.connection_param_provided)
	{
		switch (action)
		{
			case STANDBY_CLONE:
			case STANDBY_FOLLOW:
			case CLUSTER_SHOW:
			case CLUSTER_MATRIX:
			case CLUSTER_CROSSCHECK:
				break;
			default:
				item_list_append_format(&cli_warnings,
										_("database connection parameters not required when executing %s"),
										action_name(action));
		}
	}

	/* -D/--pgdata */
	if (runtime_options.data_dir[0])
	{
		switch (action)
		{
			case STANDBY_CLONE:
			case STANDBY_FOLLOW:
			case NODE_SERVICE:
				break;
			default:
				item_list_append_format(&cli_warnings,
										_("-D/--pgdata not required when executing %s"),
										action_name(action));
		}
	}

	/*
	 * --node-id
	 *
	 * NOTE: overrides --node-name, if present
	 */
	if (runtime_options.node_id != UNKNOWN_NODE_ID)
	{
		switch (action)
		{
			case PRIMARY_UNREGISTER:
			case STANDBY_UNREGISTER:
			case CLUSTER_EVENT:
			case CLUSTER_MATRIX:
			case CLUSTER_CROSSCHECK:
				break;
			default:
				item_list_append_format(&cli_warnings,
										_("--node-id not required when executing %s"),
										action_name(action));
				runtime_options.node_id = UNKNOWN_NODE_ID;
		}
	}

	if (runtime_options.node_name[0])
	{
		switch (action)
		{
			case STANDBY_UNREGISTER:
			case CLUSTER_EVENT:
				if (runtime_options.node_id != UNKNOWN_NODE_ID)
				{
					item_list_append(&cli_warnings,
									 _("--node-id provided, ignoring --node-name"));
					memset(runtime_options.node_name, 0, sizeof(runtime_options.node_name));
				}
				break;
			default:
				item_list_append_format(&cli_warnings,
										_("--node-name not required when executing %s"),
										action_name(action));
				memset(runtime_options.node_name, 0, sizeof(runtime_options.node_name));
		}
	}

	if (runtime_options.upstream_node_id != UNKNOWN_NODE_ID)
	{
		switch (action)
		{
			case STANDBY_CLONE:
			case STANDBY_REGISTER:
				break;
			default:
				item_list_append_format(&cli_warnings,
										_("--upstream-node-id will be ignored when executing %s"),
										action_name(action));
		}
	}

	if (runtime_options.event[0])
	{
		switch (action)
		{
			case CLUSTER_EVENT:
				break;
			default:
				item_list_append_format(&cli_warnings,
										_("--event not required when executing %s"),
										action_name(action));
		}
	}

	if (runtime_options.replication_user[0])
	{
		switch (action)
		{
			case PRIMARY_REGISTER:
			case STANDBY_REGISTER:
				break;
			case STANDBY_CLONE:
			case STANDBY_FOLLOW:
				item_list_append_format(&cli_warnings,
										_("--replication-user ignored when executing %s)"),
										action_name(action));
			default:
				item_list_append_format(&cli_warnings,
										_("--replication-user not required when executing %s"),
										action_name(action));
		}
	}

	if (runtime_options.limit_provided)
	{
		switch (action)
		{
			case CLUSTER_EVENT:
				if (runtime_options.limit < 1)
				{
					item_list_append_format(&cli_errors,
											_("value for --limit must be 1 or greater (provided: %i)"),
											runtime_options.limit);
				}
				break;
			default:
				item_list_append_format(&cli_warnings,
										_("--limit not required when executing %s"),
										action_name(action));
		}
	}

	if (runtime_options.all)
	{
		switch (action)
		{
			case CLUSTER_EVENT:
				if (runtime_options.limit_provided == true)
				{
					runtime_options.all = false;
					item_list_append(&cli_warnings,
									 _("--limit provided, ignoring --all"));
				}
				break;
			default:
				item_list_append_format(&cli_warnings,
										_("--all not required when executing %s"),
										action_name(action));
		}
	}

	/* repmgr node service --action */
	if (runtime_options.action[0] != '\0')
	{
		switch (action)
		{
			case NODE_SERVICE:
				break;
			default:
				item_list_append_format(&cli_warnings,
										_("--action will be ignored when executing %s"),
										action_name(action));
		}
	}

	/* repmgr node status --is-shutdown */
	if (runtime_options.is_shutdown == true)
	{
		switch (action)
		{
			case NODE_STATUS:
				break;
			default:
				item_list_append_format(
					&cli_warnings,
					_("--is-shutdown will be ignored when executing %s"),
					action_name(action));
		}
	}

	if (runtime_options.always_promote == true)
	{
		switch (action)
		{
			case STANDBY_SWITCHOVER:
				break;
			default:
				item_list_append_format(
					&cli_warnings,
					_("--always-promote will be ignored when executing %s"),
					action_name(action));
		}
	}

	if (runtime_options.force_rewind == true)
	{
		switch (action)
		{
			case STANDBY_SWITCHOVER:
			case NODE_REJOIN:
				break;
			default:
				item_list_append_format(
					&cli_warnings,
					_("--force-rewind will be ignored when executing %s"),
					action_name(action));
		}
	}


	if (runtime_options.config_files[0] != '\0')
	{
		switch (action)
		{
			case NODE_REJOIN:
				break;
			default:
				item_list_append_format(
					&cli_warnings,
					_("--config-files will be ignored when executing %s"),
					action_name(action));
		}
	}

	/* check only one of --csv, --nagios and --optformat  used */
	{
		int used_options = 0;

		if (runtime_options.csv == true)
			used_options ++;

		if (runtime_options.nagios == true)
			used_options ++;

		if (runtime_options.optformat == true)
			used_options ++;

		if (used_options > 1)
		{
			/* TODO: list which options were used */
			item_list_append(
				&cli_errors,
				"only one of --csv, --nagios and --optformat can be used");
		}
	}
}


static const char*
action_name(const int action)
{
	switch(action)
	{
		case PRIMARY_REGISTER:
			return "PRIMARY REGISTER";
		case PRIMARY_UNREGISTER:
			return "PRIMARY UNREGISTER";

		case STANDBY_CLONE:
			return "STANDBY CLONE";
		case STANDBY_REGISTER:
			return "STANDBY REGISTER";
		case STANDBY_UNREGISTER:
			return "STANDBY UNREGISTER";
		case STANDBY_PROMOTE:
			return "STANDBY PROMOTE";
		case STANDBY_FOLLOW:
			return "STANDBY FOLLOW";

		case BDR_REGISTER:
			return "BDR REGISTER";
		case BDR_UNREGISTER:
			return "BDR UNREGISTER";

		case NODE_STATUS:
			return "NODE STATUS";
		case NODE_CHECK:
			return "NODE CHECK";
		case NODE_REJOIN:
			return "NODE REJOIN";
		case NODE_SERVICE:
			return "NODE SERVICE";

		case CLUSTER_SHOW:
			return "CLUSTER SHOW";
		case CLUSTER_EVENT:
			return "CLUSTER EVENT";
		case CLUSTER_MATRIX:
			return "CLUSTER MATRIX";
		case CLUSTER_CROSSCHECK:
			return "CLUSTER CROSSCHECK";
	}

	return "UNKNOWN ACTION";
}


void
print_error_list(ItemList *error_list, int log_level)
{
	ItemListCell *cell = NULL;

	for (cell = error_list->head; cell; cell = cell->next)
	{
		switch(log_level)
		{
			/* Currently we only need errors and warnings */
			case LOG_ERROR:
				log_error("%s",  cell->string);
				break;
			case LOG_WARNING:
				log_warning("%s",  cell->string);
				break;
		}
	}
}


static void
do_help(void)
{
	printf(_("%s: replication management tool for PostgreSQL\n"), progname());
	puts("");

	/* add a big friendly warning if root is executing "repmgr --help" */
	if (geteuid() == 0)
	{
		printf(_("	**************************************************\n"));
		printf(_("	*** repmgr must be executed by a non-superuser ***\n"));
		printf(_("	**************************************************\n"));
		puts("");
	}

	printf(_("Usage:\n"));
#ifndef BDR_ONLY
	printf(_("	%s [OPTIONS] primary {register|unregister}\n"), progname());
	printf(_("	%s [OPTIONS] standby {register|unregister|clone|promote|follow}\n"), progname());
#endif
	printf(_("	%s [OPTIONS] bdr     {register|unregister}\n"), progname());
	printf(_("	%s [OPTIONS] node    status\n"), progname());
	printf(_("	%s [OPTIONS] cluster {show|event|matrix|crosscheck}\n"), progname());

	puts("");

	printf(_("General options:\n"));
	printf(_("  -?, --help                          show this help, then exit\n"));
	printf(_("  -V, --version                       output version information, then exit\n"));

	puts("");

	printf(_("General configuration options:\n"));
	printf(_("  -b, --pg_bindir=PATH                path to PostgreSQL binaries (optional)\n"));
	printf(_("  -f, --config-file=PATH              path to the repmgr configuration file\n"));
	printf(_("  -F, --force                         force potentially dangerous operations to happen\n"));
	puts("");

	printf(_("Connection options:\n"));
	printf(_("  -S, --superuser=USERNAME            superuser to use if repmgr user is not superuser\n"));

	puts("");

	printf(_("Node-specific options:\n"));
	printf(_("  -D, --pgdata=DIR                    location of the node's data directory \n"));
	printf(_("  --node-id                           specify a node by id (only available for some operations)\n"));
	printf(_("  --node-name                         specify a node by name (only available for some operations)\n"));

	puts("");

	printf(_("Logging options:\n"));
	printf(_("  --dry-run                           show what would happen for action, but don't execute it\n"));
	printf(_("  -L, --log-level                     set log level (overrides configuration file; default: NOTICE)\n"));
	printf(_("  --log-to-file                       log to file (or logging facility) defined in repmgr.conf\n"));
	printf(_("  -t, --terse                         don't display detail, hints and other non-critical output\n"));
	printf(_("  -v, --verbose                       display additional log output (useful for debugging)\n"));

	puts("");

	printf(_("CLUSTER EVENT options:\n"));
	printf(_("  --limit                             maximum number of events to display (default: %i)\n"), CLUSTER_EVENT_LIMIT);
	printf(_("  --all                               display all events (overrides --limit)\n"));
	printf(_("  --event                             filter specific event\n"));

	puts("");
}


/*
 * Create the repmgr extension, and grant access for the repmgr
 * user if not a superuser.
 *
 * Note:
 *   This is one of two places where superuser rights are required.
 *   We should also consider possible scenarious where a non-superuser
 *   has sufficient privileges to install the extension.
 */

bool
create_repmgr_extension(PGconn *conn)
{
	PQExpBufferData	  query;
	PGresult		 *res;

	ExtensionStatus	 extension_status = REPMGR_UNKNOWN;

	t_connection_user  userinfo = T_CONNECTION_USER_INITIALIZER;
	bool			 is_superuser = false;
	PGconn			 *superuser_conn = NULL;
	PGconn			 *schema_create_conn = NULL;

	extension_status = get_repmgr_extension_status(conn);

	switch(extension_status)
	{
		case REPMGR_UNKNOWN:
			log_error(_("unable to determine status of \"repmgr\" extension"));
			return false;

		case REPMGR_UNAVAILABLE:
			log_error(_("\"repmgr\" extension is not available"));
			return false;

		case REPMGR_INSTALLED:
			/* TODO: check version */
			log_info(_("\"repmgr\" extension is already installed"));
			return true;

		case REPMGR_AVAILABLE:
			if (runtime_options.dry_run == true)
			{
				log_notice(_("would now attempt to install extension \"repmgr\""));
			}
			else
			{
				log_notice(_("attempting to install extension \"repmgr\""));
			}
			break;
	}

	/* 3. Attempt to get a superuser connection */

	is_superuser = is_superuser_connection(conn, &userinfo);

	get_superuser_connection(&conn, &superuser_conn, &schema_create_conn);

	if (runtime_options.dry_run == true)
		return true;

	/* 4. Create extension */

	initPQExpBuffer(&query);

	wrap_ddl_query(&query, config_file_options.replication_type,
				   "CREATE EXTENSION repmgr");

	res = PQexec(schema_create_conn, query.data);

	termPQExpBuffer(&query);

	if ((PQresultStatus(res) != PGRES_COMMAND_OK && PQresultStatus(res) != PGRES_TUPLES_OK))
	{
		log_error(_("unable to create \"repmgr\" extension:\n  %s"),
				  PQerrorMessage(schema_create_conn));
		log_hint(_("check that the provided user has sufficient privileges for CREATE EXTENSION"));

		PQclear(res);
		if (superuser_conn != NULL)
			PQfinish(superuser_conn);
		return false;
	}

	PQclear(res);

	/* 5. If not superuser, grant usage */
	if (is_superuser == false)
	{
		initPQExpBuffer(&query);

		wrap_ddl_query(&query, config_file_options.replication_type,
					   "GRANT USAGE ON SCHEMA repmgr TO %s",
					   userinfo.username);

		res = PQexec(schema_create_conn, query.data);

		termPQExpBuffer(&query);
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
		{
			log_error(_("unable to grant usage on \"repmgr\" extension to %s:\n  %s"),
					  userinfo.username,
					  PQerrorMessage(schema_create_conn));
			PQclear(res);

			if (superuser_conn != 0)
				PQfinish(superuser_conn);

			return false;
		}

		initPQExpBuffer(&query);
		wrap_ddl_query(&query, config_file_options.replication_type,
					   "GRANT ALL ON ALL TABLES IN SCHEMA repmgr TO %s",
					   userinfo.username);

		res = PQexec(schema_create_conn, query.data);

		termPQExpBuffer(&query);

		if (PQresultStatus(res) != PGRES_COMMAND_OK)
		{
			log_error(_("unable to grant permission on tables on \"repmgr\" extension to %s:\n  %s"),
					  userinfo.username,
					  PQerrorMessage(schema_create_conn));
			PQclear(res);

			if (superuser_conn != NULL)
				PQfinish(superuser_conn);

			return false;
		}
	}

	if (superuser_conn != NULL)
		PQfinish(superuser_conn);

	log_notice(_("\"repmgr\" extension successfully installed"));

	create_event_notification(conn,
						&config_file_options,
						config_file_options.node_id,
						"cluster_created",
						true,
						NULL);

	return true;
}


/**
 * check_server_version()
 *
 * Verify that the server is MIN_SUPPORTED_VERSION_NUM or later
 *
 * PGconn *conn:
 *	 the connection to check
 *
 * char *server_type:
 *	 either "primary" or "standby"; used to format error message
 *
 * bool exit_on_error:
 *	 exit if reported server version is too low; optional to enable some callers
 *	 to perform additional cleanup
 *
 * char *server_version_string
 *	 passed to get_server_version(), which will place the human-readable
 *	 server version string there (e.g. "9.4.0")
 */
int
check_server_version(PGconn *conn, char *server_type, bool exit_on_error, char *server_version_string)
{
	int			conn_server_version_num = UNKNOWN_SERVER_VERSION_NUM;

	conn_server_version_num = get_server_version(conn, server_version_string);
	if (conn_server_version_num < MIN_SUPPORTED_VERSION_NUM)
	{
		if (conn_server_version_num > 0)
			log_error(_("%s requires %s to be PostgreSQL %s or later"),
					  progname(),
					  server_type,
					  MIN_SUPPORTED_VERSION
				);

		if (exit_on_error == true)
		{
			PQfinish(conn);
			exit(ERR_BAD_CONFIG);
		}

		return -1;
	}

	return conn_server_version_num;
}


int
test_ssh_connection(char *host, char *remote_user)
{
	char		script[MAXLEN] = "";
	int			r = 1, i;

	/* On some OS, true is located in a different place than in Linux
	 * we have to try them all until all alternatives are gone or we
	 * found `true' because the target OS may differ from the source
	 * OS
	 */
	const char *bin_true_paths[] = {
		"/bin/true",
		"/usr/bin/true",
		NULL
	};

	for (i = 0; bin_true_paths[i] && r != 0; ++i)
	{
		if (!remote_user[0])
			maxlen_snprintf(script, "ssh -o Batchmode=yes %s %s %s 2>/dev/null",
							config_file_options.ssh_options, host, bin_true_paths[i]);
		else
			maxlen_snprintf(script, "ssh -o Batchmode=yes %s %s -l %s %s 2>/dev/null",
							config_file_options.ssh_options, host, remote_user,
							bin_true_paths[i]);

		log_verbose(LOG_DEBUG, _("test_ssh_connection(): executing %s"), script);
		r = system(script);
	}

	if (r != 0)
		log_warning(_("unable to connect to remote host '%s' via SSH"), host);

	return r;
}


/*
 * Execute a command locally. "outputbuf" should either be an
 * initialised PQexpbuffer, or NULL
 */
bool
local_command(const char *command, PQExpBufferData *outputbuf)
{
	FILE *fp;
	char output[MAXLEN];
	int retval = 0;

	if (outputbuf == NULL)
	{
		retval = system(command);
		return (retval == 0) ? true : false;
	}

	fp = popen(command, "r");

	if (fp == NULL)
	{
		log_error(_("unable to execute local command:\n%s"), command);
		return false;
	}

	/* TODO: better error handling */
	while (fgets(output, MAXLEN, fp) != NULL)
	{
		appendPQExpBuffer(outputbuf, "%s", output);
	}

	pclose(fp);

	if (outputbuf->data != NULL)
		log_verbose(LOG_DEBUG, "local_command(): output returned was:\n%s", outputbuf->data);
	else
		log_verbose(LOG_DEBUG, "local_command(): no output returned");

	return true;
}


void
get_superuser_connection(PGconn **conn, PGconn **superuser_conn, PGconn **privileged_conn)
{
	t_connection_user userinfo = T_CONNECTION_USER_INITIALIZER;
	bool			  is_superuser = false;

	/* this should never happen */
	if (PQstatus(*conn) != CONNECTION_OK)
	{
		log_error(_("no database connection available"));
		exit(ERR_INTERNAL);
	}
	is_superuser = is_superuser_connection(*conn, &userinfo);

	if (is_superuser == true)
	{
		*privileged_conn = *conn;

		return;
	}

	if (runtime_options.superuser[0] == '\0')
	{
		log_error(_("\"%s\" is not a superuser and no superuser name supplied"), userinfo.username);
		log_hint(_("supply a valid superuser name with -S/--superuser"));
		PQfinish(*conn);
		exit(ERR_BAD_CONFIG);
	}

	*superuser_conn = establish_db_connection_as_user(config_file_options.conninfo,
													 runtime_options.superuser,
													 false);

	if (PQstatus(*superuser_conn) != CONNECTION_OK)
	{
		log_error(_("unable to establish superuser connection as \"%s\""),
				  runtime_options.superuser);

		PQfinish(*conn);
		exit(ERR_BAD_CONFIG);
	}

	/* check provided superuser really is superuser */
	if (!is_superuser_connection(*superuser_conn, NULL))
	{
		log_error(_("\"%s\" is not a superuser"), runtime_options.superuser);
		PQfinish(*superuser_conn);
		PQfinish(*conn);
		exit(ERR_BAD_CONFIG);
	}

	*privileged_conn = *superuser_conn;
	return;
}


/*
 * check_upstream_config()
 *
 * Perform sanity check on upstream server configuration before starting cloning
 * process
 *
 * TODO:
 *  - check user is qualified to perform base backup
 */

bool
check_upstream_config(PGconn *conn, int server_version_num, bool exit_on_error)
{
	int			i;
	bool		config_ok = true;
	char	   *wal_error_message = NULL;
	t_basebackup_options  backup_options = T_BASEBACKUP_OPTIONS_INITIALIZER;
	bool		backup_options_ok = true;
	ItemList	backup_option_errors = { NULL, NULL };
	bool		xlog_stream = true;
	standy_clone_mode mode;

	/*
	 * Detecting the intended cloning mode
	 */
	mode = get_standby_clone_mode();

	/*
	 * Parse `pg_basebackup_options`, if set, to detect whether --xlog-method
	 * has been set to something other than `stream` (i.e. `fetch`), as
	 * this will influence some checks
	 */

	backup_options_ok = parse_pg_basebackup_options(
		config_file_options.pg_basebackup_options,
		&backup_options, server_version_num,
		&backup_option_errors);

	if (backup_options_ok == false)
	{
		if (exit_on_error == true)
		{
			log_error(_("error(s) encountered parsing 'pg_basebackup_options'"));
			print_error_list(&backup_option_errors, LOG_ERR);
			log_hint(_("'pg_basebackup_options' is: '%s'"),
					 config_file_options.pg_basebackup_options);
			exit(ERR_BAD_CONFIG);
		}

		config_ok = false;
	}

	if (strlen(backup_options.xlog_method) && strcmp(backup_options.xlog_method, "stream") != 0)
		xlog_stream = false;

	/* Check that WAL level is set correctly */
	{
		char *levels_pre96[] = {
			"hot_standby",
			"logical",
			NULL,
		};

		/*
		 * Note that in 9.6+, "hot_standby" and "archive" are accepted as aliases
		 * for "replica", but current_setting() will of course always return "replica"
		 */
		char *levels_96plus[] = {
			"replica",
			"logical",
			NULL,
		};

		char **levels;
		int j = 0;

		if (server_version_num < 90600)
		{
			levels = (char **)levels_pre96;
			wal_error_message = _("parameter 'wal_level' must be set to 'hot_standby' or 'logical'");
		}
		else
		{
			levels = (char **)levels_96plus;
			wal_error_message = _("parameter 'wal_level' must be set to 'replica' or 'logical'");
		}

		do
		{
			i = guc_set(conn, "wal_level", "=", levels[j]);
			if (i)
			{
				break;
			}
			j++;
		} while (levels[j] != NULL);
	}

	if (i == 0 || i == -1)
	{
		if (i == 0)
		{
			log_error("%s", wal_error_message);
		}

		if (exit_on_error == true)
		{
			PQfinish(conn);
			exit(ERR_BAD_CONFIG);
		}

		config_ok = false;
	}

	if (config_file_options.use_replication_slots)
	{
		i = guc_set_typed(conn, "max_replication_slots", ">",
						  "0", "integer");
		if (i == 0 || i == -1)
		{
			if (i == 0)
			{
				log_error(_("parameter \"max_replication_slots\" must be set to at least 1 to enable replication slots"));
				log_hint(_("\"max_replication_slots\" should be set to at least the number of expected standbys"));
				if (exit_on_error == true)
				{
					PQfinish(conn);
					exit(ERR_BAD_CONFIG);
				}

				config_ok = false;
			}
		}
	}
	/*
	 * physical replication slots not available or not requested - check if
	 * there are any circumstances where `wal_keep_segments` should be set
	 */
	else if (mode != barman)
	{
		bool check_wal_keep_segments = false;
		char min_wal_keep_segments[MAXLEN] = "1";

		/*
		 * -w/--wal-keep-segments was supplied - check against that value
		 */
		if (runtime_options.wal_keep_segments_used == true)
		{
			check_wal_keep_segments = true;
			strncpy(min_wal_keep_segments, runtime_options.wal_keep_segments, MAXLEN);
		}

		/*
		 * A non-zero `wal_keep_segments` value will almost certainly be required
		 * if pg_basebackup is being used with --xlog-method=fetch,
		 * *and* no restore command has been specified
		 */
		else if (xlog_stream == false
			 && strcmp(config_file_options.restore_command, "") == 0)
		{
			check_wal_keep_segments = true;
		}

		if (check_wal_keep_segments == true)
		{
			i = guc_set_typed(conn, "wal_keep_segments", ">=", min_wal_keep_segments, "integer");

			if (i == 0 || i == -1)
			{
				if (i == 0)
				{
					log_error(_("parameter \"wal_keep_segments\" on the upstream server must be be set to %s or greater"),
							min_wal_keep_segments);
					log_hint(_("Choose a value sufficiently high enough to retain enough WAL "
							   "until the standby has been cloned and started.\n "
							   "Alternatively set up WAL archiving using e.g. PgBarman and configure "
							   "'restore_command' in repmgr.conf to fetch WALs from there."));

					if (server_version_num >= 90400)
					{
						log_hint(_("In PostgreSQL 9.4 and later, replication slots can be used, which "
								   "do not require \"wal_keep_segments\" to be set "
								   "(set parameter \"use_replication_slots\" in repmgr.conf to enable)\n"
									 ));
					}
				}

				if (exit_on_error == true)
				{
					PQfinish(conn);
					exit(ERR_BAD_CONFIG);
				}

				config_ok = false;
			}
		}
	}


	/*
	 * If archive_mode is enabled, check that 'archive_command' is non empty
	 * (however it's not practical to check that it actually represents a valid
	 * command).
	 *
	 * From PostgreSQL 9.5, archive_mode can be one of 'off', 'on' or 'always'
	 * so for ease of backwards compatibility, rather than explicitly check for an
	 * enabled mode, check that it's not "off".
	 */

	if (guc_set(conn, "archive_mode", "!=", "off"))
	{
		i = guc_set(conn, "archive_command", "!=", "");

		if (i == 0 || i == -1)
		{
			if (i == 0)
				log_error(_("parameter \"archive_command\" must be set to a valid command"));

			if (exit_on_error == true)
			{
				PQfinish(conn);
				exit(ERR_BAD_CONFIG);
			}

			config_ok = false;
		}
	}


	/*
	 * Check that 'hot_standby' is on. This isn't strictly necessary
	 * for the primary server, however the assumption is that we'll be
	 * cloning standbys and thus copying the primary configuration;
	 * this way the standby will be correctly configured by default.
	 */

	i = guc_set(conn, "hot_standby", "=", "on");
	if (i == 0 || i == -1)
	{
		if (i == 0)
		{
			log_error(_("parameter 'hot_standby' must be set to 'on'"));
		}

		if (exit_on_error == true)
		{
			PQfinish(conn);
			exit(ERR_BAD_CONFIG);
		}

		config_ok = false;
	}

	i = guc_set_typed(conn, "max_wal_senders", ">", "0", "integer");
	if (i == 0 || i == -1)
	{
		if (i == 0)
		{
			log_error(_("parameter \"max_wal_senders\" must be set to be at least 1"));
			log_hint(_("\"max_wal_senders\" should be set to at least the number of expected standbys"));
		}

		if (exit_on_error == true)
		{
			PQfinish(conn);
			exit(ERR_BAD_CONFIG);
		}

		config_ok = false;
	}

	/*
	 * If using pg_basebackup, ensure sufficient replication connections can be made.
	 * There's no guarantee they'll still be available by the time pg_basebackup
	 * is executed, but there's nothing we can do about that.
	 */
	if (mode == pg_basebackup)
	{

		PGconn	  **connections;
		int			i;
		int			min_replication_connections = 1,
					possible_replication_connections = 0;

		t_conninfo_param_list repl_conninfo;

		/* Make a copy of the connection parameter arrays, and append "replication" */

		initialize_conninfo_params(&repl_conninfo, false);

		conn_to_param_list(conn, &repl_conninfo);

		param_set(&repl_conninfo, "replication", "1");

		if (*runtime_options.replication_user)
			param_set(&repl_conninfo, "user", runtime_options.replication_user);

		/*
		 * work out how many replication connections are required (1 or 2)
		 */

		if (xlog_stream == true)
			min_replication_connections += 1;

		log_verbose(LOG_NOTICE, "checking for available walsenders on upstream node (%i required)",
					min_replication_connections);

		connections = pg_malloc0(sizeof(PGconn *) * min_replication_connections);

		/* Attempt to create the minimum number of required concurrent connections */
		for (i = 0; i < min_replication_connections; i++)
		{
			PGconn *replication_conn;

			replication_conn = establish_db_connection_by_params(&repl_conninfo, false);

			if (PQstatus(replication_conn) == CONNECTION_OK)
			{
				connections[i] = replication_conn;
				possible_replication_connections++;
			}
		}

		/* Close previously created connections */
		for (i = 0; i < possible_replication_connections; i++)
		{
			PQfinish(connections[i]);
		}

		pfree(connections);
		free_conninfo_params(&repl_conninfo);

		if (possible_replication_connections < min_replication_connections)
		{
			config_ok = false;

			/*
			 * XXX at this point we could check current_setting('max_wal_senders) - COUNT(*) FROM pg_stat_replication;
			 * if >= min_replication_connections we could infer possible authentication error.
			 *
			 * Alternatively call PQconnectStart() and poll for presence/absence of CONNECTION_AUTH_OK ?
			 */
			log_error(_("unable to establish necessary replication connections"));
			log_hint(_("increase \"max_wal_senders\" by at least %i"),
					 min_replication_connections - possible_replication_connections);

			if (exit_on_error == true)
			{
				PQfinish(conn);
				exit(ERR_BAD_CONFIG);
			}
		}

		log_verbose(LOG_INFO, "sufficient walsenders available on upstream node (%i required)",
					min_replication_connections);
	}

	return config_ok;
}

standy_clone_mode
get_standby_clone_mode(void)
{
	standy_clone_mode mode;

	if (strcmp(config_file_options.barman_host, "") != 0 && ! runtime_options.without_barman)
		mode = barman;
	else
		mode = pg_basebackup;

	return mode;
}


char *
make_pg_path(const char *file)
{
	maxlen_snprintf(path_buf, "%s%s", pg_bindir, file);

	return path_buf;
}


int
copy_remote_files(char *host, char *remote_user, char *remote_path,
				  char *local_path, bool is_directory, int server_version_num)
{
	PQExpBufferData 	rsync_flags;
	char		script[MAXLEN] = "";
	char		host_string[MAXLEN] = "";
	int			r = 0;

	initPQExpBuffer(&rsync_flags);

	if (*config_file_options.rsync_options == '\0')
	{
		appendPQExpBuffer(&rsync_flags, "%s",
						  "--archive --checksum --compress --progress --rsh=ssh");
	}
	else
	{
		appendPQExpBuffer(&rsync_flags, "%s",
						  config_file_options.rsync_options);
	}

	if (runtime_options.force)
	{
		appendPQExpBuffer(&rsync_flags, "%s",
						  " --delete --checksum");
	}

	if (!remote_user[0])
	{
		maxlen_snprintf(host_string, "%s", host);
	}
	else
	{
		maxlen_snprintf(host_string, "%s@%s", remote_user, host);
	}

	/*
	 * When copying the main PGDATA directory, certain files and contents
	 * of certain directories need to be excluded.
	 *
	 * See function 'sendDir()' in 'src/backend/replication/basebackup.c' -
	 * we're basically simulating what pg_basebackup does, but with rsync rather
	 * than the BASEBACKUP replication protocol command.
	 *
	 * *However* currently we'll always copy the contents of the 'pg_replslot'
	 * directory and delete later if appropriate.
	 */
	if (is_directory)
	{
		/* Files which we don't want */
		appendPQExpBuffer(&rsync_flags, "%s",
						  " --exclude=postmaster.pid --exclude=postmaster.opts --exclude=global/pg_control");

		appendPQExpBuffer(&rsync_flags, "%s",
						  " --exclude=recovery.conf --exclude=recovery.done");

		if (server_version_num >= 90400)
		{
			/*
			 * Ideally we'd use PG_AUTOCONF_FILENAME from utils/guc.h, but
			 * that has too many dependencies for a mere client program.
			 */
			appendPQExpBuffer(&rsync_flags, "%s",
							  " --exclude=postgresql.auto.conf.tmp");
		}

		/* Temporary files which we don't want, if they exist */
		appendPQExpBuffer(&rsync_flags, " --exclude=%s*",
						  PG_TEMP_FILE_PREFIX);

		/* Directories which we don't want */

		if (server_version_num >= 100000)
		{
			appendPQExpBuffer(&rsync_flags, "%s",
							  " --exclude=pg_wal/*");
		}
		else
		{
			appendPQExpBuffer(&rsync_flags, "%s",
							  " --exclude=pg_xlog/*");
		}

		appendPQExpBuffer(&rsync_flags, "%s",
						  " --exclude=pg_log/* --exclude=pg_stat_tmp/*");

		maxlen_snprintf(script, "rsync %s %s:%s/* %s",
						rsync_flags.data, host_string, remote_path, local_path);
	}
	else
	{
		maxlen_snprintf(script, "rsync %s %s:%s %s",
						rsync_flags.data, host_string, remote_path, local_path);
	}

	log_info(_("rsync command line: '%s'"), script);

	r = system(script);

	log_debug("copy_remote_files(): r = %i; WIFEXITED: %i; WEXITSTATUS: %i", r, WIFEXITED(r), WEXITSTATUS(r));

	/* exit code 24 indicates vanished files, which isn't a problem for us */
	if (WIFEXITED(r) && WEXITSTATUS(r) && WEXITSTATUS(r) != 24)
		log_verbose(LOG_WARNING, "copy_remote_files(): rsync returned unexpected exit status %i", WEXITSTATUS(r));

	return r;
}



/*
 * Creates a recovery.conf file for a standby
 *
 * A database connection pointer is required for escaping primary_conninfo
 * parameters. When cloning from Barman and --no-upstream-connection ) this
 * might not be available.
 */
bool
create_recovery_file(t_node_info *node_record, t_conninfo_param_list *recovery_conninfo, const char *data_dir)
{
	FILE	   *recovery_file;
	char		recovery_file_path[MAXPGPATH] = "";
	char		line[MAXLEN] = "";
	mode_t		um;

	maxpath_snprintf(recovery_file_path, "%s/%s", data_dir, RECOVERY_COMMAND_FILE);

	/* Set umask to 0600 */
	um = umask((~(S_IRUSR | S_IWUSR)) & (S_IRWXG | S_IRWXO));
	recovery_file = fopen(recovery_file_path, "w");
	umask(um);

	if (recovery_file == NULL)
	{
		log_error(_("unable to create recovery.conf file at \"%s\""),
				  recovery_file_path);
		return false;
	}

	log_debug("create_recovery_file(): creating \"%s\"...",
			  recovery_file_path);

	/* standby_mode = 'on' */
	maxlen_snprintf(line, "standby_mode = 'on'\n");

	if (write_recovery_file_line(recovery_file, recovery_file_path, line) == false)
		return false;

	log_debug("recovery.conf: %s", line);

	/* primary_conninfo = '...' */

	/*
	 * the user specified --upstream-conninfo string - copy that
	 */
	if (strlen(runtime_options.upstream_conninfo))
	{
		char *escaped = escape_recovery_conf_value(runtime_options.upstream_conninfo);
		maxlen_snprintf(line, "primary_conninfo = '%s'\n",
						escaped);
		free(escaped);
	}
	/*
	 * otherwise use the conninfo inferred from the upstream connection
	 * and/or node record
	 */
	else
	{
		write_primary_conninfo(line, recovery_conninfo);
	}

	if (write_recovery_file_line(recovery_file, recovery_file_path, line) == false)
		return false;

	log_debug("recovery.conf: %s", line);

	/* recovery_target_timeline = 'latest' */
	maxlen_snprintf(line, "recovery_target_timeline = 'latest'\n");

	if (write_recovery_file_line(recovery_file, recovery_file_path, line) == false)
		return false;

	log_debug("recovery.conf: %s", line);

	/* recovery_min_apply_delay = ... (optional) */
	if (*runtime_options.recovery_min_apply_delay)
	{
		maxlen_snprintf(line, "recovery_min_apply_delay = %s\n",
						runtime_options.recovery_min_apply_delay);
		if (write_recovery_file_line(recovery_file, recovery_file_path, line) == false)
			return false;

		log_debug("recovery.conf: %s", line);
	}

	/* primary_slot_name = '...' (optional, for 9.4 and later) */
	if (config_file_options.use_replication_slots)
	{
		maxlen_snprintf(line, "primary_slot_name = %s\n",
						node_record->slot_name);
		if (write_recovery_file_line(recovery_file, recovery_file_path, line) == false)
			return false;

		log_debug("recovery.conf: %s", line);
	}

	/* If restore_command is set, we use it as restore_command in recovery.conf */
	if (strcmp(config_file_options.restore_command, "") != 0)
	{
		maxlen_snprintf(line, "restore_command = '%s'\n",
						config_file_options.restore_command);
		if (write_recovery_file_line(recovery_file, recovery_file_path, line) == false)
		        return false;

		log_debug("recovery.conf: %s", line);
	}
	fclose(recovery_file);

	return true;
}


static bool
write_recovery_file_line(FILE *recovery_file, char *recovery_file_path, char *line)
{
	if (fputs(line, recovery_file) == EOF)
	{
		log_error(_("unable to write to recovery file at \"%s\""), recovery_file_path);
		fclose(recovery_file);
		return false;
	}

	return true;
}


static void
write_primary_conninfo(char *line, t_conninfo_param_list *param_list)
{
	PQExpBufferData conninfo_buf;
	bool application_name_provided = false;
	int c;
	char *escaped = NULL;

	initPQExpBuffer(&conninfo_buf);

	for (c = 0; c < param_list->size && param_list->keywords[c] != NULL; c++)
	{
		/*
		 * Skip empty settings and ones which don't make any sense in
		 * recovery.conf
		 */
		if (strcmp(param_list->keywords[c], "dbname") == 0 ||
		    strcmp(param_list->keywords[c], "replication") == 0 ||
		    (param_list->values[c] == NULL) ||
		    (param_list->values[c] != NULL && param_list->values[c][0] == '\0'))
			continue;

		/* only include "password" if explicitly requested */
		if (runtime_options.use_recovery_conninfo_password == false &&
			strcmp(param_list->keywords[c], "password") == 0)
			continue;

		if (conninfo_buf.len != 0)
			appendPQExpBufferChar(&conninfo_buf, ' ');

		if (strcmp(param_list->keywords[c], "application_name") == 0)
			application_name_provided = true;

		appendPQExpBuffer(&conninfo_buf, "%s=", param_list->keywords[c]);
		appendConnStrVal(&conninfo_buf, param_list->values[c]);
	}

	/* `application_name` not provided - default to repmgr node name */
	if (application_name_provided == false)
	{
		if (strlen(config_file_options.node_name))
		{
			appendPQExpBuffer(&conninfo_buf, " application_name=");
		    appendConnStrVal(&conninfo_buf, config_file_options.node_name);
		}
		else
		{
			appendPQExpBuffer(&conninfo_buf, " application_name=repmgr");
		}
	}
	escaped = escape_recovery_conf_value(conninfo_buf.data);

	maxlen_snprintf(line, "primary_conninfo = '%s'\n", escaped);

	free(escaped);

	termPQExpBuffer(&conninfo_buf);
}


/*
 * Execute a command via ssh on the remote host.
 *
 * TODO: implement SSH calls using libssh2.
 */
bool
remote_command(const char *host, const char *user, const char *command, PQExpBufferData *outputbuf)
{
	FILE *fp;
	char ssh_command[MAXLEN] = "";
	PQExpBufferData ssh_host;

	char output[MAXLEN] = "";

	initPQExpBuffer(&ssh_host);

	if (*user != '\0')
	{
		appendPQExpBuffer(&ssh_host, "%s@", user);
	}

	appendPQExpBuffer(&ssh_host, "%s", host);

	maxlen_snprintf(ssh_command,
					"ssh -o Batchmode=yes %s %s %s",
					config_file_options.ssh_options,
					ssh_host.data,
					command);

	termPQExpBuffer(&ssh_host);

	log_debug("remote_command():\n  %s", ssh_command);

	fp = popen(ssh_command, "r");

	if (fp == NULL)
	{
		log_error(_("unable to execute remote command:\n  %s"), ssh_command);
		return false;
	}

	if (outputbuf != NULL)
	{
		/* TODO: better error handling */
		while (fgets(output, MAXLEN, fp) != NULL)
		{
			appendPQExpBuffer(outputbuf, "%s", output);
		}
	}
	else
	{
		/*
		 * When executed remotely, repmgr commands which execute pg_ctl (particularly
		 * `repmgr standby follow`) will see the pg_ctl command appear to fail with a
		 * non-zero return code when the output from the executed pg_ctl command
		 * has nowhere to go, even though the command actually succeeds. We'll consume an
		 * arbitrary amount of output and throw it away to work around this.
		 */
		int i = 0;
		while (fgets(output, MAXLEN, fp) != NULL && i < 10)
		{
			i++;
		}
	}

	pclose(fp);

	if (outputbuf != NULL)
		log_verbose(LOG_DEBUG, "remote_command(): output returned was:\n  %s", outputbuf->data);

	return true;
}


void
make_remote_repmgr_path(PQExpBufferData *output_buf, t_node_info *remote_node_record)
{
	appendPQExpBuffer(output_buf,
					  "%s -f %s ",
					  make_pg_path("repmgr"),
					  remote_node_record->config_file);

}


/* ======================== */
/* server control functions */
/* ======================== */

void
get_server_action(t_server_action action, char *script, char *data_dir)
{
	PQExpBufferData command;

	if (data_dir == NULL || data_dir[0] == '\0')
		data_dir = "(none provided)";

	switch(action)
	{
		case ACTION_NONE:
			script[0] = '\0';
			return;

		case ACTION_START:
		{
			if (config_file_options.service_start_command[0] != '\0')
			{
				maxlen_snprintf(script, "%s",
								config_file_options.service_start_command);
			}
			else
			{
				initPQExpBuffer(&command);

				appendPQExpBuffer(
					&command,
					"%s %s -w -D ",
					make_pg_path("pg_ctl"),
					config_file_options.pg_ctl_options);

				appendShellString(
					&command,
					data_dir);

				appendPQExpBuffer(
					&command,
					" start");

				strncpy(script, command.data, MAXLEN);

				termPQExpBuffer(&command);
			}

			return;
		}

		case ACTION_STOP:
		{
			if (config_file_options.service_stop_command[0] != '\0')
			{
				maxlen_snprintf(script, "%s",
								config_file_options.service_stop_command);
			}
			else
			{
				initPQExpBuffer(&command);
				appendPQExpBuffer(
					&command,
					"%s %s -D ",
					make_pg_path("pg_ctl"),
					config_file_options.pg_ctl_options);

				appendShellString(
					&command,
					data_dir);

				appendPQExpBuffer(
					&command,
					" -m fast -W stop");

				strncpy(script, command.data, MAXLEN);

				termPQExpBuffer(&command);
			}
			return;
		}

		case ACTION_RESTART:
		{
			if (config_file_options.service_restart_command[0] != '\0')
			{
				maxlen_snprintf(script, "%s",
								config_file_options.service_restart_command);
			}
			else
			{
				initPQExpBuffer(&command);
				appendPQExpBuffer(
					&command,
					"%s %s -w -D ",
					make_pg_path("pg_ctl"),
					config_file_options.pg_ctl_options);

				appendShellString(
					&command,
					data_dir);

				appendPQExpBuffer(
					&command,
					" restart");

				strncpy(script, command.data, MAXLEN);

				termPQExpBuffer(&command);
			}
			return;
		}

		case ACTION_RELOAD:
		{
			if (config_file_options.service_reload_command[0] != '\0')
			{
				maxlen_snprintf(script, "%s",
								config_file_options.service_reload_command);
			}
			else
			{
				initPQExpBuffer(&command);
				appendPQExpBuffer(
					&command,
					"%s %s -w -D ",
					make_pg_path("pg_ctl"),
					config_file_options.pg_ctl_options);

				appendShellString(
					&command,
					data_dir);

				appendPQExpBuffer(
					&command,
					" reload");

				strncpy(script, command.data, MAXLEN);

				termPQExpBuffer(&command);

			}
			return;
		}

		case ACTION_PROMOTE:
		{
			if (config_file_options.service_promote_command[0] != '\0')
			{
				maxlen_snprintf(script, "%s",
								config_file_options.service_promote_command);
			}
			else
			{
				initPQExpBuffer(&command);
				appendPQExpBuffer(
					&command,
					"%s %s -w -D ",
					make_pg_path("pg_ctl"),
					config_file_options.pg_ctl_options);

				appendShellString(
					&command,
					data_dir);

				appendPQExpBuffer(
					&command,
					" promote");

				strncpy(script, command.data, MAXLEN);

				termPQExpBuffer(&command);
			}
			return;
		}

		default:
			return;
	}

	return;
}


bool
data_dir_required_for_action(t_server_action action)
{
	switch(action)
	{
		case ACTION_NONE:
			return false;

		case ACTION_START:
			if (config_file_options.service_start_command[0] != '\0')
			{
				return false;
			}
			return true;

		case ACTION_STOP:
			if (config_file_options.service_stop_command[0] != '\0')
			{
				return false;
			}
			return true;

		case ACTION_RESTART:
			if (config_file_options.service_restart_command[0] != '\0')
			{
				return false;
			}
			return true;

		case ACTION_RELOAD:
			if (config_file_options.service_reload_command[0] != '\0')
			{
				return false;
			}
			return true;

		case ACTION_PROMOTE:
			if (config_file_options.service_promote_command[0] != '\0')
			{
				return false;
			}
			return true;

		default:
			return false;
	}

	return false;
}


void
get_node_data_directory(char *data_dir_buf)
{
	/*
	 * the configuration file setting has priority, and will always be
	 * set when a configuration file was provided
	 */
	if (config_file_options.data_directory[0] != '\0')
	{
		strncpy(data_dir_buf, config_file_options.data_directory, MAXPGPATH);
		return;
	}

	if (runtime_options.data_dir[0] != '\0')
	{
		strncpy(data_dir_buf, runtime_options.data_dir, MAXPGPATH);
		return;
	}

	return;
}


/*
 * initialise a node record from the provided configuration
 * parameters
 */
void
init_node_record(t_node_info *node_record)
{
	node_record->node_id = config_file_options.node_id;
	node_record->upstream_node_id = runtime_options.upstream_node_id;
	node_record->priority = config_file_options.priority;
	node_record->active = true;

	strncpy(node_record->location, config_file_options.location, MAXLEN);

	strncpy(node_record->node_name, config_file_options.node_name, MAXLEN);
	strncpy(node_record->conninfo, config_file_options.conninfo, MAXLEN);
	strncpy(node_record->config_file, config_file_path, MAXLEN);

	if (config_file_options.replication_user[0] != '\0')
	{
		/* replication user explicitly provided */
		strncpy(node_record->repluser, config_file_options.replication_user, NAMEDATALEN);
	}
	else
	{
		/* use the "user" value from "conninfo" */
		(void)get_conninfo_value(config_file_options.conninfo, "user", node_record->repluser);
	}

	if (config_file_options.use_replication_slots == true)
	{
		maxlen_snprintf(node_record->slot_name, "repmgr_slot_%i", config_file_options.node_id);
	}
}
