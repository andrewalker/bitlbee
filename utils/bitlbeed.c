/****************************************************************\
*                                                                *
*  bitlbeed.c                                                    *
*                                                                *
*  A tiny daemon to allow you to run The Bee as a non-root user  *
*  (without access to /etc/inetd.conf or whatever)               *
*                                                                *
*  Copyright 2002 Wilmer van der Gaast <lintux@debian.org>       *
*                                                                *
*  Licensed under the GNU General Public License                 *
*                                                                *
\****************************************************************/

/* 
   ChangeLog:
   
   2003-12-26: Added the SO_REUSEADDR sockopt, logging and CPU-time limiting
               for clients using setrlimit(), fixed the execv() call
   2002-11-29: Added the timeout so old child processes clean up faster
   2002-11-28: First version
*/

#define SELECT_TIMEOUT 2
#define MAX_LOG_LEN 128

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <stdarg.h>
#include <time.h>

#include <sys/wait.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>

#include <netinet/in.h>
#include <arpa/inet.h>

typedef struct settings
{
	char *interface;
	signed int port;
	
	unsigned char max_conn;
	int seconds;
	
	char **call;
} settings_t;

FILE *logfile;

settings_t *set_load( int argc, char *argv[] );
void log( char *fmt, ... );

int main( int argc, char *argv[] )
{
	const int rebind_on = 1;
	settings_t *set;
	
	int serv_fd, serv_len;
	struct sockaddr_in serv_addr;
	
	pid_t st;
	
	if( !( set = set_load( argc, argv ) ) )
		return( 1 );
	
	if( !logfile )
		if( !( logfile = fopen( "/dev/null", "w" ) ) )
		{
			perror( "fopen" );
			return( 1 );
		}
	
	fcntl( fileno( logfile ), F_SETFD, FD_CLOEXEC );
	
	serv_fd = socket( AF_INET, SOCK_STREAM, 6 );
	setsockopt( serv_fd, SOL_SOCKET, SO_REUSEADDR, &rebind_on, sizeof( rebind_on ) );
	fcntl( serv_fd, F_SETFD, FD_CLOEXEC );
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = inet_addr( set->interface );
	serv_addr.sin_port = htons( set->port );
	serv_len = sizeof( serv_addr );
	
	if( bind( serv_fd, (struct sockaddr *) &serv_addr, serv_len ) != 0 )
	{
		perror( "bind" );
		return( 1 );
	}
	
	if( listen( serv_fd, set->max_conn ) != 0 )
	{
		perror( "listen" );
		return( 1 );
	}
	
	st = fork();
	if( st < 0 )
	{
		perror( "fork" );
		return( 1 );
	}
	else if( st > 0 )
	{
		return( 0 );
	}
	
	setsid();
	close( 0 );
	close( 1 );
	close( 2 );
	
	log( "bitlbeed running" );
	
	/* The Daemon */
	while( 1 )
	{
		int cli_fd, cli_len, i, st;
		struct sockaddr_in cli_addr;
		pid_t child;
		
		static int running = 0;
		
		fd_set rd;
		struct timeval tm;
		
		/* accept() only returns after someone connects. To clean up old
		   processes (by running waitpid()) it's better to use select()
		   with a timeout. */
		FD_ZERO( &rd );
		FD_SET( serv_fd, &rd );
		tm.tv_sec = SELECT_TIMEOUT;
		tm.tv_usec = 0;
		if( select( serv_fd + 1, &rd, NULL, NULL, &tm ) > 0 )
		{
			cli_len = sizeof( cli_addr );
			cli_fd = accept( serv_fd, (struct sockaddr *) &cli_addr, &cli_len );
			
			/* We want this socket on stdout and stderr too! */
			dup( cli_fd ); dup( cli_fd );
			
			if( ( child = fork() ) == 0 )
			{
				if( set->seconds )
				{
					struct rlimit li;
					
					li.rlim_cur = (rlim_t) set->seconds;
					li.rlim_max = (rlim_t) set->seconds + 1;
					setrlimit( RLIMIT_CPU, &li );
				}
				execv( set->call[0], set->call );
				log( "Error while executing %s!", set->call[0] );
				return( 1 );
			}
			else
			{
				running ++;
				close( 0 );
				close( 1 );
				close( 2 );
				
				log( "Started child process for client %s (PID=%d), got %d clients now", inet_ntoa( cli_addr.sin_addr ), child, running );
			}
		}
		
		/* If the max. number of connection is reached, don't accept
		   new connections until one expires -> Not always WNOHANG
		   
		   Cleaning up child processes is a good idea anyway... :-) */
		while( ( i = waitpid( 0, &st, ( ( running < set->max_conn ) || ( set->max_conn == 0 ) ) ? WNOHANG : 0 ) ) > 0 )
		{
			running --;
			if( WIFEXITED( st ) )
			{
				log( "Child process (PID=%d) exited normally with status %d. %d Clients left now",
				     i, WEXITSTATUS( st ), running );
			}
			else if( WIFSIGNALED( st ) )
			{
				log( "Child process (PID=%d) killed by signal %d. %d Clients left now",
				     i, WTERMSIG( st ), running );
			}
			else
			{
				/* Should not happen AFAIK... */
				log( "Child process (PID=%d) stopped for unknown reason, %d clients left now",
				     i, running );
			}
		}
	}
	
	return( 0 );
}

