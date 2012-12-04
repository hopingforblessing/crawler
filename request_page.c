#include<unistd.h>
#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<netdb.h>
#include<sys/socket.h>
#include<sys/errno.h>
#include<sys/types.h>
#include<sys/stat.h>
#include<fcntl.h>
#include<glib.h>
#include<regex.h>
#include "queue.h"
#define PAGEMAX 500000
#define FILENAMEMAX 100

#define handle_error(msg) \
	do { perror(msg); exit(EXIT_FAILURE); } while (0)
#define handle_error_en(en, msg) \
	do { errno = en; perror(msg); exit(EXIT_FAILURE); } while (0)

char page_buff[PAGEMAX]; 

/* The generate_file_name function strip out invalied characters from string pointed by hostname and copies the remaining to string pointed by filename at most size bytes including terminating null byte ('\0'). */
char * generate_file_name(const char *url, char *filename, size_t size) {
	int res, length, last;
	regex_t regex;
	regmatch_t regmatchs[1];

	res = regcomp(&regex, "[\\?/:*|<>\"]", 0);
	if(res) handle_error_en(errno, "regcomp");

	strncpy(filename, url, size);
	length = strlen(url);
	last = (length > size - 1) ? size - 1 : length;
	filename[last] = '\0';

	while(!(res = regexec(&regex, filename, 1, regmatchs, 0))) {
		filename[regmatchs[0].rm_so] = '_';
	}
	regfree(&regex);

	return filename;
}

/* Make sure hostname and path have enough size */
void get_hostname_path_by_url(const char *url, char *hostname, char *path) {
	char *s;

	s = strstr(url, "/");
	if(s) {
		int pos = s - url;
		strncpy(hostname, url, pos);
		hostname[pos] = '\0';
		strcpy(path, s);
	}
	else {
		strcpy(hostname, url);
		strcpy(path, "/\0");
	}
}

/* Scan every link in the webpage*/
void parse_web_page(queue *q_waiting_ptr, GHashTable *h_waiting_ptr, GHashTable *h_visited_ptr, const char *src) {
	int cursor, errcode, length;
	char errbuf[100], *url;
	regex_t regex;
	regmatch_t regmatchs[1];

	errcode = regcomp(&regex,
			"(http|HTTP)://[a-zA-Z0-9]+(\\.[a-zA-Z0-9+,;/?&%$#=~_-]+)+",
			REG_EXTENDED);
	if(errcode) {
		regerror(errcode, &regex, errbuf, 100);
		handle_error(errbuf);
	}

	cursor = 0;
	while(!(errcode = regexec(&regex, src+cursor, 1, regmatchs, 0))) {
		/* Throw the http:// part */
		length = regmatchs[0].rm_eo - (regmatchs[0].rm_so + 7);
		url = (char *)malloc(length + 1);

		strncpy(url, src+cursor+regmatchs[0].rm_so+7, length);
		url[length] = '\0';

		if(g_hash_table_lookup(h_visited_ptr, url) == NULL &&
			g_hash_table_lookup(h_waiting_ptr, url) == NULL) 
		{
			queue_add_last(q_waiting_ptr, url);
			g_hash_table_add(h_waiting_ptr, url);

			fprintf(stderr, "[%s] Added\n", url);
		}
		else {
			free(url);
		}
		cursor += regmatchs[0].rm_eo;
	}
	regfree(&regex);
	return;
}

