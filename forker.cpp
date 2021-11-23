// C++ (maybe gets removed)
#include <memory>
// Stdlib
#include <stddef.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <getopt.h>
// Unix sockets
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>

/**
 *
 **/
struct {
   const char* program;
   const char* socket;
   int         verbose;
} global;


/**
 *
 **/
int fork_exec(char* argv[])
{
   int status = 0;
   pid_t pid;

   if( (pid = fork()) < 0 )
   {
      status = -1;
   }
   else if(!pid) 
   {
      // make call
      if(execvp(argv[0], argv) == -1)
      {
         // error handling
         printf(" could not start process ");
      }
   } 
   else 
   {
      // Loop over waitpid to wait for child
      while(waitpid(pid, &status, 0) < 0)
      {
         if(errno != EINTR)
         {
            status = -1;
            break;
         }
         
         sched_yield();
      }
   }

   return status;
}

/**
 * Make a named local socket.
 **/
int make_named_socket (const char *filename)
{
   struct sockaddr_un name;
   int sock;
   size_t size;

   /* Create the socket. */
   sock = socket (AF_LOCAL, SOCK_STREAM, 0);
   if (sock < 0)
   {
      perror ("socket");
      exit (EXIT_FAILURE);
   }

   /* Bind a name to the socket. */
   name.sun_family = AF_LOCAL;
   strncpy (name.sun_path, filename, sizeof (name.sun_path));
   name.sun_path[sizeof (name.sun_path) - 1] = '\0';
   
   if (bind (sock, (struct sockaddr *) &name, SUN_LEN(&name)) < 0)
   {
      perror ("bind");
      exit (EXIT_FAILURE);
   }
   
   /* Return socket */
   return sock;
}

/**
 *
 **/
int handle_connection(int sockfd)
{
   #define buffer_capacity 1024
   char buffer[buffer_capacity];
   int read_bytes;

   while((read_bytes = read(sockfd, buffer, buffer_capacity)))
   {
      /* Create argv for exec() from buffer */
      char* cptr = buffer;
      char* pch  = strchr(cptr, ' ');
      int count = strlen(cptr) != 0 ? 2 : 1;
      while (pch != NULL)
      {
         pch = strchr(pch + 1, ' ');
         ++count;
      }
      
      std::unique_ptr<char*[]> argv{new char*[count]};
      argv[0] = cptr;
      argv[count - 1] = nullptr;
      count = 1;
      pch  = strchr(cptr, ' ');
      while (pch != NULL)
      {
         *pch = '\0';
         argv[count] = pch + 1;
         pch = strchr(pch + 1, ' ');
         ++count;
      }
      
      /* Call fork()+exec() */
      if(fork_exec(argv.get()) != 0)
      {
         printf("Could not fork()+exec()");
      }
   }
}

/**
 *
 **/
void print_usage (FILE* stream, int exit_code)
{
  fprintf (stream, "Usage:  %s [options ...]\n", global.program);
  fprintf (stream,
           "  -h  --help              Display this usage information.\n"
           "  -s  --socket <filename> Socket to use for communication.\n"
           "  -v  --verbose           Print verbose messages.\n");
  exit (exit_code);
}

/**
 *
 **/
int parse_command_line(int argc, char* argv[])
{
   int c;

   while (1)
   {
      static struct option long_options[] =
      {
         /* These options set a flag. */
         {"verbose", no_argument,       &global.verbose, 1},
         /* These options don’t set a flag.
            We distinguish them by their indices. */
         {"help",    no_argument,       0, 'h'},
         {"socket",  required_argument, 0, 's'},
         {0, 0, 0, 0}
      };

      /* getopt_long stores the option index here. */
      int option_index = 0;

      c = getopt_long (argc, argv, "hs:",
            long_options, &option_index);

      /* Detect the end of the options. */
      if (c == -1)
         break;

      switch (c)
      {
         case 0:
            /* If this option set a flag, do nothing else now. */
            if (long_options[option_index].flag != 0)
               break;
            printf ("option %s", long_options[option_index].name);
            if (optarg)
               printf (" with arg %s", optarg);
            printf ("\n");
            break;

         case 'h':
            print_usage(stdout, 0);

         case 's':
            printf ("option -s with value `%s'\n", optarg);
            global.socket = optarg;
            break;

         case '?':
            /* getopt_long already printed an error message. */
            break;

         default:
            abort ();
      }
   }

   /* Instead of reporting ‘--verbose’
      and ‘--brief’ as they are encountered,
      we report the final status resulting from them. */
   if (global.verbose)
      printf("verbose flag is set");

   /* Print any remaining command line arguments (not options). */
   if (optind < argc)
   {
      printf ("non-option ARGV-elements: ");
      while (optind < argc)
         printf ("%s ", argv[optind++]);
      putchar ('\n');
   }

   return 0;
}

/**
 *
 **/
int main(int argc, char* argv[])
{
   /* Input processing */
   global.program = argv[0];
   global.socket  = NULL;
   global.verbose = 0;

   if(parse_command_line(argc, argv) != 0)
   {
      printf("Could not parse command line.\n");
      return EXIT_FAILURE;
   }
   if(!global.socket)
   {
      printf("No socket name provided.\n");
      return EXIT_FAILURE;
   }
   
   /* Create local socket for listening */
   unlink(global.socket); /* Will remove the local socket/fd/file if it exists */
   int sock = make_named_socket(global.socket);
   
   if(listen(sock, 1024) != 0) /* 1024 connections allowed */
   {
      perror("Could not listen on socket.");
      exit(EXIT_FAILURE);
   }
   
   /* Listen on socket and handle any requests */
   socklen_t clilen;
   struct sockaddr_un cliaddr;
	int connfd;

   while(true)
   {
      clilen = sizeof(cliaddr);
      connfd = accept(sock, (sockaddr*) &cliaddr, &clilen);

      printf("Connection accepted: %i\n", connfd);
      handle_connection(connfd);

      close(connfd);
   }
   
   /* Clean-up */
   close(sock);
   unlink(global.socket); /* Clean up the local socket/fd/file */
}
