/*
 * tsh - A tiny shell program with job control
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>

/* Misc manifest constants */
#define MAXLINE    1024   /* Max line size */
#define MAXARGS     128   /* Max args on a command line */
#define MAXJOBS      16   /* Max jobs at any point in time */

/* Job states */
#define UNDEF 0 /* Undefined */
#define FG 1    /* Running in foreground */
#define BG 2    /* Running in background */
#define ST 3    /* Stopped */

/*
 * Job state transitions and enabling actions:
 * - FG -> ST: ctrl-z
 * - ST -> FG: fg command
 * - ST -> BG: bg command
 * - BG -> FG: fg command
 * At most one job can be in the FG state.
 */

/* Global variables */
extern char **environ;      /* Defined in libc */
char prompt[] = "tsh> ";    /* Command line prompt (DO NOT CHANGE) */
int verbose = 0;            /* If true, print additional output */
char sbuf[MAXLINE];         /* Buffer for composing sprintf messages */

struct job_t {              /* Per-job data */
    pid_t pid;              /* Job PID */
    int jid;                /* Job ID [1, 2, ...] */
    int state;              /* UNDEF, FG, BG, or ST */
    char cmdline[MAXLINE];  /* Command line */
};
struct job_t jobs[MAXJOBS]; /* The job list */

volatile sig_atomic_t ready; /* Indicates if the newest child is in its process group */

/* End global variables */

/* Function prototypes */

/* Core shell functions to implement */
void eval(char *cmdline);
int builtin_cmd(char **argv);
void do_bgfg(char **argv);
void waitfg(pid_t pid);
void sigchld_handler(int sig);
void sigint_handler(int sig);
void sigtstp_handler(int sig);

/* Helper routines (provided) */
int parseline(const char *cmdline, char **argv);
void sigquit_handler(int sig);
void sigusr1_handler(int sig);

void clearjob(struct job_t *job);
void initjobs(struct job_t *jobs);
int freejid(struct job_t *jobs);
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline);
int deletejob(struct job_t *jobs, pid_t pid);
pid_t fgpid(struct job_t *jobs);
struct job_t *getjobpid(struct job_t *jobs, pid_t pid);
struct job_t *getjobjid(struct job_t *jobs, int jid);
int pid2jid(pid_t pid);
void listjobs(struct job_t *jobs);

void usage(void);
void unix_error(char *msg);
void app_error(char *msg);
typedef void handler_t(int);
handler_t *Signal(int signum, handler_t *handler);

/*
 * main - The shell's main routine
 */
int main(int argc, char **argv) {
    char c;
    char cmdline[MAXLINE];
    int emit_prompt = 1; /* Emit prompt (default) */

    /* Redirect stderr to stdout (so all output goes to stdout) */
    dup2(STDOUT_FILENO, STDERR_FILENO);

    /* Parse the command line */
    while ((c = getopt(argc, argv, "hvp")) != -1) {
        switch (c) {
            case 'h': /* Print help message */
                usage();
                break;
            case 'v': /* Emit additional diagnostic info */
                verbose = 1;
                break;
            case 'p': /* Don't print a prompt (useful for automated testing) */
                emit_prompt = 0;
                break;
            default:
                usage();
        }
    }

    /* Install the signal handlers */
    Signal(SIGUSR1, sigusr1_handler); /* Child process is ready */

    /* These are the handlers you will implement */
    Signal(SIGINT,  sigint_handler);  /* ctrl-c */
    Signal(SIGTSTP, sigtstp_handler); /* ctrl-z */
    Signal(SIGCHLD, sigchld_handler); /* Terminated or stopped child */

    /* This handler provides a clean way to terminate the shell */
    Signal(SIGQUIT, sigquit_handler);

    /* Initialize the job list */
    initjobs(jobs);

    /* Execute the shell's read/eval loop */
    while (1) {
        /* Read the command line */
        if (emit_prompt) {
            printf("%s", prompt);
            fflush(stdout);
        }
        if ((fgets(cmdline, MAXLINE, stdin) == NULL) && ferror(stdin))
            app_error("fgets error");
        if (feof(stdin)) { /* End of file (ctrl-d) */
            fflush(stdout);
            exit(0);
        }

        /* Evaluate the command line */
        eval(cmdline);
        fflush(stdout);
    }

    exit(0); /* Control never reaches here */
}

