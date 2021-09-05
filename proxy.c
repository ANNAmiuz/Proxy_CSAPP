#include <stdio.h>
#include "csapp.h"
#include "sbuf.h"

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400
#define MAX_CACHE 16

/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";

/* URL INFO STORAGE */
typedef struct
{
    char host[MAXLINE];
    char port[MAXLINE];
    char file[MAXLINE];
} url_t;

/* LRU CACHE */
typedef struct
{
    int occupied;
} cache_t;

/* CACHE LOCK */
typedef struct
{
    sem_t accesslock;
    sem_t wlock;
} rwlock_t;

static rwlock_t rwlock;
static sbuf_t sbuf;

void parse_url(char *URL, url_t *url); /* parse the URL and store the info in url*/
void doit(int connfd);                 /* perform the proxy service and call the cache service*/
void init_rwlock();                    /* initialize the global read/write lock */
//void init_sbuf();                    /* initialize the global sbuf */
int rcache(int connfd, char *url); /* return 1 if there is content in cache */
void wcache(char *buf, char *url); /* store the content from server in buf and the url to the global CACHE */
void *thread(void *vargp);
void adjust_http(url_t *u, char *new_http);

int main(int argc, char **argv)
{
    int listenfd, connfd, *connfdp;
    char hostname[MAXLINE], port[MAXLINE];
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;
    pthread_t tid;

    if (argc != 2)
    {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }
    init_rwlock();
    //init_sbuf();
    listenfd = Open_listenfd(argv[1]);
    while (1)
    {
        clientlen = sizeof(struct sockaddr_storage);
        connfdp = malloc(sizeof(int));
        connfd = accept(listenfd, (SA *)&clientaddr, &clientlen);
        *connfdp = connfd;
        Getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE, 0);
        printf("Accepted connection from (%s, %s)\n", hostname, port);
        Pthread_create(&tid, NULL, thread, connfdp);
    }

    printf("%s", user_agent_hdr);
    return 0;
}

void *thread(void *vargp)
{
    int connfd = *((int *)vargp);
    Pthread_detach(Pthread_self());
    free(vargp);
    doit(connfd);
    close(connfd);
    return NULL;
}

void doit(int fd)
{
    int is_static;
    struct stat sbuf;
    char buf[MAXLINE], method[MAXLINE], url[MAXLINE], version[MAXLINE];
    char filename[MAXLINE], cgiargs[MAXLINE];
    rio_t rio, serve_rio;   //for comunication with clients & server
    char url_copy[MAXLINE]; //a copy of the url
    char new_http[MAXLINE]; //1.1 -> 1.0
    url_t u;

    /* Read request line and headers */
    Rio_readinitb(&rio, fd);

    if (!Rio_readlineb(&rio, buf, MAXLINE))
        return;

    printf("%s", buf);
    sscanf(buf, "%s %s %s", method, url, version);
    strcpy(url_copy, url);

    /* only serve the GET method */
    if (strcasecmp(method, "GET"))
    {
        clienterror(fd, method, "501", "Not Implemented",
                    "Proxy does not send this method to servers");
        return;
    }

    /* get the content from cache */
    if (rcache(fd, url_copy) == 1)
        return;

    /* parse and adjust URL from clients */
    parse_url(url_copy, &u);
    adjust_http(&u, new_http); //1.1--1.0

    /* forward request to the target server */
    int servfd = open_clientfd(u.host, u.port);
    Rio_readinitb(&serve_rio, servfd);
    Rio_writen(&serve_rio, new_http, strlen(new_http));

    /* try to cache the received data from server and return them to the client*/
    char cachebuf[MAX_OBJECT_SIZE];
    int n;     //for every rio read
    int total; //the total number of reading bytes
    while ((n = Rio_readlineb(&serve_rio, buf, MAXLINE)) != 0)
    {
        total += n;
        Rio_writen(fd, buf, n); //return to the client
        strcat(cachebuf, buf);  //store in the cache
    }
    printf("Successfully send %d bytes to the client.\n", total);
    if (total <= MAX_OBJECT_SIZE)
        wcache(cachebuf, url_copy);
    close(servfd);
    return;
}

/* URL -->  host, port, file */
//form 1: http://www.cmu.edu/hub/index.html
//form 2: /hub/index.html
void parse_url(char *URL, url_t *url)
{
    
}
