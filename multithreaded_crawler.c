#include <stdio.h>
#include <stdlib.h>
#ifndef WIN32
#include <unistd.h>
#endif
#include <curl/multi.h>
#include <queue.h>
#include <search.h>

#include <libxml/HTMLparser.h>
#include <libxml/parser.h>
#include <libxml/xpath.h>
#include <libxml/uri.h>

#define ECE252_HEADER "X-Ece252-Fragment: "
#define BUF_SIZE 1048576  /* 1024*1024 = 1M */
#define BUF_INC  524288   /* 1024*512  = 0.5M */

#define CT_PNG  "image/png"
#define CT_HTML "text/html"
#define CT_PNG_LEN  9
#define CT_HTML_LEN 9

typedef struct recv_buf2 {
    char *buf;       /* memory to hold a copy of received data */
    size_t size;     /* size of valid data in buf in bytes*/
    size_t max_size; /* max capacity of buf in bytes*/
    int seq;         /* >=0 sequence number extracted from http header */
                     /* <0 indicates an invalid seq number */
} RECV_BUF;

#define max(a, b) \
   ({ __typeof__ (a) _a = (a); \
       __typeof__ (b) _b = (b); \
     _a > _b ? _a : _b; })




#define MAX_WAIT_MSECS 30*1000 /* Wait max. 30 seconds */

char * freeArray[1000];
int freeArraySize = 0;
int filled = 0;

//init the queue
int max_connections = 1;
int num_images = 50;
char * filepath = NULL; 
RECV_BUF * recv_buf_array;
char ** url_array;




htmlDocPtr mem_getdoc(char *buf, int size, const char *url)
{
    int opts = HTML_PARSE_NOBLANKS | HTML_PARSE_NOERROR | \
               HTML_PARSE_NOWARNING | HTML_PARSE_NONET;
    htmlDocPtr doc = htmlReadMemory(buf, size, url, NULL, opts);
    
    if ( doc == NULL ) {
        fprintf(stderr, "Document not parsed successfully.\n");
        return NULL;
    }
    return doc;
}

xmlXPathObjectPtr getnodeset (xmlDocPtr doc, xmlChar *xpath)
{
	
    xmlXPathContextPtr context;
    xmlXPathObjectPtr result;

    context = xmlXPathNewContext(doc);
    if (context == NULL) {
        printf("Error in xmlXPathNewContext\n");
        return NULL;
    }
    result = xmlXPathEvalExpression(xpath, context);
    xmlXPathFreeContext(context);
    if (result == NULL) {
        printf("Error in xmlXPathEvalExpression\n");
        return NULL;
    }
    if(xmlXPathNodeSetIsEmpty(result->nodesetval)){
        xmlXPathFreeObject(result);
        // printf("No result\n");
        return NULL;
    }
    return result;
}

int find_http(char *buf, int size, int follow_relative_links, const char *base_url, QUEUE * queue)
{

    int i;
    htmlDocPtr doc;
    xmlChar *xpath = (xmlChar*) "//a/@href";
    xmlNodeSetPtr nodeset;
    xmlXPathObjectPtr result;
    xmlChar *href;
		
    if (buf == NULL) {
        return 1;
    }
    
    
    doc = mem_getdoc(buf, size, base_url);
    
    result = getnodeset (doc, xpath);
        if (result) {
            nodeset = result->nodesetval;
            for (i=0; i < nodeset->nodeNr; i++) {
                href = xmlNodeListGetString(doc, nodeset->nodeTab[i]->xmlChildrenNode, 1);
                if ( follow_relative_links ) {
                    xmlChar *old = href;
                    href = xmlBuildURI(href, (xmlChar *) base_url);
                    xmlFree(old);
                }
                if ( href != NULL && !strncmp((const char *)href, "http", 4) ) {


                    //push non visited
                    char * string = strdup(href);
                    ENTRY e;
                    e.key = string; //lol wtf is this
                    e.data = 1;
                    
                    if(hsearch(e, FIND) == NULL) {
                        //keep track of keys to free LOL
                        freeArray[freeArraySize] = string;
                        freeArraySize++;
                        push(queue, (char *) href);
                        hsearch(e, ENTER);
                    }
                    else{
                        free(string);
                    }
                }
                xmlFree(href);
            }
            xmlXPathFreeObject (result);
        }
    

    xmlFreeDoc(doc);
    
    return 0;
}
/**
 * @brief  cURL header call back function to extract image sequence number from 
 *         http header data. An example header for image part n (assume n = 2) is:
 *         X-Ece252-Fragment: 2
 * @param  char *p_recv: header data delivered by cURL
 * @param  size_t size size of each memb
 * @param  size_t nmemb number of memb
 * @param  void *userdata user defined data structurea
 * @return size of header data received.
 * @details this routine will be invoked multiple times by the libcurl until the full
 * header data are received.  we are only interested in the ECE252_HEADER line 
 * received so that we can extract the image sequence number from it. This
 * explains the if block in the code.
 */