/* The function get_web_page() get the web page indicated by hostname and copies to the buffer pointed by page_buff at most size bytes including a terminating null byte. The function returns the actual number of saved bytes in dest, or negative value if error occured. */
int get_web_page(const char *hostname, const char* path, char *dest, size_t size) {
	int sockfd, s, j;
	char buffer[1024];
	struct sockaddr_in server_addr;
	struct hostent *host;
	struct addrinfo hints;
	struct addrinfo *result, *rp;
	ssize_t nread;
	char *send_buff, *service = "http";
	char *http_request = "GET %s HTTP/1.0\r\nHost: %s\r\n\r\n";
	int nbytes, cursor;

	bzero(&hints, sizeof(struct addrinfo));
	hints.ai_family = AF_UNSPEC;	 /* Allow IPv4 or IPv6 */
	hints.ai_socktype = SOCK_STREAM; /* Stream socket */
	hints.ai_flags = 0;
	hints.ai_protocol = 0;		 /* Any protocol */

	s = getaddrinfo(hostname, service, &hints, &result);
	if(s != 0) {
		fprintf(stderr, "Could not resolve [%s]: %s\n",
				hostname, gai_strerror(s));
		return -1;
	}
	for(rp = result; rp != NULL; rp = rp->ai_next) {
		sockfd = socket(rp->ai_family,
				rp->ai_socktype,
				rp->ai_protocol);
		if(sockfd == -1)
			continue;
		if(connect(sockfd, rp->ai_addr, rp->ai_addrlen) != -1)
			break;

		close(sockfd);
	}
	if (rp == NULL) {
		fprintf(stderr, "Could not connect\n");
		return -2;
	}

	freeaddrinfo(result);

	/*
	 * if((host = (struct hostent*)gethostbyname(hostname)) == NULL) {
	 fprintf(stderr,"Gethostname error\n");
	 return -1; * get host name error *
	 }
	 if((sockfd=socket(AF_INET, SOCK_STREAM, 0))==-1)
	 {
	 fprintf(stderr, "Socket Error:%s\a\n", strerror(errno));
	 return -2; * socket error *
	 }
	 bzero(&server_addr, sizeof(server_addr));
	 server_addr.sin_family = AF_INET;
	 server_addr.sin_port = htons(portnumber);
	 server_addr.sin_addr = *((struct in_addr *)host->h_addr_list[0]);
	 if(connect(sockfd, (struct sockaddr *)(&server_addr), sizeof(struct sockaddr)) == -1) {
	 fprintf(stderr, "Connection Error:%s\a\n", strerror(errno));
	 return -3; * connection error *
	 }
	 */
	send_buff = (char *)malloc(strlen(http_request) + 
			strlen(hostname) + strlen(path) + 1);
	sprintf(send_buff, http_request, path, hostname);

	if(send(sockfd, send_buff, strlen(send_buff), 0) < 0) {
		fprintf(stderr, "Sending Error:%s\a\n",
				strerror(errno));
		free(send_buff);

		return -2; /*sending error */
	}
	free(send_buff);

	cursor = 0;
	fprintf(stderr, "Reading From [%s%s]... ", hostname, path);
	while(1) {
		nbytes = recv(sockfd, buffer, 1024, 0);
		if(nbytes == -1) {
			fprintf(stderr,"Error: %s\n", strerror(errno));
			break;
		}
		if(nbytes == 0) {
			fprintf(stderr,"%s\n", strerror(errno));
			break;
		}
		/* why needed? losing data?
		   if(nbytes < 1024)
		   buffer[nbytes] = '\0';
		   */
		if(cursor + nbytes > size - 1) {
			fprintf(stderr, "Rest cut [%s]\n", hostname);

			nbytes = size - 1 - cursor;
			strncpy(dest+cursor, buffer, nbytes);
			cursor += nbytes;
			break;
		}
		strncpy(dest+cursor, buffer, nbytes);
		cursor += nbytes;
	}
	dest[cursor] = '\0';
	close(sockfd);
	return cursor;
}

/* Function provided to g_hash_table_init to free the memory allocated to key when destroying table or removing from table */
void key_destroy_func(gpointer data) {
	free(data);
}

