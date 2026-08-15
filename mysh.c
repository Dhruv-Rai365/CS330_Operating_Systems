#include "types.h"
#include "stat.h"
#include "user.h"

/*
 * mysh - Minimal process-chaining shell for xv6
 *
 * Supported:
 *   1. Normal command execution
 *        echo hello
 *
 *   2. Arbitrary pipelines
 *        ls | wc
 *        cat README | grep xv6 | wc
 *
 * Not supported intentionally:
 *   - file redirection (<, >)
 *   - background jobs (&)
 *   - command history
 *   - environment variables
 *   - quoting
 *   - built-in cd
 *
 * The goal is to demonstrate:
 *   fork(), exec(), pipe(), dup(), close(), and wait().
 */

#define MAX_INPUT     512
#define MAX_COMMANDS    8
#define MAX_ARGS       16

/*
 * Each pipeline stage is represented as one command.
 *
 * Example:
 *
 *   cat README | grep xv6 | wc
 *
 * becomes:
 *
 *   commands[0].argv = {"cat", "README", 0}
 *   commands[1].argv = {"grep", "xv6", 0}
 *   commands[2].argv = {"wc", 0}
 */
struct command {
  char *argv[MAX_ARGS];
};


/*
 * Return 1 if c is whitespace that can separate arguments.
 *
 */
static int
is_space(char c)
{
  return c == ' ' ||
         c == '\t' ||
         c == '\r' ||
         c == '\n';
}


/*
 * Parse one input line into pipeline stages.
 *
 * Example input:
 *
 *   "cat README | grep xv6 | wc"
 *
 * The function modifies the input buffer itself by replacing
 * spaces and '|' characters with '\0'.
 *
 * Result:
 *
 *   cmds[0] -> cat README
 *   cmds[1] -> grep xv6
 *   cmds[2] -> wc
 *
 * Return value:
 *
 *    > 0 : number of commands
 *      0 : empty input
 *     -1 : invalid command / too many arguments
 */
static int
parse_pipeline(char *line, struct command cmds[])
{
  int cmd_index;
  int argc;
  char *p;
  char delimiter;

  cmd_index = 0;
  argc = 0;
  p = line;

  /*
   * Start with an empty command table.
   */
  memset(cmds, 0, sizeof(struct command) * MAX_COMMANDS);

  while(1){

    /*
     * Skip spaces before the next argument or pipe.
     */
    while(is_space(*p))
      p++;

    /*
     * End of the input line.
     */
    if(*p == '\0')
      break;

    /*
     * Handle a pipe that appears after whitespace:
     *
     *   ls | wc
     *
     * When we see '|', the current command is complete.
     */
    if(*p == '|'){

      /*
       * A pipe without a command on its left is invalid:
       *
       *   | wc
       *   ls || wc
       */
      if(argc == 0)
        return -1;

      /*
       * argv arrays passed to exec() must end with NULL.
       */
      cmds[cmd_index].argv[argc] = 0;

      cmd_index++;

      /*
       * If another command follows, we need space for it.
       */
      if(cmd_index >= MAX_COMMANDS)
        return -1;

      argc = 0;
      p++;

      continue;
    }

    /*
     * Leave one argv slot for the terminating NULL pointer.
     */
    if(argc >= MAX_ARGS - 1)
      return -1;

    /*
     * The beginning of the current word becomes argv[argc].
     *
     * For:
     *
     *   echo hello
     *
     * we first store a pointer to "echo", then one to "hello".
     */
    cmds[cmd_index].argv[argc++] = p;

    /*
     * Move until we hit:
     *
     *   whitespace
     *   pipe
     *   end of string
     */
    while(*p != '\0' &&
          !is_space(*p) &&
          *p != '|')
      p++;

    /*
     * Remember what terminated the word before replacing it
     * with '\0'.
     */
    delimiter = *p;

    if(*p != '\0'){
      *p = '\0';
      p++;
    }

    /*
     * This handles pipelines written without spaces:
     *
     *   ls|wc
     *
     * because the pipe itself terminated the previous word.
     */
    if(delimiter == '|'){

      cmds[cmd_index].argv[argc] = 0;

      cmd_index++;

      if(cmd_index >= MAX_COMMANDS)
        return -1;

      argc = 0;
    }
  }

  /*
   * If argc is zero after at least one completed command,
   * the input ended immediately after a pipe:
   *
   *   ls |
   *
   * which is invalid.
   */
  if(argc == 0){
    if(cmd_index == 0)
      return 0;

    return -1;
  }

  /*
   * Terminate the argv array of the final command.
   */
  cmds[cmd_index].argv[argc] = 0;

  return cmd_index + 1;
}


/*
 * Execute all pipeline stages.
 *
 * For:
 *
 *   cmd1 | cmd2 | cmd3
 *
 * we construct:
 *
 *          pipe 1          pipe 2
 *
 *   cmd1 ----------> cmd2 ----------> cmd3
 *
 * Each command runs in a separate child process.
 */
