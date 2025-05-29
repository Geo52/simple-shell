
#include <ctype.h>	   /* Character types                       */
#include <stdio.h>	   /* Standard buffered input/output        */
#include <stdlib.h>	   /* Standard library functions            */
#include <string.h>	   /* String operations                     */
#include <sys/types.h> /* Data types                            */
#include <sys/wait.h>  /* Declarations for waiting              */
#include <unistd.h>	   /* Standard symbolic constants and types */

#define SHELL_BUFFER_SIZE 256 /* Size of the Shell input buffer        */
#define SHELL_MAX_ARGS 8	  /* Maximum number of arguments parsed    */
#define HISTORY_SIZE 10		  /* Size of the command history circular queue */

enum
{
	STATE_SPACE,
	STATE_NON_SPACE,
}; /* Parser states */

/* Global shell nesting level counter */
int shell_nesting_level = 0;
#define MAX_SHELL_NESTING 3 /* Maximum allowed nesting of subshells */

int search_path(const char *cmd, char *const args[])
{
	char *path_env, *path_copy, *dir, *path_to_exec;
	int found = 0;

	path_env = getenv("PATH");

	// Make a copy of PATH since strtok modifies the string
	path_copy = strdup(path_env);

	path_to_exec = malloc(strlen(path_env) + strlen(cmd) + 2);

	dir = strtok(path_copy, ":");
	while (dir != NULL)
	{
		// Construct the full path
		sprintf(path_to_exec, "%s/%s", dir, cmd);

		// Check if the file exists and is executable
		if (access(path_to_exec, X_OK) == 0)
		{
			found = 1;
			break;
		}

		dir = strtok(NULL, ":");
	}

	// Execute the command if found
	int result = -1;
	if (found)
	{
		result = execv(path_to_exec, args) ? -1 : 0;
	}
	else
	{
		fprintf(stderr, "Command %s does not exist\n", cmd);
	}

	// Clean up (though we won't reach here if execv succeeds)
	free(path_copy);
	free(path_to_exec);

	return result;
}

int imthechild(const char *path_to_exec, char *const args[])
{

	if (strchr(path_to_exec, '/') != NULL)
	{
		return execv(path_to_exec, args) ? -1 : 0;
	}

	return search_path(path_to_exec, args);
}
void imtheparent(pid_t child_pid, int run_in_background)
{
	int child_return_val, child_error_code;

	/* fork returned a positive pid so we are the parent */
	fprintf(stderr, "  Parent says 'child process has been forked with pid=%d'\n", child_pid);
	if (run_in_background)
	{
		fprintf(stderr, "  Parent says 'run_in_background=1 ... so we're not waiting for the child'\n");
		return;
	}

	// Using waitpid to wait specifically for the direct child process
	waitpid(child_pid, &child_return_val, 0);

	/* Use the WEXITSTATUS to extract the status code from the return value */
	child_error_code = WEXITSTATUS(child_return_val);
	fprintf(stderr, "  Parent says 'waitpid() returned so the child with pid=%d is finished.'\n", child_pid);
	if (child_error_code != 0)
	{
		/* Error: Child process failed. Most likely a failed exec */
		fprintf(stderr, "  Parent says 'Child process %d failed with code %d'\n", child_pid, child_error_code);
	}
}