/* The function will turn url to reasonable filename, get rid of http header from webpage and save the webpage under the filename into directory indicated by folder */
int save_webpage_to_file(const char* folder, const char *url,
				const char *webpage, size_t page_size) {
	int fd, folder_len, pos;
	char *filename, *s;


	s = strstr(webpage, "\r\n\r\n");
	if(s) {
		pos = s - webpage + 4;
	}
	else {
		pos = 0;
	}

	folder_len = strlen(folder);
	filename = malloc(FILENAMEMAX+folder_len);
	strncpy(filename, folder, folder_len);
	generate_file_name(url, filename+folder_len, FILENAMEMAX);

	if((fd=open(filename, O_CREAT|O_EXCL|O_WRONLY, 0644)) == -1) {
		fprintf(stderr, "Open Error:%s\n", strerror(errno));
		free(filename);
		return -1;
	}
	if(write(fd, webpage+pos, page_size-pos) == -1) {
		fprintf(stderr, "Write Error:%s\n", strerror(errno));
		free(filename);
		close(fd);
		return -2;
	}
}

/* Function for crawling the web starting at seed. Notice: GHashTable will free the memory allocated to the key */
int crawl(const char *seed, size_t max_waiting, size_t max_total) {

	int seed_len, url_len, page_size;
	char *folder, *seed_m, *url, *hostname, *path;
	queue *q_waiting_ptr;
	GHashTable *h_waiting_ptr, *h_visited_ptr;

	if(mkdir(seed, 0744) == -1) handle_error_en(errno, "mkdir");

	seed_len = strlen(seed);
	folder = (char *)malloc(seed_len+2);
	if(folder == NULL) handle_error("malloc folder");

	strcpy(folder, seed);
	folder[seed_len] = '/';
	folder[seed_len+1] = '\0';

	/* Queue of url waiting to be visited */
	q_waiting_ptr = queue_init();
	/* Hashtable of url waiting to be visited, for fast querying */
	h_waiting_ptr = g_hash_table_new_full(
			g_str_hash, g_str_equal,
			key_destroy_func, NULL);
	/* Hashtable of visited list */
	h_visited_ptr = g_hash_table_new_full(
			g_str_hash, g_str_equal,
			key_destroy_func, NULL);

	seed_m = strdup(seed);
	if(seed_m == NULL) handle_error("strdup");

	queue_add_last(q_waiting_ptr, seed_m);
	g_hash_table_add(h_waiting_ptr, seed_m);

	while(q_waiting_ptr->size > 0) {
		/* Get length of next url to allocate memory */
		url_len = queue_get_first_size(q_waiting_ptr);

		url = (char *)malloc(url_len + 1);
		queue_remove_first(q_waiting_ptr, url, url_len + 1);
		g_hash_table_remove(h_waiting_ptr, url);

		hostname = (char *)malloc(url_len + 1);
		path = (char *)malloc(url_len + 1);
		get_hostname_path_by_url(url, hostname, path);

		page_size = get_web_page(hostname,path,page_buff,PAGEMAX);

		free(hostname);
		free(path);

		if(page_size < 0) {
			free(url);
			continue;
		}

		if(save_webpage_to_file(folder, url,
					page_buff, page_size) < 0) {
			free(url);
			continue; /* failure */
		}

		/* Adds url to visited table */
		g_hash_table_add(h_visited_ptr, url);

		/* Extract url from page */
		parse_web_page(q_waiting_ptr, h_waiting_ptr,
				h_visited_ptr, page_buff);

		fprintf(stderr, "Waiting:[%d]\tVisited:[%d]\n", 
				q_waiting_ptr->size,
				g_hash_table_size(h_visited_ptr));
	}

	/* free(seed) not needed, visited hash table will free it */
	free(folder);
	
	queue_destroy(q_waiting_ptr);
	g_hash_table_destroy(h_waiting_ptr);
	g_hash_table_destroy(h_visited_ptr);

	return 0;
}

/* Starting Point */
int main(int argc, char *argv[]) {

	if(argc != 2) {
		fprintf(stderr, "Usage: %s URL(without protocol)\a\n",
				argv[0]);
		exit(EXIT_FAILURE);
	}

	crawl(argv[1], 0, 0);

	exit(EXIT_SUCCESS);
}
