// Stdlib
#include <stddef.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <getopt.h>
#include <fcntl.h>
// Unix sockets
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
// Pthreads
#include <pthread.h>

#define USE_SPLICE

/**
 * Some global settings and variables. These should mainly be used by the master thread.
 **/
struct {
   const char* program;
   const char* socket;
   int         verbose;
   int         num_threads;
   pthread_t*  threads;
} global;

/**
 * Print usage help message.
 * Will exit the program.
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
 * Parse command line options.
 * Called on master thread.
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
         {"num-threads", required_argument, 0, 'n'},
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
         
         case 'n':
            printf ("option -s with value `%s'\n", optarg);
            global.num_threads = strtol(optarg, NULL, 0);
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
 * Make a named local socket.
 *
 * Called on master thread to create a local UNIX socket,
 * which clients can connect to with requests.
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
int fork_exec(char** argv, char* dir, int sockfd)
{
   int status = 0;
   pid_t pid;
   
   int pipes[1][2];
                   
   // pipes for parent to write and read
   int pip0 = pipe(pipes[0]);

   if( (pid = fork()) < 0 )
   {
      status = -1;
   }
   else if(!pid) 
   {
      /* Make stdout print to write pipe */
      if(dup2(pipes[0][1], fileno(stdout)) == -1)
      {
         printf(" CHILD_READ_FD DUP ERROR ");
         abort();
      }
      
      /* Close pipes as child process should not know of these */
      close(pipes[0][0]);
      close(pipes[0][1]);

      /* Change directory if requested */
      if(dir)
      {
         if(chdir(dir) == -1)
         {
            printf("Could not chdir\n");
         }
      }

      /* Make call */
      if(execvp(argv[0], argv) == -1)
      {
         /* Call did not succeed */
         printf("Could not start process\n");
      }

      /* Make sure childs always exit if execvp fails */
      _Exit(1); /* We use "_Exit" as it will not interfere with parents atexit handlers */
   } 
   else 
   {
      close(pipes[0][1]); /* Close child write pipe, such that it is not kept alive by parent 
                           * as this would mean the splice (read/write) loop would run forever
                           */

      char buffer[1024];
      int read_bytes;
      
      /* Transfer output to client */
#ifdef USE_SPLICE
      /* Using splice function will do it in kernel space, which is more optimal,
       * but splice is only present in newer kernels 
       */
      while(read_bytes = splice(pipes[0][0], NULL, sockfd, NULL, 1024 - 1, 0))
      {
         printf("Read bytes %i\n", read_bytes);
      }
#else
      /* Fallback to read/write loop if splice is not available */
      while(read_bytes = read(pipes[0][0], buffer, 1024))
      {
         printf("Read bytes %i\n", read_bytes);
         write(sockfd, buffer, read_bytes);
      }
#endif /* USE_SPLICE */
      
      close(pipes[0][0]);

      /* Loop over waitpid to wait for child */
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
 *
 **/
int handle_connection(int sockfd)
{
   int status = 0;

   #define buffer_capacity 1024
   char buffer[buffer_capacity];
   int read_bytes;

   if(read_bytes = read(sockfd, buffer, buffer_capacity) != buffer_capacity)
   {
      /* Get directory */
      int   cmd_len = strlen(buffer);
      char* dir     = buffer + cmd_len + 1;

      /* Create argv for exec() from buffer */
      char* cptr = buffer;
      char* pch  = strchr(cptr, ' ');
      int count = strlen(cptr) != 0 ? 2 : 1;
      while (pch != NULL)
      {
         pch = strchr(pch + 1, ' ');
         ++count;
      }
      
      char** argv = (char**) malloc(sizeof(char**) * count);
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
      if(fork_exec(argv, dir, sockfd) != 0)
      {
         printf("Could not fork()+exec()\n");
      }
      
      /* Clean-up */
      free(argv);
   }
   else
   {
      printf("Command could not fit in buffer\n");
   }

   return status;
}


/**
 *
 **/
void listen_on_socket(int sockfd)
{
   socklen_t clilen;
   struct sockaddr_un cliaddr;
	int connfd;

   while(true)
   {
      clilen = sizeof(cliaddr);
      connfd = accept(sockfd, (sockaddr*) &cliaddr, &clilen);

      printf("Connection accepted: %i\n", connfd);
      int status = handle_connection(connfd);

      close(connfd);
   }
}

/**
 *
 **/
void* pthread_listen_on_socket(void* arg)
{
   listen_on_socket(* (int*) arg);
   return NULL;
}

/**
 *
 **/
int main(int argc, char* argv[])
{
   /* Input processing */
   global.program     = argv[0];
   global.socket      = NULL;
   global.verbose     = 0;
   global.num_threads = 1;
   global.threads     = NULL;

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
   int sockfd = make_named_socket(global.socket);
   
   if(listen(sockfd, 1024) != 0) /* 1024 connections allowed */
   {
      perror("Could not listen on socket.");
      exit(EXIT_FAILURE);
   }
   
   /* Create threads listening on socket which will handle any requests */
   global.threads = (pthread_t*)malloc(global.num_threads * sizeof(pthread_t));

   for(int i = 0; i < global.num_threads; ++i)
   {
      pthread_create(&global.threads[i], NULL, pthread_listen_on_socket, (void *) &sockfd);
   }

   for(int i = 0; i < global.num_threads; ++i)
   {
      pthread_join(global.threads[i], NULL);
   }
   
   /* Clean-up */
   free(global.threads);
   close(sockfd);
   unlink(global.socket); /* Clean up the local socket/fd/file */

   return EXIT_SUCCESS;
}