settings_t *set_load( int argc, char *argv[] )
{
	settings_t *set;
	int opt, i;
	
	set = malloc( sizeof( settings_t ) );
	memset( set, 0, sizeof( settings_t ) );
	set->interface = "0.0.0.0";
	set->port = 6667;
	
	while( ( opt = getopt( argc, argv, "i:p:n:t:l:h" ) ) >= 0 )
	{
		if( opt == 'i' )
		{
			set->interface = strdup( optarg );
		}
		else if( opt == 'p' )
		{
			if( ( sscanf( optarg, "%d", &i ) != 1 ) || ( i <= 0 ) || ( i > 65535 ) )
			{
				fprintf( stderr, "Invalid port number: %s\n", optarg );
				return( NULL );
			}
			set->port = i;
		}
		else if( opt == 'n' )
		{
			if( ( sscanf( optarg, "%d", &i ) != 1 ) || ( i < 0 ) )
			{
				fprintf( stderr, "Invalid number of connections: %s\n", optarg );
				return( NULL );
			}
			set->max_conn = i;
		}
		else if( opt == 't' )
		{
			if( ( sscanf( optarg, "%d", &i ) != 1 ) || ( i < 0 ) || ( i > 128 ) )
			{
				fprintf( stderr, "Invalid number of seconds: %s\n", optarg );
				return( NULL );
			}
			set->seconds = i;
		}
		else if( opt == 'l' )
		{
			if( !( logfile = fopen( optarg, "a" ) ) )
			{
				perror( "fopen" );
				fprintf( stderr, "Error opening logfile, giving up.\n" );
				return( NULL );
			}
			setbuf( logfile, NULL );
		}
		else if( opt == 'h' )
		{
			printf( "Usage: %s [-i <interface>] [-p <port>] [-n <num>] <command> <args...>\n"
			        "A simple inetd-like daemon to have a program listening on a TCP socket without\n"
			        "needing root access to the machine\n"
			        "\n"
			        "  -i  Specify the interface (by IP address) to listen on.\n"
			        "      (Default: 0.0.0.0 (any interface))\n"
			        "  -p  Port number to listen on. (Default: 6667)\n"
			        "  -n  Maximum number of connections. (Default: 0 (unlimited))\n"
			        "  -t  Specify the maximum number of CPU seconds per process.\n"
			        "      (Default: 0 (unlimited))\n"
			        "  -l  Specify a logfile. (Default: none)\n"
			        "  -h  This information\n", argv[0] );
			return( NULL );
		}
	}
	
	if( optind == argc )
	{
		fprintf( stderr, "Missing program parameter!\n" );
		return( NULL );
	}
	
	/* The remaining arguments are the executable and its arguments */
	set->call = malloc( ( argc - optind + 1 ) * sizeof( char* ) );
	memcpy( set->call, argv + optind, sizeof( char* ) * ( argc - optind ) );
	set->call[argc-optind] = NULL;
	
	return( set );
}

void log( char *fmt, ... )
{
	va_list params;
	char line[MAX_LOG_LEN];
	time_t tm;
	int l;
	
	memset( line, 0, MAX_LOG_LEN );
	
	time( &tm );
	strcpy( line, ctime( &tm ) );
	l = strlen( line );
	line[l-1] = ' ';
	
	va_start( params, fmt );
	vsnprintf( line + l, MAX_LOG_LEN - l - 2, fmt, params );
	va_end( params );
	strcat( line, "\n" );
	
	fprintf( logfile, "%s", line );
}