size_t header_cb_curl(char *p_recv, size_t size, size_t nmemb, void *userdata)
{
    
    int realsize = size * nmemb;
    RECV_BUF *p = userdata;
    
#ifdef DEBUG1_
    printf("%s", p_recv);
#endif /* DEBUG1_ */
    if (realsize > strlen(ECE252_HEADER) &&
	strncmp(p_recv, ECE252_HEADER, strlen(ECE252_HEADER)) == 0) {

        /* extract img sequence number */
	p->seq = atoi(p_recv + strlen(ECE252_HEADER));
    printf("actual sequence number %d \n", p->seq);
    }
    return realsize;
}


/**
 * @brief write callback function to save a copy of received data in RAM.
 *        The received libcurl data are pointed by p_recv, 
 *        which is provided by libcurl and is not user allocated memory.
 *        The user allocated memory is at p_userdata. One needs to
 *        cast it to the proper struct to make good use of it.
 *        This function maybe invoked more than once by one invokation of
 *        curl_easy_perform().
 */

size_t write_cb_curl3(char *p_recv, size_t size, size_t nmemb, void *p_userdata)
{
   
    size_t realsize = size * nmemb;
    RECV_BUF *p = (RECV_BUF *)p_userdata;
 
    if (p->size + realsize + 1 > p->max_size) {/* hope this rarely happens */ 
        /* received data is not 0 terminated, add one byte for terminating 0 */
        size_t new_size = p->max_size + max(BUF_INC, realsize + 1);   
        char *q = realloc(p->buf, new_size);
        if (q == NULL) {
            perror("realloc"); /* out of memory */
            return -1;
        }
        p->buf = q;
        p->max_size = new_size;
    }
    
    memcpy(p->buf + p->size, p_recv, realsize); /*copy data from libcurl*/
    
    p->size += realsize;
    p->buf[p->size] = 0;

    return realsize;
}


int recv_buf_init(RECV_BUF *ptr, size_t max_size)
{
    void *p = NULL;
    
    if (ptr == NULL) {
        return 1;
    }

    p = malloc(max_size);
    if (p == NULL) {
	return 2;
    }
    
    ptr->buf = p;
    ptr->size = 0;
    ptr->max_size = max_size;
    ptr->seq = -1;              /* valid seq should be positive */
    return 0;
}

int recv_buf_cleanup(RECV_BUF *ptr)
{
    if (ptr == NULL) {
	return 1;
    }
    
    free(ptr->buf);
    ptr->size = 0;
    ptr->max_size = 0;
    return 0;
}

void cleanup(CURL *curl, RECV_BUF *ptr)
{
        // curl_easy_cleanup(curl);
        
        recv_buf_cleanup(ptr);
}
/**
 * @brief output data in memory to a file
 * @param path const char *, output file path
 * @param in  void *, input data to be written to the file
 * @param len size_t, length of the input data in bytes
 */

int write_file(const char *path, const void *in, size_t len)
{
    FILE *fp = NULL;

    if (path == NULL) {
        fprintf(stderr, "write_file: file name is null!\n");
        return -1;
    }

    if (in == NULL) {
        fprintf(stderr, "write_file: input data is null!\n");
        return -1;
    }

    // fp = fopen(path, "wb");
    // if (fp == NULL) {
    //     perror("fopen");
    //     return -2;
    // }

    // if (fwrite(in, 1, len, fp) != len) {
    //     fprintf(stderr, "write_file: imcomplete write!\n");
    //     return -3; 
    // }
    // fclose(fp);
    return 1;
}

/**
 * @brief create a curl easy handle and set the options.
 * @param RECV_BUF *ptr points to user data needed by the curl write call back function
 * @param const char *url is the target url to fetch resoruce
 * @return a valid CURL * handle upon sucess; NULL otherwise
 * Note: the caller is responsbile for cleaning the returned curl handle
 */