static int
run_pipeline(struct command cmds[], int ncmd)
{
  int i;
  int pid;
  int pipefd[2];

  /*
   * File descriptor containing the read end of the
   * previous pipeline.
   *
   * -1 means there is no previous pipe.
   */
  int previous_read;

  int children;

  previous_read = -1;
  children = 0;

  for(i = 0; i < ncmd; i++){

    /*
     * Every command except the final command needs a new pipe
     * connecting its stdout to the next command's stdin.
     */
    if(i < ncmd - 1){

      if(pipe(pipefd) < 0){
        printf(2, "mysh: pipe creation failed\n");

        if(previous_read >= 0)
          close(previous_read);

        /*
         * Reap any children that were already created.
         */
        while(children > 0){
          wait();
          children--;
        }

        return -1;
      }
    }

    /*
     * Create a new process for this command.
     */
    pid = fork();

    if(pid < 0){

      printf(2, "mysh: fork failed\n");

      if(previous_read >= 0)
        close(previous_read);

      if(i < ncmd - 1){
        close(pipefd[0]);
        close(pipefd[1]);
      }

      while(children > 0){
        wait();
        children--;
      }

      return -1;
    }


    /*
     * ---------------- CHILD PROCESS ----------------
     */
    if(pid == 0){

      /*
       * If this is not the first command, its stdin should
       * come from the previous pipe.
       *
       * xv6 does not provide dup2().
       *
       * Instead:
       *
       *   close(0)
       *   dup(previous_read)
       *
       * works because dup() returns the lowest available
       * file descriptor. After closing fd 0, that lowest
       * descriptor is stdin (0).
       */
      if(previous_read >= 0){

        close(0);

        if(dup(previous_read) < 0){
          printf(2, "mysh: dup stdin failed\n");
          exit();
        }
      }


      /*
       * If this is not the last command, redirect stdout
       * into the write end of the new pipe.
       *
       * Again:
       *
       *   close(1)
       *   dup(pipefd[1])
       *
       * causes the duplicate to become stdout (fd 1).
       */
      if(i < ncmd - 1){

        close(1);

        if(dup(pipefd[1]) < 0){
          printf(2, "mysh: dup stdout failed\n");
          exit();
        }
      }


      /*
       * The descriptors have already been duplicated into
       * stdin/stdout, so the original descriptors are no
       * longer needed by this child.
       */
      if(previous_read >= 0)
        close(previous_read);

      if(i < ncmd - 1){
        close(pipefd[0]);
        close(pipefd[1]);
      }


      /*
       * Replace this child process with the requested program.
       *
       * Example:
       *
       *   cmds[i].argv = {"grep", "xv6", 0}
       *
       * becomes:
       *
       *   exec("grep", argv)
       *
       * exec() does NOT create another process.
       * It replaces this child's address space with grep.
       */
      exec(cmds[i].argv[0], cmds[i].argv);


      /*
       * exec() returns only when execution failed.
       */
      printf(2, "mysh: exec %s failed\n",
             cmds[i].argv[0]);

      exit();
    }


    /*
     * ---------------- PARENT PROCESS ----------------
     *
     * The parent shell does not execute the command.
     * It manages the pipes and keeps creating the remaining
     * processes in the pipeline.
     */

    children++;


    /*
     * Once the next process has inherited the previous pipe,
     * the shell itself no longer needs that read descriptor.
     */
    if(previous_read >= 0)
      close(previous_read);


    if(i < ncmd - 1){

      /*
       * The shell never writes command data into the pipe,
       * so it must close its copy of the write end.
       *
       * This is important: keeping a write end open can
       * prevent the reader from ever seeing EOF.
       */
      close(pipefd[1]);

      /*
       * Save the read end.
       *
       * On the next loop iteration, it becomes stdin for
       * the next command.
       */
      previous_read = pipefd[0];

    } else {

      previous_read = -1;
    }
  }


  /*
   * Wait until every process in the pipeline finishes.
   */
  for(i = 0; i < children; i++)
    wait();

  return 0;
}


int
main(int argc, char *argv[])
{
  char input[MAX_INPUT];
  struct command cmds[MAX_COMMANDS];
  int ncmd;

  /*
   * argc/argv are unused because mysh is interactive.
   */
  (void)argc;
  (void)argv;

  printf(1, "mysh: minimal xv6 process-chaining shell\n");

  while(1){

    /*
     * Display our shell prompt.
     */
    printf(1, "mysh> ");

    memset(input, 0, sizeof(input));

    /*
     * Read one line from stdin.
     */
    gets(input, sizeof(input));

    /*
     * Empty input can indicate EOF.
     */
    if(input[0] == '\0')
      break;


    /*
     * Turn the command line into pipeline stages.
     */
    ncmd = parse_pipeline(input, cmds);

    if(ncmd < 0){
      printf(2, "mysh: invalid command\n");
      continue;
    }

    if(ncmd == 0)
      continue;


    /*
     * Small built-in command so we can leave mysh and
     * return to xv6's normal shell.
     *
     * We intentionally do not implement other shell builtins.
     */
    if(ncmd == 1 &&
       cmds[0].argv[0] != 0 &&
       strcmp(cmds[0].argv[0], "exit") == 0){
      break;
    }


    /*
     * Execute either:
     *
     *   one normal command
     *
     * or
     *
     *   an N-stage pipeline.
     */
    run_pipeline(cmds, ncmd);
  }

  exit();
}
