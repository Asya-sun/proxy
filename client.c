#define _GNU_SOURCE
#include <stdio.h>
#include <sys/socket.h>
#include <stdlib.h>
#include <assert.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <errno.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <pthread.h>
#include <stdarg.h> 


#include "logger/logger.h"
#include "client.h"
#include "http_utils/http_utils.h"
#include "llhttp.h"

// #include "http_parser/http_request_parser.h"
// #include "picohttpparser.h"
// #include "../hashmap/hashmap.h"


#define BUFF_SIZE 256 * 8
#define START_RESPONCE_BUFF_SIZE 1024 * 1024
#define MAX_RESPONCE_BUF_SIZE 1024 * 1024 * 1024 //1 Gb
#define SERVER_STRING_SIZE 124
#define HEADERS_NUM 100
#define METHOD_MAX_LEN 16

#define TTL 360

int parse_url(const char *url, char *host, size_t max_host_len, int *port, Logger *logger) {
    memset(host, 0, max_host_len);
    *port = 80;

    if (url == NULL) {
        return -1;
    }

    // Найдем начало хоста
    const char *host_start = strstr(url, "://");
    if (host_start != NULL) {
        host_start += 3; // Пропускаем "://"
    } else {
        host_start = url; // Если "://" не найден, начинаем с начала строки
    }

    // Найдем конец хоста (найдем первый символ '/' или ':')
    const char *host_end = strpbrk(host_start, ":/");
    if (host_end == NULL) {
        host_end = host_start + strlen(host_start); // Если ничего не найдено, устанавливаем конец строки
    }

    // Выделяем память под имя хоста и копируем его
    size_t host_length = host_end - host_start;
    if (host_length > max_host_len) {
        elog(logger, ERROR, "surprisingly too long url: %.*s", host_length, host_start);
        return -1;
    }

    strncpy(host, host_start, host_length);
    host[host_length] = '\0'; // Завершаем строку

    // Если мы нашли символ ':', то это означает, что есть порт
    if (*host_end == ':') {
        *port = atoi(host_end + 1); // Преобразуем строку в число
    }

    return 0;
}

/**
//  * return 0 if success
//  * 1 if not success
 * return number of read bytes on success
 * -1 f error
 * //should i change size of buffer if message is too big?
 * //i do!
 */
int read_max_possible(int client_fd, char *buffer, size_t *buffer_size, Logger *logger) {
    size_t total_read = 0;
    size_t number_read;
    // timeout
    // \r\n\r\n - конец get
    while ((number_read = read(client_fd, buffer + total_read, *buffer_size - total_read)) > 0) {
        const size_t end_len = 4;
        const char *end = "\r\n\r\n\0";
        total_read += number_read;

        elog(logger, INFO, "read %ld bytes of request data", total_read);

        if (*buffer_size == total_read) {
            // elog(logger, ERROR, "too big message");
            // return 1;
            char *tmp = buffer;
            *buffer_size *= 2;
            buffer = realloc(buffer, *buffer_size);
            if (buffer == NULL) {
                elog(logger, ERROR, strerror(errno));
                free(tmp);
                return -1;
            }
        }

        buffer[total_read] = '\0';

        if (total_read > end_len && (strcmp(end, buffer + total_read - 4) == 0)) {
            break;
        }

        elog(logger, INFO, "waiting for next bytes (strcmp res: %d)", strcmp(end, buffer + total_read - 4));
    }

// 0 - connectio was cloxed
    elog(logger, INFO, "full request received (%ld bytes)", total_read);
    elog(logger, INFO, "total_read: %ld bytes   strlen(buffer): %ld bytes/n", total_read, strlen(buffer));

    if (number_read == -1) {
        elog(logger, ERROR, strerror(errno), NULL);
        return -1;
    }
    return total_read;
}