/*
 * eval - Evaluate the command line that the user has just typed in
 *
 * If the user has requested a built-in command (quit, jobs, bg, or fg),
 * then execute it immediately. Otherwise, fork a child process and
 * run the job in the context of the child. If the job is running in
 * the foreground, wait for it to terminate and then return. Note:
 * each child process must have a unique process group ID so that our
 * background children don't receive SIGINT (SIGTSTP) from the kernel
 * when we type ctrl-c (ctrl-z) at the keyboard.
 */
void eval(char *cmdline) {
    char *argv[MAXARGS];            /* Argument list for execve() */
    char buf[MAXLINE];              /* Holds modified command line */
    int bg;                         /* Should the job run in bg or fg? */
    pid_t pid;                      /* Process ID */
    sigset_t mask_all, mask_one, prev_one; /* Signal masks */
    int in_redir = 0, out_redir = 0; /* Redirection flags */
    char *in_file = NULL, *out_file = NULL; /* Redirection filenames */

    strcpy(buf, cmdline);
    bg = parseline(buf, argv);
    if (argv[0] == NULL) return; /* Ignore empty lines */

    /* Initialize signal blocking */
    sigfillset(&mask_all);
    sigemptyset(&mask_one);
    sigaddset(&mask_one, SIGCHLD);

    /* Check if the command is a built-in command */
    if (!builtin_cmd(argv)) {
        /* Handle redirection for non-built-in commands */
        for (int i = 0; argv[i] != NULL; i++) {
            if (strcmp(argv[i], "<") == 0) {
                in_redir = 1;
                in_file = argv[i + 1];
                argv[i] = NULL; /* Truncate the argv array here */
            } else if (strcmp(argv[i], ">") == 0) {
                out_redir = 1;
                out_file = argv[i + 1];
                argv[i] = NULL; /* Truncate the argv array here */
            }
        }

        /* Block SIGCHLD signals */
        sigprocmask(SIG_BLOCK, &mask_one, &prev_one);

        if ((pid = fork()) < 0) {
            unix_error("fork error"); /* Handle fork errors */
        } else if (pid == 0) { /* Child runs the user job */
            /* Unblock SIGCHLD signals */
            sigprocmask(SIG_SETMASK, &prev_one, NULL);
            setpgid(0, 0); /* Put the child in a new process group */

            /* Handle input redirection */
            if (in_redir) {
                int fd0 = open(in_file, O_RDONLY, 0);
                if (fd0 < 0) {
                    fprintf(stderr, "Error: Cannot open %s for input: %s\n", in_file, strerror(errno));
                    exit(1);
                }
                if (dup2(fd0, STDIN_FILENO) < 0) {
                    fprintf(stderr, "Error: dup2 failed for input redirection: %s\n", strerror(errno));
                    exit(1);
                }
                close(fd0);
            }

            /* Handle output redirection */
            if (out_redir) {
                int fd1 = open(out_file, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
                if (fd1 < 0) {
                    fprintf(stderr, "Error: Cannot open %s for output: %s\n", out_file, strerror(errno));
                    exit(1);
                }
                if (dup2(fd1, STDOUT_FILENO) < 0) {
                    fprintf(stderr, "Error: dup2 failed for output redirection: %s\n", strerror(errno));
                    exit(1);
                }
                close(fd1);
            }

            /* Execute the command */
            if (execvp(argv[0], argv) < 0) {
                fprintf(stderr, "%s: Command not found.\n", argv[0]);
                exit(1); /* Exit with an error code if command fails */
            }
        }
        // Parent adds the job to the job list before unblocking SIGCHLD
        if (!bg) {
            if (!addjob(jobs, pid, FG, cmdline)) {
                fprintf(stderr, "Failed to add job to the job list\n");
            }
        } else {
            if (!addjob(jobs, pid, BG, cmdline)) {
                fprintf(stderr, "Failed to add job to the job list\n");
            } else {
                printf("[%d] (%d) %s", pid2jid(pid), pid, cmdline);
            }
        }
        sigprocmask(SIG_UNBLOCK, &mask_one, NULL); // Unblock SIGCHLD

        // Parent waits for foreground job to terminate
        if (!bg) {
            waitfg(pid);
        }
    }
}

/*
 * parseline - Parse the command line and build the argv array.
 *
 * Characters enclosed in single quotes are treated as a single
 * argument. Return the number of arguments parsed.
 */
int parseline(const char *cmdline, char **argv) {
    static char array[MAXLINE]; /* Holds local copy of command line */
    char *buf = array;          /* Pointer traversing the command line */
    char *delim;                /* Points to space or quote delimiters */
    int argc;                   /* Number of arguments */

    strcpy(buf, cmdline);
    buf[strlen(buf) - 1] = ' '; /* Replace trailing '\n' with space */
    while (*buf && (*buf == ' ')) /* Ignore leading spaces */
        buf++;

    /* Build the argv list */
    argc = 0;
    if (*buf == '\'') {
        buf++;
        delim = strchr(buf, '\'');
    } else {
        delim = strchr(buf, ' ');
    }

    while (delim) {
        argv[argc++] = buf;
        *delim = '\0';
        buf = delim + 1;
        while (*buf && (*buf == ' ')) /* Ignore spaces */
            buf++;

        if (*buf == '\'') {
            buf++;
            delim = strchr(buf, '\'');
        } else {
            delim = strchr(buf, ' ');
        }
    }
    argv[argc] = NULL;

    return argc;
}

/*
 * builtin_cmd - If the user has typed a built-in command, execute
 * it immediately.
 */
int builtin_cmd(char **argv) {
    return 0; /* Not a built-in command */
}

/*
 * do_bgfg - Execute the built-in bg and fg commands
 */
void do_bgfg(char **argv) {
    return;
}

/*
 * waitfg - Block until process pid is no longer the foreground process
 */
void waitfg(pid_t pid) {
    sigset_t mask;
    sigemptyset(&mask); /* Create an empty signal set */

    sigdelset(&mask, SIGCHLD); /* Ensure that SIGCHLD isn't blocked */

    while (fgpid(jobs) == pid) {
        sigsuspend(&mask); /* Temporarily wait for SIGCHLD to be received */
    }
    // Note: sigsuspend restores the process's signal mask to its original state upon return
}

/*****************
 * Signal handlers
 *****************/

/*
 * sigchld_handler - The kernel sends a SIGCHLD to the shell whenever
 * a child job terminates (becomes a zombie) or stops due to SIGSTOP
 * or SIGTSTP. The handler reaps all available zombie children.
 */
void sigchld_handler(int sig) {
    return;
}

/*
 * sigint_handler - The kernel sends a SIGINT to the shell whenever the
 * user types ctrl-c at the keyboard. Catch it and send it to the
 * foreground job.
 */
void sigint_handler(int sig) {
    return;
}

/*
 * sigtstp_handler - The kernel sends a SIGTSTP to the shell whenever
 * the user types ctrl-z at the keyboard. Catch it and suspend the
 * foreground job by sending it a SIGTSTP.
 */
void sigtstp_handler(int sig) {
    return;
}

/*
 * sigusr1_handler - Child is ready
 */
void sigusr1_handler(int sig) {
    ready = 1;
}

/*********************
 * End signal handlers
 *********************/
/***********************************************
 * Helper routines that manipulate the job list
 ***********************************************/

/* clearjob - Clear the entries in a job struct */
void clearjob(struct job_t *job) {
    job->pid = 0;
    job->jid = 0;
    job->state = UNDEF;
    job->cmdline[0] = '\0';
}

/* initjobs - Initialize the job list */
void initjobs(struct job_t *jobs) {
    for (int i = 0; i < MAXJOBS; i++) {
        clearjob(&jobs[i]);
    }
}

/* freejid - Returns the smallest free job ID */
int freejid(struct job_t *jobs) {
    int taken[MAXJOBS + 1] = {0};
    for (int i = 0; i < MAXJOBS; i++) {
        if (jobs[i].jid != 0) {
            taken[jobs[i].jid] = 1;
        }
    }
    for (int i = 1; i <= MAXJOBS; i++) {
        if (!taken[i]) {
            return i;
        }
    }
    return 0;
}

/* addjob - Add a job to the job list */
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline) {
    if (pid < 1) return 0;

    int free = freejid(jobs);
    if (!free) {
        printf("Tried to create too many jobs\n");
        return 0;
    }

    for (int i = 0; i < MAXJOBS; i++) {
        if (jobs[i].pid == 0) {
            jobs[i].pid = pid;
            jobs[i].state = state;
            jobs[i].jid = free;
            strcpy(jobs[i].cmdline, cmdline);
            if (verbose) {
                printf("Added job [%d] %d %s\n", jobs[i].jid, jobs[i].pid, jobs[i].cmdline);
            }
            return 1;
        }
    }
    return 0; /* Suppress compiler warning */
}

