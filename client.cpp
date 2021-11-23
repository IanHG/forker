// Stdlib
#include <stddef.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
// Unix sockets
#include <sys/socket.h>
#include <sys/un.h>

int connect(const char* filename)
{
   int sockfd;
   sockaddr_un name;

   sockfd = socket(AF_LOCAL, SOCK_STREAM, 0);

   memset(&name, 0, sizeof(name));

   name.sun_family = AF_LOCAL;
   strncpy (name.sun_path, filename, sizeof (name.sun_path));
   name.sun_path[sizeof (name.sun_path) - 1] = '\0';
   
   int connect_status = -1;
   if(connect_status = connect(sockfd, (sockaddr*) &name, sizeof(name)) != 0)
   {
      perror("Connect failed");
   }
   
   //int status = fcntl(sockfd, F_SETFL, fcntl(sockfd, F_GETFL, 0) | O_NONBLOCK);
   
   return sockfd;
}

int main(int argc, char* argv[])
{
   char name[] = "/home/ian/programming/cpp/forker/test";

   int sockfd = connect(name);
   
   printf("Connecting with : %i\n", sockfd);

   char message[] = "uname -a";

   int write_size = write(sockfd, message, sizeof(message));

   printf("Client exiting.");
}