// int read_request(int client_fd, char *buffer, size_t *buffer_size, Logger *logger) {
//     const size_t end_len = 4;
//     const char *end = "\r\n\r\n\0";    
//     size_t total_read = 0;
//     size_t number_read; 
//     /**
//      * a litle bit of explanation
//      * if we get end of request, which is pointed by "\r\n\r\n\0", we it means we got write request
//      * otherwise, if we get out of while - read return value <=0, which means error or closed connection
//      */
//     while ((number_read = read(client_fd, buffer + total_read, *buffer_size - total_read)) > 0) {
//         total_read += number_read;
//         elog(logger, INFO, "read %ld bytes of request data", total_read);
//         if (*buffer_size == total_read) {
//             // elog(logger, ERROR, "too big message");
//             // return 1;
//             char *tmp = buffer;
//             *buffer_size *= 2;
//             buffer = realloc(buffer, *buffer_size);
//             if (buffer == NULL) {
//                 elog(logger, ERROR, strerror(errno));
//                 free(tmp);
//                 return -1;
//             }
//         }
//         buffer[total_read] = '\0';
//         if (total_read > end_len && (strcmp(end, buffer + total_read - 4) == 0)) {
//             break;
//         }
//         elog(logger, INFO, "waiting for next bytes (strcmp res: %d)", strcmp(end, buffer + total_read - 4));
//     }
// // 0 - connectio was cloxed
//     if (number_read == -1) {
//         elog(logger, ERROR, "error occurred while reading request: %s", strerror(errno), NULL);
//         return -1;
//     } else if (number_read == 0) {
//         elog(logger, ERROR, "connection was closed by the client");
//     }
//     elog(logger, INFO, "full request received (%ld bytes)", total_read);
//     elog(logger, INFO, "total_read: %ld bytes   strlen(buffer): %ld bytes/n", total_read, strlen(buffer));
//     return total_read;
// }



int read_parse_request(http_request_t *request, char **buffer, size_t *buf_size, int client_fd, Logger *logger) {
    int request_maxsize = BUFF_SIZE;
    memset(*buffer, 0, request_maxsize);

    int total_read = 0;

    while(!request->finished) {

        if (total_read == request_maxsize) {
            request_maxsize = request_maxsize * 3 / 2;
            char *tmp = realloc(*buffer, request_maxsize);
            if (tmp == NULL) {
                elog(logger, ERROR, "realloc error: %s\n", strerror(errno));
                return -1;
            } 
            *buffer = tmp;
        }

        int n = read(client_fd, *buffer + total_read, request_maxsize - total_read);
        if (n < 0) {
            elog(logger, ERROR, "error while reading request: %s\n", strerror(errno));
            return -1;
        } else if (n == 0) {
            elog(logger, ERROR, "client closed connection: %s\n", strerror(errno));
            return -1;
        }

        // if error
        if( http_request_parse(request, *buffer + total_read, n)) {
            elog(logger, ERROR, "error paring request\n");
            return -1;
        }

        total_read += n;
    }

    *buf_size = total_read;
    return 0;
}



int read_parse_responce(http_response_t *responce, char **buffer, size_t *buffer_size, int server_fd, Logger *logger) {
    int responce_maxsise = START_RESPONCE_BUFF_SIZE;
    memset(*buffer, 0, responce_maxsise);

    int total_read = 0;

    while(!responce->finished) {

        if (total_read == responce_maxsise) {
            responce_maxsise = responce_maxsise * 3 / 2;
            char *tmp = realloc(*buffer, responce_maxsise);
            if (tmp == NULL) {
                elog(logger, ERROR, "realloc error: %s\n", strerror(errno));
                return -1;
            } 
            *buffer = tmp;
        }


        int n = read(server_fd, *buffer + total_read, responce_maxsise - total_read);
        if (n < 0) {
            elog(logger, ERROR, "error while reading responce: %s\n", strerror(errno));
            return -1;
        } else if (n == 0) {
            elog(logger, ERROR, "server closed connection: %s\n", strerror(errno));
            return -1;
        }

        if (http_response_parse(responce, *buffer + total_read, n)) {
            elog(logger, ERROR, "error paring responce\n");
            return -1;
        }

        total_read += n;

    }

    *buffer_size = total_read;
    return 0;

}