// static void init(CURLM *cm, int i)
// {
//   CURL *eh = curl_easy_init();
//   curl_easy_setopt(eh, CURLOPT_WRITEFUNCTION, header_cb_curl);
//   curl_easy_setopt(eh, CURLOPT_HEADER, 0L);
//   curl_easy_setopt(eh, CURLOPT_URL, urls[i]);
//   curl_easy_setopt(eh, CURLOPT_PRIVATE, urls[i]);
//   curl_easy_setopt(eh, CURLOPT_VERBOSE, 0L);
//   curl_multi_add_handle(cm, eh);
// }
CURL *easy_handle_init(RECV_BUF *ptr, char *url, CURLM * cm, int i) {
    CURL *curl_handle = NULL;

    if ( ptr == NULL || url == NULL) {
        return NULL;
    }
    /* init user defined call back function buffer */
    if ( recv_buf_init(ptr, BUF_SIZE) != 0 ) {
        return NULL;
    }

    /* init a curl session */
    curl_handle = curl_easy_init();

    if (curl_handle == NULL) {
        fprintf(stderr, "curl_easy_init: returned NULL\n");
        return NULL;
    }

    /* specify URL to get */
 
    curl_easy_setopt(curl_handle, CURLOPT_URL, url);
    
    /* register write call back function to process received data */
    curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, write_cb_curl3); 
    /* user defined data structure passed to the call back function */
    curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)ptr);

    /* register header call back function to process received header data */
    curl_easy_setopt(curl_handle, CURLOPT_HEADERFUNCTION, header_cb_curl); 
    /* user defined data structure passed to the call back function */
    curl_easy_setopt(curl_handle, CURLOPT_HEADERDATA, (void *)ptr);

    /* some servers requires a user-agent field */
    curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "ece252 lab4 crawler");

    /* follow HTTP 3XX redirects */
    curl_easy_setopt(curl_handle, CURLOPT_FOLLOWLOCATION, 1L);
    /* continue to send authentication credentials when following locations */
    curl_easy_setopt(curl_handle, CURLOPT_UNRESTRICTED_AUTH, 1L);
    /* max numbre of redirects to follow sets to 5 */
    curl_easy_setopt(curl_handle, CURLOPT_MAXREDIRS, 5L);
    /* supports all built-in encodings */ 
    curl_easy_setopt(curl_handle, CURLOPT_ACCEPT_ENCODING, "");

    /* Max time in seconds that the connection phase to the server to take */
    //curl_easy_setopt(curl_handle, CURLOPT_CONNECTTIMEOUT, 5L);
    /* Max time in seconds that libcurl transfer operation is allowed to take */
    //curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT, 10L);
    /* Time out for Expect: 100-continue response in milliseconds */
    //curl_easy_setopt(curl_handle, CURLOPT_EXPECT_100_TIMEOUT_MS, 0L);

    /* Enable the cookie engine without reading any initial cookies */
    curl_easy_setopt(curl_handle, CURLOPT_COOKIEFILE, "");
    /* allow whatever auth the proxy speaks */
    curl_easy_setopt(curl_handle, CURLOPT_PROXYAUTH, CURLAUTH_ANY);
    /* allow whatever auth the server speaks */
    curl_easy_setopt(curl_handle, CURLOPT_HTTPAUTH, CURLAUTH_ANY);


    curl_easy_setopt(curl_handle, CURLOPT_PRIVATE, i);


   


    return curl_handle;
}

int process_html(CURL *curl_handle, RECV_BUF *p_recv_buf, QUEUE * queue)
{
    char fname[256];
    int follow_relative_link = 1;
    char *url = NULL; 
    pid_t pid =getpid();
    
    curl_easy_getinfo(curl_handle, CURLINFO_EFFECTIVE_URL, &url);
    find_http(p_recv_buf->buf, p_recv_buf->size, follow_relative_link, url, queue);
    
    sprintf(fname, "./output_%d.html", pid);
    
    return write_file(fname, p_recv_buf->buf, p_recv_buf->size);
}

int process_png(CURL *curl_handle, RECV_BUF *p_recv_buf)
{
   
    pid_t pid =getpid();
    char fname[256];
    char *eurl = NULL;          /* effective URL */
    curl_easy_getinfo(curl_handle, CURLINFO_EFFECTIVE_URL, &eurl);
    if ( eurl != NULL) {
        // printf("The PNG url is: %s\n", eurl);
    }
    
    
    sprintf(fname, "./output_%d_%d.png", p_recv_buf->seq, pid);
    return write_file(fname, p_recv_buf->buf, p_recv_buf->size);
}
/**
 * @brief process teh download data by curl
 * @param CURL *curl_handle is the curl handler
 * @param RECV_BUF p_recv_buf contains the received data. 
 * @return 0 on success; non-zero otherwise
 */