/* deletejob - Delete a job whose PID=pid from the job list */
int deletejob(struct job_t *jobs, pid_t pid) {
    if (pid < 1) return 0;

    for (int i = 0; i < MAXJOBS; i++) {
        if (jobs[i].pid == pid) {
            clearjob(&jobs[i]);
            return 1;
        }
    }
    return 0;
}

/* fgpid - Return PID of the current foreground job, 0 if no such job */
pid_t fgpid(struct job_t *jobs) {
    for (int i = 0; i < MAXJOBS; i++) {
        if (jobs[i].state == FG) {
            return jobs[i].pid;
        }
    }
    return 0;
}

/* getjobpid - Find a job (by PID) on the job list */
struct job_t *getjobpid(struct job_t *jobs, pid_t pid) {
    if (pid < 1) return NULL;

    for (int i = 0; i < MAXJOBS; i++) {
        if (jobs[i].pid == pid) {
            return &jobs[i];
        }
    }
    return NULL;
}

/* getjobjid - Find a job (by JID) on the job list */
struct job_t *getjobjid(struct job_t *jobs, int jid) {
    if (jid < 1) return NULL;

    for (int i = 0; i < MAXJOBS; i++) {
        if (jobs[i].jid == jid) {
            return &jobs[i];
        }
    }
    return NULL;
}

