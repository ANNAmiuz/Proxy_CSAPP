#include <stdio.h>
#include "csapp.h"
#include "sbuf.h"

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400
#define MAX_CACHE MAX_CACHE_SIZE / MAX_OBJECT_SIZE

/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";
static const char *conn_close = "Connection: close\r\n";
static const char *proxy_conn_close = "Proxy-Connection: close\r\n";

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
    int used; //usage to calculate LRU
    char url_key[MAXLINE];
    char content[MAX_OBJECT_SIZE];
} cache_t;

/* CACHE LOCK */
typedef struct
{
    sem_t in;
    sem_t out;
    sem_t wlock;
    int in_cnt;
    int out_cnt;
    int wait;
} rwlock_t;

static rwlock_t rwlock;          /* global read and write lock for access CACHE */
static cache_t Cache[MAX_CACHE]; /* global CACHE */
int LRUptr = 0;

void parse_url(char *URL, url_t *url); /* parse the URL and store the info in url*/
void doit(int connfd);                 /* perform the proxy service and call the cache service*/
void init_rwlock();                    /* initialize the global read/write lock */
void init_cache();                     /* initialize the global cache */
int rcache(int connfd, char *url);     /* return 1 if there is content in cache */
void wcache(char *buf, char *url);     /* store the content from server in buf and the url to the global CACHE */
void *thread(void *vargp);
void adjust_request(url_t *u, char *new_http,
                    rio_t *rio); /* http1.1 --> http1.0... */

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
    init_cache();
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
    //printf("%s", user_agent_hdr);
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

    //printf("%s", buf);
    sscanf(buf, "%s %s %s", method, url, version);
    strcpy(url_copy, url);

    /* only serve the GET method */
    if (strcasecmp(method, "GET"))
    {
        printf(fd, method, "501", "Not Implemented",
                    "Proxy does not send this method to servers");
        return;
    }

    /* get the content from cache */
    if (rcache(fd, url_copy) == 1)
        return;

    /* parse and adjust URL from clients */
    parse_url(url_copy, &u);
    adjust_request(&u, new_http, &rio); //1.1--1.0...

    /* forward request to the target server */
    int servfd = open_clientfd(u.host, u.port);
    Rio_readinitb(&serve_rio, servfd);
    Rio_writen(servfd, new_http, strlen(new_http));

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
//implictly or explictly assign a port number
void parse_url(char *URL, url_t *url)
{
    //form 1 or form 2?
    char *hostptr = strstr(URL, "//");
    ///hub/index.html
    if (hostptr == NULL)
    {
        char *pathptr = strstr(URL, "/");
        if (pathptr != NULL)
            strcpy(url->file, pathptr);
        strcpy(url->port, "80");
    }
    //http://www.cmu.edu/hub/index.html
    else
    {
        char *portptr = strstr(hostptr+2, ":");
        //implict port
        if (portptr == NULL)
        {
            char *pathptr = strstr(hostptr + 2, "/");
            if (pathptr != NULL)
            {
                strcpy(url->file, pathptr);
                strcpy(url->port, "80");
                *pathptr = '\0';
            }
        }
        //explict port
        else
        {
            int port;
            sscanf(portptr + 1, "%d%s", &port, url->file);
            sprintf(url->port, "%d", port);
            *portptr = '\0';
        }
        strcpy(url->host, hostptr + 2);
    }
    return;
}

void adjust_request(url_t *u, char *modified_http, rio_t *rio)
{
    char request[MAXLINE], buf[MAXLINE], host[MAXLINE], endings[MAXLINE];
    sprintf(request, "GET %s HTTP/1.0\r\n", u->file);
    //continue getting the request headers, which terminates with a "\r\n"
    while (Rio_readlineb(rio, buf, MAXLINE) != 0)
    {
        if (strcmp(buf, "\r\n") == 0)
        {
            strcat(endings, "\r\n");
            break;
        }
        if (strncasecmp(buf, "host:", 5) == 0)
            strcpy(host, buf);
        if (!strncasecmp(buf, "Connection:", 11) && !strncasecmp(buf, "Proxy_Connection:", 17) && !strncasecmp(buf, "User-agent:", 11))
            strcat(endings, buf);
    }
    if (strlen(host) == 0)
        strcpy(host, u->host);
    sprintf(modified_http, "%S%S%S%S%S%S", request, host, user_agent_hdr, conn_close, proxy_conn_close, endings);
    return;
}

int rcache(int connfd, char *url)
{
    sem_wait(&rwlock.in);
    rwlock.in_cnt++;
    sem_post(&rwlock.in);

    //critical section: find the content in cache
    int found = 0;
    for (int i = 0; i < MAX_CACHE; ++i)
    {
        if (strcmp(url, Cache[i].url_key) == 0)
        {
            found = 1;
            Rio_writen(connfd, Cache[i].content, strlen(Cache[i].content));
            printf("proxy send %d bytes to client from cache.\n", strlen(Cache[i].content));
            Cache[i].used = 1;
            break;
        }
    }
    sem_wait(&rwlock.out);
    rwlock.out_cnt++;
    if (rwlock.wait == 1 && rwlock.in_cnt == rwlock.out_cnt)
        sem_post(&rwlock.wlock);
    sem_post(&rwlock.out);
    return found;
}

void wcache(char *buf, char *url)
{
    sem_wait(&rwlock.in);
    sem_post(&rwlock.out);
    if (rwlock.in_cnt == rwlock.out_cnt)
        sem_post(&rwlock.out);
    else
    {
        rwlock.wait = 1;
        sem_post(&rwlock.out);
        sem_wait(&rwlock.wlock);
        rwlock.wait = 0;
    }
    //critical section: write the content to the LRU CACHE
    while (Cache[LRUptr].used != 0)
    {
        Cache[LRUptr].used = 0;
        LRUptr = (LRUptr + 1) % MAX_CACHE;
    }
    int target = LRUptr;
    strcpy(Cache[target].url_key, url);
    strcpy(Cache[target].content, buf);
    sem_post(&rwlock.in);
    return;
}

void init_rwlock()
{
    Sem_init(&rwlock.wlock, 0, 0);
    Sem_init(&rwlock.in, 0, 1);
    Sem_init(&rwlock.out, 0, 1);
    rwlock.in_cnt = 0;
    rwlock.out_cnt = 0;
    rwlock.wait = 0;
    return;
}

void init_cache()
{
    for (int i = 0; i < MAX_CACHE; ++i)
    {
        Cache[i].used = 0;
    }
}
