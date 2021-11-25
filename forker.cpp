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
#include <signal.h>
// Unix sockets
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
// Pthreads
#include <pthread.h>

/**
 * Use "splice()" function. If splice is not available, comment this out,
 * and program will use a read/write loop instead.
 **/
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
   sigset_t    sigset;
} global;

/**
 * Print usage help message.
 * Will exit the program.
 **/
void print_usage (FILE* stream, int exit_code)
{
  fprintf (stream, "Usage:  %s [options ...]\n", global.program);
  fprintf (stream, "\n"
                   "  Sending USR1 signal to a running %s, will cause a safe shutdown.\n"
                   "\n"
                 , global.program 
          );
  fprintf (stream,
           "  -h  --help              Display this usage information.\n"
           "  -s  --socket <filename> Socket to use for communication.\n"
           "  -n  --num-threads       Number of threads for handling requests (default: 1).\n"
           "  -v  --verbose           Print verbose messages.\n"
           );
  exit (exit_code);
}

/**
 * Parse command line options.
 * Called on master thread.
 *
 * Based on this example:
 *    https://www.gnu.org/software/libc/manual/html_node/Getopt-Long-Option-Example.html
 **/
int parse_command_line(int argc, char* argv[])
{
   int c;
   
   while (1)
   {
      /* Define command-line options to be parsed with getopt_long */
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
 * Handle fork() + exec(), for an argv array, and
 * an optional directory, dir, to run in.
 *
 * Will send all output from the exec'ed program back on 
 * the socket defined by sockfd.
 * This can be done with "splice" (by defining USE_SPLICE macro),
 * or a basic "read/write" loop, if "splice is not available.
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
         _Exit(1); /* We use "_Exit" as it will not interfere with parents atexit handlers */
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
            _Exit(1); /* We use "_Exit" as it will not interfere with parents atexit handlers */
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
 * Handle a connection. 
 * 
 * Will read the command and directory sent from the client,
 * do some preparation and call fork_exec, which will handle fork() + exec().
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
         status = -1;
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
 * Listen on a listen socket, and handle connections.
 *
 * Will check the socket for any connections, 
 * handle the connection, and close the connection again.
 * 
 * Called on each thread after the master thread has 
 * opened the socket and set it up for listening.
 **/
void listen_on_socket(int sockfd)
{
   socklen_t clilen;
   struct sockaddr_un cliaddr;
	int connfd;
   
   /* Listening loop */
   while(true)
   {
      /* Accept connection */
      clilen = sizeof(cliaddr);
      connfd = accept(sockfd, (sockaddr*) &cliaddr, &clilen);
   
      /* Check for shutdown */
      if ( connfd < 0 )
      {
         if( errno == EINVAL )
         {
            break;
         }
      }
      
      /* Handle connection */
      int status = handle_connection(connfd);
      close(connfd);
   }
}

/**
 * Listen on socket wrapper function for creating Posix-threads.
 **/
void* pthread_listen_on_socket(void* arg)
{
   /* Call actual listening function */
   listen_on_socket(* (int*) arg);

   return NULL;
}

/**
 * Handle SIGUSR1 on master thread.
 *
 * When signal is caught, master thread will
 * shutdown the UNIX socket, which will make all 
 * threads exit, and cause the program to safely shut down
 * in a clean way.
 **/
int handle_signal(int sockfd)
{
   int s, sig;
   
   /* Wait for SIGUSR1 */
   s = sigwait(&global.sigset, &sig);
   if (s != 0)
   {
      return 1;
   }
   
   /* When SIGUSR1 is sent, shutdown listening socket.
    * This will make threads exit as well 
    */
   shutdown(sockfd, SHUT_RDWR);

   return 0;
}

/**
 * Main of forker program.
 **/
int main(int argc, char* argv[])
{
   /* Input processing */
   global.program     = argv[0];
   global.socket      = NULL;
   global.verbose     = 0;
   global.num_threads = 1;
   global.threads     = NULL;
   
   /* Parse command-line */
   if(parse_command_line(argc, argv) != 0)
   {
      perror("Could not parse command line.\n");
      return EXIT_FAILURE;
   }
   if(!global.socket)
   {
      perror("No socket name provided.\n");
      return EXIT_FAILURE;
   }
   
   /* Block SIGUSR1 on ALL threads (including master)
    * This signal will be handled by master in a special function
    */
   sigemptyset (&global.sigset);
   sigaddset   (&global.sigset, SIGUSR1);
   int s;
   if((s = pthread_sigmask(SIG_BLOCK, &global.sigset, NULL)) != 0)
   {
      perror("Could not mask out SIGUSR1.\n");
      return EXIT_FAILURE;
   }

   /* Create local socket for listening */
   unlink(global.socket); /* Will remove the local socket/fd/file if it exists */
   int sockfd = make_named_socket(global.socket);
   
   if(listen(sockfd, 1024) != 0) /* 1024 connections allowed */
   {
      perror("Could not listen on created UNIX socket.\n");
      return EXIT_FAILURE;
   }
   
   /* Create threads listening on socket which will handle any requests */
   global.threads = (pthread_t*)malloc(global.num_threads * sizeof(pthread_t));

   for(int i = 0; i < global.num_threads; ++i)
   {
      pthread_create(&global.threads[i], NULL, pthread_listen_on_socket, (void *) &sockfd);
   }
   
   if(handle_signal(sockfd) != 0)
   {
      return EXIT_FAILURE;
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