int parse_and_execute(char *buffer, int *command_counter, char cmd_history[HISTORY_SIZE][SHELL_BUFFER_SIZE])
{
	pid_t pid_from_fork;
	int exec_argc, i, parser_state, run_in_background;
	char *exec_argv[SHELL_MAX_ARGS + 1];
	int n_read = strlen(buffer);
	char original_command[SHELL_BUFFER_SIZE];

	strncpy(original_command, buffer, SHELL_BUFFER_SIZE);

	run_in_background = n_read > 2 && buffer[n_read - 2] == '&';
	buffer[n_read - run_in_background - 1] = '\0';

	parser_state = STATE_SPACE;
	for (exec_argc = 0, i = 0; (buffer[i] != '\0') && (exec_argc < SHELL_MAX_ARGS); i++)
	{
		if (!isspace(buffer[i])) // isspace returns true if what's passed to it is a space
		{
			if (parser_state == STATE_SPACE)
				exec_argv[exec_argc++] = &buffer[i];
			parser_state = STATE_NON_SPACE;
		}
		else
		{
			buffer[i] = '\0';
			parser_state = STATE_SPACE;
		}
	}

	/* If no command was given (empty line) the Shell just returns */
	if (!exec_argc)
		return 0;

	/* Terminate the list of exec parameters with NULL */
	exec_argv[exec_argc] = NULL;

	/* If Shell runs 'exit' it exits the program. */
	if (!strcmp(exec_argv[0], "exit"))
	{
		printf("Exiting process %d\n", getpid());
		exit(EXIT_SUCCESS); /* End Shell program */
	}
	else if (!strcmp(exec_argv[0], "!!"))
	{
		// Store the !! command in history
		strncpy(cmd_history[*command_counter % HISTORY_SIZE], original_command, SHELL_BUFFER_SIZE);
		(*command_counter)++;

		// Print command history
		for (int i = 0; i < HISTORY_SIZE && i < *command_counter; i++)
		{
			int idx = (*command_counter - i - 1) % HISTORY_SIZE;
			printf("%d. %s", (*command_counter - i - 1) % HISTORY_SIZE, cmd_history[idx]);
		}
		return 0;
	}
	else if (!strcmp(exec_argv[0], "cd") && exec_argc > 1)
	{
		/* Running 'cd' changes the Shell's working directory. */
		if (chdir(exec_argv[1]))
			/* Error: change directory failed */
			fprintf(stderr, "cd: failed to chdir %s\n", exec_argv[1]);

		// Store command in history
		strncpy(cmd_history[*command_counter % HISTORY_SIZE], original_command, SHELL_BUFFER_SIZE);
		(*command_counter)++;
		return 0;
	}
	else if (!strcmp(exec_argv[0], "shell"))
	{
		/* Check if we've reached the maximum allowed shell nesting level */
		if (shell_nesting_level >= MAX_SHELL_NESTING - 1)
		{
			fprintf(stderr, "Error: Maximum shell nesting level (%d) reached. Cannot create another subshell.\n", MAX_SHELL_NESTING);

			// Store command in history even though it failed
			strncpy(cmd_history[*command_counter % HISTORY_SIZE], original_command, SHELL_BUFFER_SIZE);
			(*command_counter)++;
			return 0;
		}

		/* Create a new subshell by forking the current process */
		pid_from_fork = fork();

		if (pid_from_fork < 0)
		{
			/* Error: fork() failed. */
			fprintf(stderr, "fork failed\n");
			return 0;
		}
		if (pid_from_fork == 0)
		{
			/* Child process - becomes the new shell */

			/* Increment the shell nesting level for the child */
			shell_nesting_level++;

			/* Reset command counter and history for the new shell */
			*command_counter = 0;
			for (int i = 0; i < HISTORY_SIZE; i++)
			{
				cmd_history[i][0] = '\0';
			}

			/* Return 1 to indicate that this is a new shell that should continue running */
			return 1;
		}
		else
		{
			/* Parent process - treats this like any external command */
			imtheparent(pid_from_fork, run_in_background);

			// Store command in history
			strncpy(cmd_history[*command_counter % HISTORY_SIZE], original_command, SHELL_BUFFER_SIZE);
			(*command_counter)++;
			return 0;
		}
	}
	else
	{
		/* Execute Commands */
		pid_from_fork = fork();

		if (pid_from_fork < 0)
		{
			/* Error: fork() failed. */
			fprintf(stderr, "fork failed\n");
			return 0;
		}
		if (pid_from_fork == 0)
		{
			return imthechild(exec_argv[0], &exec_argv[0]);
			/* Exit from function. */
		}
		else
		{
			imtheparent(pid_from_fork, run_in_background);
			// Store command in history
			strncpy(cmd_history[*command_counter % HISTORY_SIZE], original_command, SHELL_BUFFER_SIZE);
			(*command_counter)++;
			return 0;
		}
	}
	return 0;
}

int main(int argc, char **argv)
{
	pid_t shell_pid;
	int n_read, command_counter = 0;
	char buffer[SHELL_BUFFER_SIZE];
	char cmd_history[HISTORY_SIZE][SHELL_BUFFER_SIZE];

	/* Allow the Shell prompt to display the pid of this process */
	shell_pid = getpid();

	while (1)
	{
		fprintf(stdout, "Shell(pid=%d)[level:%d]%d> ", getpid(), shell_nesting_level, command_counter % HISTORY_SIZE);
		fflush(stdout);

		/* Read a line of input. */
		if (fgets(buffer, SHELL_BUFFER_SIZE, stdin) == NULL)
			return EXIT_SUCCESS;
		n_read = strlen(buffer);

		if (buffer[0] == '!' && n_read > 1 && isdigit(buffer[1]))
		{
			int history_index = atoi(&buffer[1]);

			// Check if the command exists in history
			if (history_index >= HISTORY_SIZE || (history_index >= command_counter) || cmd_history[history_index][0] == '\0')
			{
				fprintf(stderr, "Not valid\n");
				continue;
			}

			// Re-execute the command from history
			char temp_buffer[SHELL_BUFFER_SIZE];
			strncpy(temp_buffer, cmd_history[history_index], SHELL_BUFFER_SIZE);
			printf("Re-executing: %s", temp_buffer);
			if (parse_and_execute(temp_buffer, &command_counter, cmd_history))
			{
				// This is a new shell created by the 'shell' command - restart the loop
				continue;
			}
		}
		else
		{
			// Process regular command
			if (parse_and_execute(buffer, &command_counter, cmd_history))
			{
				// This is a new shell created by the 'shell' command - restart the loop
				continue;
			}
		}
	}

	return EXIT_SUCCESS;
} /* end main() */