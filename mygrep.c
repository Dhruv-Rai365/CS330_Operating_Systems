#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"

#define MAXLINE 1024

// Returns 1 if pattern occurs anywhere inside text.
int
contains(char *text, char *pattern)
{
  int i, j;

  if(pattern[0] == '\0')
    return 1;

  for(i = 0; text[i] != '\0'; i++){
    for(j = 0;
        pattern[j] != '\0' && text[i+j] == pattern[j];
        j++)
      ;

    if(pattern[j] == '\0')
      return 1;
  }

  return 0;
}

int
main(int argc, char *argv[])
{
  int fd;
  int n;
  int len = 0;
  char c;
  char line[MAXLINE];

  if(argc != 3){
    printf(2, "usage: mygrep pattern file\n");
    exit();
  }

  fd = open(argv[2], O_RDONLY);

  if(fd < 0){
    printf(2, "mygrep: cannot open %s\n", argv[2]);
    exit();
  }

  while((n = read(fd, &c, 1)) > 0){

    // Store character if the line fits in our buffer.
    if(len < MAXLINE - 1)
      line[len++] = c;

    if(c == '\n'){
      line[len] = '\0';

      if(contains(line, argv[1]))
        write(1, line, len);

      len = 0;
    }
  }

  // Handle final line if file does not end with '\n'.
  if(len > 0){
    line[len] = '\0';

    if(contains(line, argv[1]))
      write(1, line, len);
  }

  close(fd);
  exit();
}
