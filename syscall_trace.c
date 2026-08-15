#include "types.h"
#include "stat.h"
#include "user.h"

int
main(int argc, char *argv[])
{
  if(argc < 2){
    printf(2, "usage: syscall_trace command [args...]\n");
    exit();
  }

  // Enable syscall tracing for this process.
  if(trace(1) < 0){
    printf(2, "syscall_trace: could not enable tracing\n");
    exit();
  }

  // Replace this process with the requested program.
  exec(argv[1], &argv[1]);

  // exec() only returns if it failed.
  printf(2, "syscall_trace: exec %s failed\n", argv[1]);

  exit();
}