int process_data(CURL *curl_handle, RECV_BUF *p_recv_buf, QUEUE * queue)
{
    CURLcode res;
    char fname[256];
    pid_t pid =getpid();
    long response_code;
    
    res = curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &response_code);
    if ( res == CURLE_OK ) {
	    // printf("Response code: %ld\n", response_code);
    }

    if ( response_code >= 400 ) { 
    	// fprintf(stderr, "Error.\n");
        return 1;
    }

    char *ct = NULL;
   
    res = curl_easy_getinfo(curl_handle, CURLINFO_CONTENT_TYPE, &ct);
    if ( res == CURLE_OK && ct != NULL ) {
    	// printf("Content-Type: %s, len=%ld\n", ct, strlen(ct));
    } else {
        fprintf(stderr, "Failed obtain Content-Type\n");
        return 2;
    }
    
    if ( strstr(ct, CT_HTML) ) {
        return process_html(curl_handle, p_recv_buf, queue);
    } else if ( strstr(ct, CT_PNG) ) {
        return process_png(curl_handle, p_recv_buf);
    } else {
        sprintf(fname, "./output_%d", pid);
    }
    

    return write_file(fname, p_recv_buf->buf, p_recv_buf->size);
}



int main(int argc, const char ** argv)
{
    CURLM *cm=NULL;
    CURL *eh=NULL;
    CURLMsg *msg=NULL;
    CURLcode return_code=0;
    int still_running=0, i=0, msgs_left=0;
    int http_status_code;
  
    hcreate(1000);
    
    curl_global_init(CURL_GLOBAL_ALL);
    
    cm = curl_multi_init();

    //curl on the queue
    QUEUE * queue = malloc(sizeof(QUEUE));

    //parse arguments
    int opt;
    xmlInitParser();
    
    while((opt = getopt(argc, argv, "t:m:v:")) != -1)
    {
        switch(opt)
        {
            case 't':
                max_connections =  strtoul(optarg, NULL, 10);
            case 'm':
                num_images = strtoul(optarg, NULL, 10);
            case 'v':
                filepath = optarg;
        }
    }
    FILE * file = fopen("png_urls.txt", "w");
    fclose(file);
    if(filepath != NULL) {
        file = fopen(filepath, "w");
        fclose(file);
    }
    
    
    
    //keep a track of keys for hashing
 
    //push the initial url
    init(queue);
    push(queue, argv[argc - 1]);

    ENTRY e;
    char * temp =  strdup(argv[argc - 1]);
    e.key = temp; //lol wtf is this
    e.data = 1;
    hsearch(e, ENTER);
    freeArray[freeArraySize] = temp;
    freeArraySize++;
    printf("%d \n", max_connections);
    recv_buf_array = malloc(BUF_SIZE * max_connections);
    url_array = malloc(sizeof(void *) * max_connections);
    for(int i = 0; i < max_connections; i++) {
        url_array[i] = malloc(sizeof(char) * 256);
    }

   
    struct timeval start;
    gettimeofday(&start, NULL);
    double startTime = start.tv_sec + start.tv_usec / 1000000.;

    //loop the curl to keep on going until either it meets the image count or all sockets are closed 
    do{
        
        // curl_multi_perform(cm, &still_running);
        CURL * res;
        
        for(int i = 0; i < max_connections; i++) {
            //pop off the queue
            
            
            char url[256];
            int status;
            status = pop(queue, url);
            
            if(status == -1){
                continue;
            }
            else{
                
                res = easy_handle_init(&(recv_buf_array[i]), url, cm, i); // store some kind of private index
                curl_multi_add_handle(cm, res);
                
                strcpy(url_array[i], url);
                // curl_handle_array[i] = res;
            }
        }
printf("hello \n");
        do {
            
            int numfds=0;
            int res = curl_multi_wait(cm, NULL, 0, MAX_WAIT_MSECS, &numfds);
            
            if(res != CURLM_OK) {
                fprintf(stderr, "error: curl_multi_wait() returned %d\n", res);
                return EXIT_FAILURE;
            }
            /*
            if(!numfds) {
                fprintf(stderr, "error: curl_multi_wait() numfds=%d\n", numfds);
                return EXIT_FAILURE;
            }
            */
            curl_multi_perform(cm, &still_running);

        } while(still_running);
        
        while ((msg = curl_multi_info_read(cm, &msgs_left))) {
            
            if (msg->msg == CURLMSG_DONE) {
                eh = msg->easy_handle;     
                int index;
                curl_easy_getinfo(eh, CURLINFO_PRIVATE, &index);
                return_code = msg->data.result;
                if(return_code!=CURLE_OK) {
                    fprintf(stderr, "CURL error code: %d\n", msg->data.result);
                    
                }
                else{
                
                    // Get HTTP status code
                    http_status_code = 0;
                    curl_easy_getinfo(eh, CURLINFO_RESPONSE_CODE, &http_status_code);
                    if(http_status_code==200) {
                        

                        // printf("200 OK \n");
                        //parse data and add it to the queue
                        
                        // printf("%x \n",(unsigned char) recv_buf_array[index] -> buf[0]); //error line
                        if(filled >= num_images) {
                            cleanup(eh, &recv_buf_array[index]);
                            curl_multi_remove_handle(cm, eh);
                            curl_easy_cleanup(eh);
                            continue;
                        }
                        if( 
                            recv_buf_array[index].seq != -1 &&
                            (unsigned char) recv_buf_array[index].buf[0] == 0x89 &&
                            (unsigned char) recv_buf_array[index].buf[1] == 'P'  &&
                            (unsigned char) recv_buf_array[index].buf[2] == 'N'  &&
                            (unsigned char) recv_buf_array[index].buf[3] == 'G'  &&
                            (unsigned char) recv_buf_array[index].buf[4] == 0x0D &&
                            (unsigned char) recv_buf_array[index].buf[5] == 0x0A &&
                            (unsigned char) recv_buf_array[index].buf[6] == 0x1A &&
                            (unsigned char) recv_buf_array[index].buf[7] == 0x0A) {
                            
                                
                                //write down the url for the png
                                FILE * file = fopen("png_urls.txt", "a");
                                fseek(file, 0, SEEK_END);
                                // fwrite(dest, 1, dest_len, allIDAT)
                                // char url[256];
                                
                                fprintf(file, "%s \n", url_array[index]);
                                fclose(file);    
                                filled = filled + 1;
                                
     
                        }
                        
                        if(filepath != NULL) {
                            //make this a conditional write if a logfile is passed over
                            FILE * file = fopen(filepath, "a");
                            
                            fseek(file, 0, SEEK_END);
                            // fwrite(dest, 1, dest_len, allIDAT);
                            
                            // char url[256];
                            // curl_easy_getinfo(eh, CURLOPT_URL, url);
                            // printf("url 3 %s \n", url);

                            printf("%d \n", index);
                            printf("%s \n", url_array[index]);
                            fprintf(file, "%s \n", url_array[index]);
                            fclose(file);
                        }

                        process_data(eh, &recv_buf_array[index], queue);
                   
                        //check the data if its a valid png
                        //if it was then increment filled images
                        //add it to the pages of travelled images

                    } else {
                        printf("http_status_code %d\n", http_status_code);

                    }
                    

                }

                cleanup(eh, &recv_buf_array[index]);
                curl_multi_remove_handle(cm, eh);
                curl_easy_cleanup(eh);
                
                
                    
                // free(recv_buf_array[index] -> buf);
                // free(recv_buf_array[index]);
        

                //subtract numfds


            }
            else {
                fprintf(stderr, "error: after curl_multi_info_read(), CURLMsg=%d\n", msg->msg);
            }
        }
        

    } while((getItems(queue) > 0) && (filled < num_images));

    struct timeval end;
    gettimeofday(&end, NULL);
    double endTime = end.tv_sec +end.tv_usec / 1000000.;
    printf("findpng2 execution time: %.6lf \n", endTime - startTime);

    //clean hashmap data
   
    for(int i = 0; i < freeArraySize; i++) {
        free(freeArray[i]);
    }
    hdestroy();
    
    //deallocate any memory
    free(queue);
    free(recv_buf_array);
    for(int i = 0; i < max_connections; i++) {
        free(url_array[i]);
    }
    free(url_array);
    //i think this handles pending requests
    curl_multi_cleanup(cm);

    xmlCleanupParser();
    curl_global_cleanup();

}