int is_digit(char c) { return c >= '0' && c <= '9'; }

/**
 * return serverFD on success, -1 if not
 */
int open_connection_with_server(const char *targetHost, int port, Logger *logger) {
    elog(logger, INFO, "open_connection_with_server: targetHost: %s\n", targetHost);
    fflush(stdout);
    int err;
    int serverFD;

    struct addrinfo hints;
    struct addrinfo *result;
    struct addrinfo *rp;

    memset(&hints, 0, sizeof(hints));

    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    err = getaddrinfo(targetHost, NULL, &hints, &result);
    if (err) {
        // fprintf(stderr, "single host server: getaddrinfo: %s\n", gai_strerror(err));
        elog(logger, ERROR, "getaddrinfo: %s\n", gai_strerror(err));
        return -1;
    }

    elog(logger, INFO, "got address info\n");

    for (rp = result; rp != NULL; rp = rp->ai_next) {
        serverFD = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        elog(logger, INFO, "AFTER SOCKET\n");
        if (serverFD == -1) {
            continue;
        }
        elog(logger, INFO, "SOCKETED SUCCESSFULLY\n");

        if (rp->ai_family == AF_INET) {
            struct sockaddr_in *ipv4 = (struct sockaddr_in *)rp->ai_addr;
            ipv4->sin_port = htons(port);
        } else if (rp->ai_family == AF_INET6) {
            struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)rp->ai_addr;
            ipv6->sin6_port = htons(port);
        }

        err = connect(serverFD, rp->ai_addr, rp->ai_addrlen);
        if (err == 0) {
            elog(logger, INFO, "CONNECTED SUCCESSFULLY\n");
            freeaddrinfo(result);
            return serverFD;
        }

        elog(logger, INFO, "CONNECT FAILED\n");

        err = close(serverFD);
        if (err) {
            freeaddrinfo(result);
            return -1;
        }
    }
    fflush(stdout);
    freeaddrinfo(result);
    return -1;
}

int write_all_bytes(const char *data, int data_size, int fd, Logger *logger) {
    int wroteBytes;
    int totallyWroteBytes = 0;

    while (totallyWroteBytes != data_size) {
        // wroteBytes = send(fd, data + totallyWroteBytes,
        //                   data_size - totallyWroteBytes, MSG_NOSIGNAL);
        wroteBytes = write(fd, data + totallyWroteBytes, data_size - totallyWroteBytes);
        if (wroteBytes == -1) {
            elog(logger, ERROR,  "single host server: send request to client: write: %s\n", strerror(errno));
            return -1;
        }
        totallyWroteBytes += wroteBytes;
        elog(logger, INFO, "TOTALLY WROTE BYTES: %d OF %d PID: %ld\n",
               totallyWroteBytes, data_size, pthread_self());
    }
    return 0;
}


void split_host_port(char *addr, char *host, int *port) {
    // Находим символ ':', который разделяет хост и порт
    char *colon_position = strchr(addr, ':');
    if (colon_position != NULL) {
        // Копируем хост в указанное место
        size_t host_length = colon_position - addr;
        strncpy(host, addr, host_length);
        host[host_length] = '\0'; // завершаем строку

        // Преобразуем строку порта в целое число
        *port = atoi(colon_position + 1);
    } else {
        // Если символ ':' не найден, то устанавливаем порт в 0
        strcpy(host, addr); // Копируем весь адрес как хост
        *port = 0; // Порт не задан
    }
}