/* pid2jid - Map process ID to job ID */
int pid2jid(pid_t pid) {
    if (pid < 1) return 0;

    for (int i = 0; i < MAXJOBS; i++) {
        if (jobs[i].pid == pid) {
            return jobs[i].jid;
        }
    }
    return 0;
}

/* listjobs - Print the job list */
void listjobs(struct job_t *jobs) {
    for (int i = 0; i < MAXJOBS; i++) {
        if (jobs[i].pid != 0) {
            printf("[%d] (%d) ", jobs[i].jid, jobs[i].pid);
            switch (jobs[i].state) {
                case BG:
                    printf("Running ");
                    break;
                case FG:
                    printf("Foreground ");
                    break;
                case ST:
                    printf("Stopped ");
                    break;
                default:
                    printf("listjobs: Internal error: job[%d].state=%d ", i, jobs[i].state);
            }
            printf("%s", jobs[i].cmdline);
        }
    }
}

/******************************
 * End job list helper routines
 ******************************/

/***********************
 * Other helper routines
 ***********************/

/*
 * usage - Print a help message and terminate
 */
void usage(void) {
    printf("Usage: shell [-hvp]\n");
    printf("   -h   Print this message\n");
    printf("   -v   Print additional diagnostic information\n");
    printf("   -p   Do not emit a command prompt\n");
    exit(1);
}

/*
 * unix_error - Unix-style error routine
 */
void unix_error(char *msg) {
    fprintf(stdout, "%s: %s\n", msg, strerror(errno));
    exit(1);
}
/*
 * app_error - Application-style error routine
 */
void app_error(char *msg) {
    fprintf(stdout, "%s\n", msg);
    exit(1);
}

/*
 * Signal - Wrapper for the sigaction function
 */
handler_t *Signal(int signum, handler_t *handler) {
    struct sigaction action, old_action;

    action.sa_handler = handler;
    sigemptyset(&action.sa_mask); /* Block signals of the type being handled */
    action.sa_flags = SA_RESTART; /* Restart system calls if possible */

    if (sigaction(signum, &action, &old_action) < 0) {
        unix_error("Signal error");
    }
    return old_action.sa_handler;
}

/*
 * sigquit_handler - The driver program can gracefully terminate the
 * child shell by sending it a SIGQUIT signal.
 */
void sigquit_handler(int sig) {
    printf("Terminating after receipt of SIGQUIT signal\n");
    exit(1);
}