void *serving_thread_work(void *_args) {
    to_serving_args args = *((to_serving_args *) _args); //so they are on the current thread's stack now
    int client_socket = args.client_socket; 
    // struct sockaddr_in client_sockaddr = args.client_sockaddr;
    int err = 0;

    const int tid = gettid();
    char name[128];
    snprintf(name, 128, "logs/%d.log", tid);
    Logger *client_logger = createLogger(name);

    elog(client_logger, INFO, "client_socket: %d\n", client_socket);


    // int is_get_request = 0;
    // int is_cached_data = 0;
    // time_t cached_time;

    //here we read and parse request

    char *buffer = malloc(BUFF_SIZE);
    if (buffer == NULL) {
        elog(client_logger, ERROR, "%s", strerror(errno));
        // here free memory and close everything needed
        // free(request);
        closeLogger(client_logger);
        free(_args);
        close(client_socket);
        pthread_exit(NULL);
    }

    size_t buffer_size = 0;


    http_request_t request;
    http_request_init(&request);
    elog(client_logger, INFO, "client_socket: %d\n", client_socket);
    err = read_parse_request(&request,  &buffer, &buffer_size, client_socket, client_logger);
    if (err) {
        elog(client_logger, ERROR, "got error while reading or parsing request, exiting thread\n");
        // here we free resourses
        free(buffer);
        // free(request);
        closeLogger(client_logger);
        free(_args);
        close(client_socket);
        pthread_exit(NULL);
    }

    elog(client_logger, INFO, "request\nmethod : %s\nurl: %s\n \n\n", llhttp_method_name(request.method), request.url);
    // elog(client_logger, INFO, "message from client: \n%s\n", buffer);

////////////
    // int is_1_0_request = (int)(request.major == 1 && request.minor == 0);
    // int is_get = (int) (request.method == HTTP_GET);
    // // тоесть если это не 1.0 get, то все =(
    // if (!is_1_0_request || !is_get) {
    //     elog(client_logger, ERROR, "only 1.0 GET method is supported\n");
    //     // here we free what needed
    //     pthread_exit(NULL);
    // }
////////////////////////////////    

    char *responce_buffer = malloc(START_RESPONCE_BUFF_SIZE);
    if (responce_buffer == NULL) {
        elog(client_logger, ERROR, "couldn't allocate responce_buffer: %s", strerror(errno));
        // here free memory and close everything needed
        free(buffer);
        // free(request);
        closeLogger(client_logger);
        free(_args);
        close(client_socket);
        pthread_exit(NULL);
    }

    // //somewhere here should be check for cache-storing
    // if (is_get_request) {
    //     // WHAT IS KEY...
    //     // let it be buffer
    //     is_cached_data = 1 - get_entry(cache, buffer, responce_buffer, &cached_time);
    //     elog(client_logger, INFO, "is_cached_data : %d cached_time: %ld\n", is_cached_data, cached_time);
    //     if (is_cached_data && (cached_time < TTL)) {
    //         err = write_all_bytes(responce_buffer, strlen(responce_buffer), client_socket, client_logger);
    //         if (err) {
    //             elog(client_logger, ERROR, "problems writing to clienr socket: %s\n", strerror(errno));
    //             free(responce_buffer);
    //             free(buffer);
    //             free(request);
    //             closeLogger(client_logger);
    //             free(_args);
    //             close(client_socket);
    //             pthread_exit(NULL);
    //         }
    //         elog(client_logger, INFO, "wrote answer from cache\n");
    //         free(responce_buffer);
    //         free(buffer);
    //         free(request);
    //         closeLogger(client_logger);
    //         free(_args);
    //         close(client_socket);
    //         pthread_exit(NULL);
    //     }
    // }



    int server_port = 0;
    char server_host[SERVER_STRING_SIZE];
    memset(server_host, 0, SERVER_STRING_SIZE);
    elog(client_logger, INFO, "request.url: %s\n", request.url);

    err = parse_url(request.url, server_host, SERVER_STRING_SIZE, &server_port, client_logger);

    elog(client_logger, INFO, "server_host: %s server_post : %d\n", server_host, server_port);

    int server_fd = open_connection_with_server(server_host, server_port, client_logger);
    if (server_fd == -1) {
        elog(client_logger, ERROR, "couldn't create connection with server\n");
        free(responce_buffer);
        free(buffer);
        // free(request);
        closeLogger(client_logger);
        free(_args);
        close(client_socket);
        pthread_exit(0);
    }

    elog(client_logger, INFO, "strlen(buffer): %ld    buffer_size: %ld\n", strlen(buffer), buffer_size);
    err = write_all_bytes(buffer, strlen(buffer), server_fd, client_logger);
    if (err) {
        elog(client_logger, ERROR, "problems writing to server socket: %s", strerror(err));
        
        free(responce_buffer);
        free(buffer);
        // free(request);
        closeLogger(client_logger);
        free(_args);
        close(client_socket);
        pthread_exit(0);
    }
    
    
    // size_t responce_buffer_size = START_RESPONCE_BUFF_SIZE;
    // size_t received_size = 0;
    size_t responce_buffer_size = 0;

    http_response_t responce;
    http_response_init(&responce);

    elog(client_logger, INFO, "http_responce was inited\n");

    err = read_parse_responce(&responce, &responce_buffer, &responce_buffer_size, server_fd, client_logger);
    if (err) {
        elog(client_logger, ERROR, "got error while reading or parsing responce from server, exiting thread\n");
        // here we free resourses
        // free(buffer);
        // free(request);
        // closeLogger(client_logger);
        // free(_args);
        // close(client_socket);
        pthread_exit(NULL);
    }

    // //another function for body
    // // headet connection close
    // received_size = read_max_possible(server_fd, responce_buffer, &responce_buffer_size, client_logger);
    // if (received_size == -1) {
    //     elog(client_logger, ERROR, "error while reading responce");
        
    //     free(responce_buffer);
    //     free(buffer);
    //     // free(request);
    //     closeLogger(client_logger);
    //     free(_args);
    //     close(client_socket);
    //     pthread_exit(0);
    // }
    elog(client_logger, INFO, responce_buffer);

    close(server_fd);


    // сначала в кэш, потом клиентам
    // много клиентов, которые читают данные из кэша


    err = write_all_bytes(responce_buffer, responce_buffer_size, client_socket, client_logger);
    
    elog(client_logger, INFO, "strlen : %d received_size: %d\n", responce_buffer_size, responce_buffer_size);
    if (err) {
        elog(client_logger, ERROR, "problems writing to client socket");
        
        free(responce_buffer);
        free(buffer);
        // free(request);
        closeLogger(client_logger);
        free(_args);
        close(client_socket);
        pthread_exit(0);
    }

    // close(client_socket);


    //here adding some stuff to cache
    // if we are here
    // elog(client_logger, INFO, "is get request ? %d\n", is_get_request);
    // if (is_get_request) {
    //     // so such data haven't been in hash at all
    //     if (!is_cached_data) {
    //         elog(client_logger, INFO, "data is not cached so we caching then \n");
    //         err = insert_entry(cache, buffer, responce_buffer);
    //         if (err) {
    //             elog(client_logger, WARNING, "problems while trying to insert data to cache");
    //         }
    //         elog(client_logger, INFO, "data was cached \n");

    //     } else if (is_cached_data && (cached_time >= TTL)) {
    //         // so this data must be renewd
    //         elog(client_logger, INFO, "data was cached but they are too old so we renewing them\n");
    //         err = insert_replace_entry(cache, buffer, responce_buffer);
    //         if (err) {
    //             elog(client_logger, WARNING, "problems while trying to replace data in cache");
    //         }
    //         elog(client_logger, INFO, "data was recached \n");
    //     }
    // }

    free(responce_buffer);
    free(buffer);
    // free(request);
    closeLogger(client_logger);
    free(_args);
    close(client_socket);
    pthread_exit(0);
    pthread_exit(0);    

}
